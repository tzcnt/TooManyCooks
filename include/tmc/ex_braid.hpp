#pragma once
#ifdef TMC_USE_MUTEXQ
#include "tmc/detail/qu_mutex.hpp"
#else
#include "tmc/detail/qu_lockfree.h"
#endif
#include "tmc/task.hpp"
#ifndef TMC_NO_POST_FUNC
#include "tmc/detail/coro_functor.hpp"
#endif
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"
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
#else
  using braid_work_item = work_item;
#endif

#ifdef TMC_USE_MUTEXQ
  using task_queue_t = detail::MutexQueue<braid_work_item>;
#else
  using task_queue_t = tmc::queue::ConcurrentQueue<braid_work_item>;
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

#ifdef TMC_IMPL
tmc::task<void>
ex_braid::try_run_loop(std::shared_ptr<tiny_lock> this_braid_lock,
                       bool *this_thread_destroyed) {
  // parameters make a ref-counted copy of lock in case this braid is destroyed
  // before this executes make a copy of this_thread_destroyed pointer for the
  // same reason
  do {
    if (!this_braid_lock->try_lock()) {
      co_return;
    }
    braid_work_item item;
    if (queue.try_dequeue(item)) {
      enter_context();
      do {
#ifdef USE_BRAID_WORK_ITEM
        detail::this_thread::this_task.prio = item.priority;
        item.work();
#else
        item();
#endif
        if (*this_thread_destroyed) [[unlikely]] {
          // It's not safe to access any member variables at this point
          // DON'T unlock after this - keep threads from entering the runloop
          delete this_thread_destroyed;
          co_return;
        }
      } while (queue.try_dequeue(item));
      exit_context();
    }
    this_braid_lock->unlock();
    // check queue again after unlocking to prevent missing work items
  } while (!queue.empty());
}

void ex_braid::enter_context() {
  // save
  stored_context = detail::this_thread::this_task;
  // DO NOT modify this - it's used outside the lock by post_*()
  // type_erased_this.parent = detail::this_thread::executor;

  // enter
  detail::this_thread::this_task.yield_priority = &never_yield;
  detail::this_thread::executor = &type_erased_this;
}

void ex_braid::exit_context() {
  // restore
  // the priority that is restored here is that of the try_run_loop() call
  // individual tasks are resumed on parent according to their own priority
  // values
  detail::this_thread::this_task = stored_context;
  detail::this_thread::executor = type_erased_this.parent;
}

void ex_braid::post_variant(work_item &&item, size_t prio) {
#ifdef USE_BRAID_WORK_ITEM
  queue.enqueue(braid_work_item{std::move(item), prio});
#else
  queue.enqueue(std::move(item));
#endif
  // If someone already has the lock, we don't need to post, as they will see
  // this item in queue.
  if (!lock->is_locked()) {
    type_erased_this.parent->post_variant(
        std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
        prio);
  }
}

ex_braid::ex_braid(detail::type_erased_executor *parent)
    : lock{std::make_shared<tiny_lock>()}, queue(32),
      destroyed_by_this_thread{new bool(false)},
      never_yield(std::numeric_limits<size_t>::max()), type_erased_this(*this) {
  type_erased_this.parent = parent;
}

ex_braid::ex_braid() : ex_braid(detail::this_thread::executor) {}

ex_braid::~ex_braid() {
  if (detail::this_thread::executor == &type_erased_this) {
    // we are inside of run_one_func() inside of try_run_loop(); we already have
    // the lock
    exit_context();
    *destroyed_by_this_thread = true;
    // release fence is required to prevent object deallocation to be moved
    // before this
    std::atomic_thread_fence(std::memory_order_release);
  } else {
    lock->spin_lock();
    // DON'T unlock after this - keep threads from entering the runloop
    delete destroyed_by_this_thread;
    // release fence is required to prevent object deallocation to be moved
    // before this
    std::atomic_thread_fence(std::memory_order_release);
  }
}

std::coroutine_handle<>
aw_braid_enter::await_suspend(std::coroutine_handle<> outer) {
  auto prio = detail::this_thread::this_task.prio;
#ifdef USE_BRAID_WORK_ITEM
  s.queue.enqueue(ex_braid::braid_work_item{outer, prio});
#else
  s.queue.enqueue(std::move(outer));
#endif
  if (detail::this_thread::executor == &s.type_erased_this) {
    // we are already inside of try_run_loop() - don't need to do anything
    return std::noop_coroutine();
  } else if (detail::this_thread::executor == s.type_erased_this.parent) {
    // rather than posting to exec, we can just run the queue directly
    return s.try_run_loop(s.lock, s.destroyed_by_this_thread);
  } else {
    // don't need to post if another thread is running try_run_loop already
    if (!s.lock->is_locked()) {
      // post s.try_run_loop to braid's parent executor
      // (don't allow braids to migrate across thread pools)
      s.type_erased_this.parent->post_variant(
          std::coroutine_handle<>(
              s.try_run_loop(s.lock, s.destroyed_by_this_thread)),
          prio);
    }
    return std::noop_coroutine();
  }
}
#endif

} // namespace tmc

#ifdef USE_BRAID_WORK_ITEM
#undef USE_BRAID_WORK_ITEM
#endif
