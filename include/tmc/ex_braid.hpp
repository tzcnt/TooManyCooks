#pragma once
#ifdef TMC_USE_MUTEXQ
#include "tmc/detail/qu_mutex.hpp"
#else
#include "tmc/detail/qu_lockfree.hpp"
#endif
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <limits>
#include <memory>

#if !defined(TMC_PRIORITY_COUNT) || (TMC_PRIORITY_COUNT > 1)
// braid_work_item's purpose is to save and restore priorities for work items.
// if there is only one priority level, we don't need this.
#define USE_BRAID_WORK_ITEM
#endif

namespace tmc {
struct aw_braid_enter;
struct aw_braid_exit;
class ex_braid {
  friend struct aw_braid_enter;
  friend struct aw_braid_exit;
#ifdef USE_BRAID_WORK_ITEM
  struct braid_work_item {
    work_item work;
    // braid doesn't have priority queues. each task must save its own priority
    size_t priority;
  };
  // Just wrap It into a braid_work_item. Every item has the same prio.
  template <typename It> struct bulk_prio_iterator {
  private:
    It wrapped;
    size_t prio;

  public:
    bulk_prio_iterator(It wrapped_in, size_t prio_in)
        : wrapped(wrapped_in), prio{prio_in} {}

    braid_work_item operator*() const
      requires(
          std::is_convertible_v<decltype(*wrapped), std::coroutine_handle<>>)
    {
      return braid_work_item{std::coroutine_handle<>(*wrapped), prio};
    }
    braid_work_item operator*() const
      requires(
          !std::is_convertible_v<decltype(*wrapped), std::coroutine_handle<>>)
    {
      return braid_work_item{*wrapped, prio};
    }
    auto &operator++() {
      ++wrapped;
      return *this;
    }
  };
  using braid_work_item_t = braid_work_item;
#else
  using braid_work_item_t = work_item;
#endif

#ifdef TMC_USE_MUTEXQ
  using task_queue_t = detail::MutexQueue<braid_work_item_t>;
#else
  using task_queue_t = tmc::queue::ConcurrentQueue<braid_work_item_t>;
#endif

  task_queue_t queue;

  // A braid may be destroyed while tasks are enqueued on its parent executor
  // that would try to access it. Use a shared_ptr to a lock to prevent this. If
  // the braid is destroyed, this ptr will outlive the braid, and the lock will
  // be left locked, which will keep out any remaining accesses.
  std::shared_ptr<tiny_lock> lock;

  // It is also trivial to destroy a braid while executing its runloop:
  // ```
  // tmc::task<void> destroy_running_braid() {
  //   tmc::ex_braid b;
  //   b.enter();
  // }
  // ```
  // b is destroyed at the end of scope, but after returning, we will be in the
  // middle of `b.try_run_loop()` Thus, a copy of this pointer is used to
  // communicate between the destructor and try_run_loop. It is non-atomic, as
  // it is only w->r by one thread.
  bool *destroyed_by_this_thread;

  std::atomic<size_t> never_yield;
  detail::type_erased_executor type_erased_this;
  detail::running_task_data stored_context;

  /// The main loop of the braid; only 1 thread is allowed to enter the inner
  /// loop. If the lock is already taken, other threads will return immediately.
  tmc::task<void> try_run_loop(std::shared_ptr<tiny_lock> this_braid_lock,
                               bool *this_thread_destroyed);

  /// Called after getting the lock. Update the thread locals so that spawn()
  /// will create tasks on the braid, and yield_requested() always returns
  /// false.
  void enter_context();

  /// Called before releasing the lock. Reset the thread locals to what they
  /// were before calling enter_context().
  void exit_context();

public:
  /// Submits a single work_item to the braid, and attempts to take the lock and
  /// start executing tasks on the braid. Rather than calling this directly, it
  /// is recommended to use the `tmc::post()` free function template.
  void post_variant(work_item &&item, size_t prio);

  /// Submits `count` items to the braid, and attempts to take the lock and
  /// start executing tasks on the braid. `It` is expected to be an iterator
  /// type that implements `operator*()` and `It& operator++()`.
  template <typename It> void post_bulk(It items, size_t prio, size_t count) {
#ifdef USE_BRAID_WORK_ITEM
    queue.enqueue_bulk(bulk_prio_iterator<It>(items, prio), count);
#else
    queue.enqueue_bulk(items, count);
#endif
    if (!lock->is_locked()) {
      type_erased_this.parent->post_variant(
          std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
          prio);
    }
  }

  /// Implements `tmc::TypeErasableExecutor` concept, but unlikely to be needed
  /// directly by users.
  inline detail::type_erased_executor *type_erased() {
    return &type_erased_this;
  }

private:
  ex_braid(detail::type_erased_executor *parent);

public:
  /// Construct a braid with the current executor as its parent. It is an error
  /// to call this from a thread that is not running on an executor.
  ex_braid();

  /// Construct a braid with the specified executor as its parent.
  template <typename Executor>
  ex_braid(Executor &parent) : ex_braid(parent.type_erased()) {}
  ~ex_braid();
  /// Returns an awaitable that suspends the current task and resumes it in the
  /// braid context. It may be resumed on a different thread than the one
  /// calling enter(). Can also be thought of as an async `lock()`. This is
  /// idempotent, and is semantically identical to `co_await resume_on(braid);`
  aw_braid_enter enter();
  /// Returns an awaitable that suspends the current task and resumes it back on
  /// the braid's parent executor. Can also be thought of as an async
  /// `unlock()`. This is idempotent and semantically identical to
  /// `co_await resume_on(braid.parent);`. It is not always necessary to call
  /// `exit()`, as at the end of the coroutine where `enter()` was called, the
  /// continuation coroutine will be resumed on whatever executor it was
  /// previously scheduled to run on.
  aw_braid_exit exit();

private:
  ex_braid &operator=(const ex_braid &other) = delete;
  ex_braid(const ex_braid &other) = delete;
  // not movable due to this_executor pointer being accessible by child threads
  ex_braid &operator=(const ex_braid &&other) = delete;
  ex_braid(const ex_braid &&other) = delete;
};

/// The awaitable type returned by `tmc::ex_braid::enter()`.
class [[nodiscard("You must co_await aw_braid_enter for it to have any "
                  "effect.")]] aw_braid_enter {
  friend class ex_braid;
  ex_braid &s;
  aw_braid_enter(ex_braid &s_in) : s(s_in) {}

public:
  /// Always suspends.
  constexpr bool await_ready() {
    // always have to suspend here, even if we can get the lock right away
    // we need to resume() inside of run_loop so that if this coro gets
    // suspended, it won't suspend while holding the lock forever
    return false;
  }

  /// Post this task to the braid queue, and attempt to take the lock and
  /// start executing tasks on the braid.
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer);

  /// Does nothing.
  constexpr void await_resume() {}
};

/// The awaitable type returned by `tmc::ex_braid::exit()`.
class [[nodiscard("You must co_await aw_braid_exit for it to have any "
                  "effect.")]] aw_braid_exit {
  friend class ex_braid;
  ex_braid &s;
  aw_braid_exit(ex_braid &s_in) : s(s_in) {}

public:
  /// Always suspends.
  constexpr bool await_ready() { return false; }

  /// Post this task to the braid's parent.
  inline void await_suspend(std::coroutine_handle<> outer) {
    s.type_erased_this.parent->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }

  /// Does nothing.
  constexpr void await_resume() {}
};

inline aw_braid_enter ex_braid::enter() { return {*this}; }
inline aw_braid_exit ex_braid::exit() { return {*this}; }

// I would LOVE to implement a scoped_lock but until we get async destructor
// that won't happen See https://wg21.link/p1662/status
// struct scoped_braid_lock
// {
//   ex_braid &b;
//   scoped_braid_lock(ex_braid &b_in) : b(b_in) {}
//   ~scoped_braid_lock() {
//     co_await b.exit();
//   }
// };

} // namespace tmc

// Intellisense / clangd gets confused if this #define is carried over into
// ex_braid.ipp, so instead just undef it here and redefine it there.
#ifdef USE_BRAID_WORK_ITEM
#undef USE_BRAID_WORK_ITEM
#endif

#ifdef TMC_IMPL
#include "tmc/detail/ex_braid.ipp"
#endif
