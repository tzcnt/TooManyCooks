#pragma once
#ifdef TMC_USE_MUTEXQ
#include "tmc/detail/qu_mutex.hpp"
#else
#include "tmc/detail/qu_lockfree.h"
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
  std::shared_ptr<tiny_lock> lock;
  bool
      *destroyed_by_this_thread; // non-atomic, as it is only w->r by one thread
  std::atomic<size_t> never_yield;
  detail::type_erased_executor type_erased_this;
  detail::running_task_data stored_context;
  tmc::task<void> try_run_loop(std::shared_ptr<tiny_lock> this_braid_lock,
                               bool *this_thread_destroyed);

  void enter_context();
  void exit_context();

public:
  void post_variant(work_item &&item, size_t prio);

#ifdef USE_BRAID_WORK_ITEM
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
    // auto operator++(int) {
    //   auto tmp = *this;
    //   ++(*this);
    //   return tmp;
    // }
  };
  template <typename It> void post_bulk(It items, size_t prio, size_t count) {
    queue.enqueue_bulk(bulk_prio_iterator<It>(items, prio), count);
    if (!lock->is_locked()) {
      type_erased_this.parent->post_variant(
          std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
          prio);
    }
  }
#else
  template <typename It> void post_bulk(It items, size_t prio, size_t count) {
    queue.enqueue_bulk(items, count);
    if (!lock->is_locked()) {
      type_erased_this.parent->post_variant(
          std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
          prio);
    }
  }
#endif
  inline detail::type_erased_executor *type_erased() {
    return &type_erased_this;
  }

private:
  ex_braid(detail::type_erased_executor *parent);

public:
  ex_braid();
  template <typename Executor>
  ex_braid(Executor &parent) : ex_braid(parent.type_erased()) {}
  ~ex_braid();
  // Can also be thought of as an async `lock()`
  aw_braid_enter enter();
  // Can also be thought of as an async `unlock()`
  aw_braid_exit exit();

private:
  ex_braid &operator=(const ex_braid &other) = delete;
  ex_braid(const ex_braid &other) = delete;
  // not movable due to this_executor pointer being accessible by child threads
  ex_braid &operator=(const ex_braid &&other) = delete;
  ex_braid(const ex_braid &&other) = delete;
};

struct aw_braid_enter {
  ex_braid &s;
  aw_braid_enter(ex_braid &s_in) : s(s_in) {}

  // always have to suspend here, even if we can get the lock right away
  // we need to resume() inside of run_loop so that if this coro gets suspended,
  // it won't suspend while holding the lock forever
  constexpr bool await_ready() { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer);

  constexpr void await_resume() {}
};

struct aw_braid_exit {
  ex_braid &s;
  aw_braid_exit(ex_braid &s_in) : s(s_in) {}

  constexpr bool await_ready() { return false; }

  inline void await_suspend(std::coroutine_handle<> outer) {
    s.type_erased_this.parent->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }
  constexpr void await_resume() {}
};

[[nodiscard("You must co_await the return of "
            "ex_braid::enter().")]] inline aw_braid_enter
ex_braid::enter() {
  return {*this};
}
[[nodiscard("You must co_await the return of "
            "ex_braid::exit().")]] inline aw_braid_exit
ex_braid::exit() {
  return {*this};
}

// I would LOVE to implement a scoped_lock but until we get async destructor
// that won't happen See https://wg21.link/p1662/status struct
// scoped_braid_lock
// {
//   ex_braid &b;
//   scoped_braid_lock(ex_braid &b_in) : b(b_in) {}
//   ~scoped_braid_lock() {
//     co_await b.exit();
//   }
// };

} // namespace tmc

// Code navigation gets confused if this #define is carried over into
// ex_braid.ipp, so instead just undef it here and redefine it there.
#ifdef USE_BRAID_WORK_ITEM
#undef USE_BRAID_WORK_ITEM
#endif

#ifdef TMC_IMPL
#include "tmc/detail/ex_braid.ipp"
#endif
