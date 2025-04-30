// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_braid. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_braid.hpp"
#include "tmc/work_item.hpp"

#include <atomic>

namespace tmc {
tmc::task<void> ex_braid::try_run_loop(
  std::shared_ptr<tiny_lock> ThisBraidLock, bool* DestroyedByThisThread
) {
  // parameters make a ref-counted copy of lock in case this braid is destroyed
  // also make a copy of destroyed_by_this_thread pointer for the same reason
  do {
    if (!ThisBraidLock->try_lock()) {
      co_return;
    }
    work_item item;
    if (queue.try_dequeue(item)) {
      thread_enter_context();
      do {
        item();
        if (*DestroyedByThisThread) [[unlikely]] {
          // It's not safe to access any member variables at this point
          // DON'T unlock after this - keep threads from entering the runloop
          delete DestroyedByThisThread;
          co_return;
        }
      } while (queue.try_dequeue(item));
      thread_exit_context();
    }
    ThisBraidLock->unlock();
    tmc::detail::memory_barrier(); // pairs with barrier in post_runloop_task
    // check queue again after unlocking to prevent missing work items
  } while (!queue.empty());
}

void ex_braid::thread_enter_context() {
  // save
  stored_context = tmc::detail::this_thread::this_task;

  // enter
  tmc::detail::this_thread::this_task.yield_priority =
    &tmc::detail::never_yield;
  tmc::detail::this_thread::executor = &type_erased_this;
}

void ex_braid::thread_exit_context() {
  // restore
  // the priority that is restored here is that of the try_run_loop() call
  // individual tasks are resumed on parent according to their own priority
  // values
  tmc::detail::this_thread::this_task = stored_context;
  tmc::detail::this_thread::executor = parent_executor;
}

void ex_braid::post(
  work_item&& Item, size_t Priority, [[maybe_unused]] size_t ThreadHint
) {
  queue.enqueue(std::move(Item));
  post_runloop_task(Priority);
}

void ex_braid::post_runloop_task(size_t Priority) {
  if (tmc::detail::this_thread::exec_is(&type_erased_this)) {
    // we are already inside of try_run_loop() - don't need to do anything
    return;
  }

  // This is always called after an enqueue. Make sure that the queue store
  // is globally visible before checking if someone has the lock.
  tmc::detail::memory_barrier(); // pairs with barrier in try_run_loop

  // If someone already has the lock, we don't need to post, as they will see
  // this item in queue.
  if (!lock->is_locked()) {
    // executor check not needed, it happened in braid constructor
    parent_executor->post(
      std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
      Priority
    );
  }
}

ex_braid::ex_braid(tmc::ex_any* Parent)
    : queue(1), lock{std::make_shared<tiny_lock>()},
      destroyed_by_this_thread{new bool(false)}, type_erased_this(this),
      parent_executor(Parent) {
  if (Parent == nullptr) {
    parent_executor = tmc::detail::g_ex_default.load(std::memory_order_acquire);
  }
}

ex_braid::ex_braid() : ex_braid(tmc::detail::this_thread::executor) {}

ex_braid::~ex_braid() {
  if (tmc::detail::this_thread::exec_is(&type_erased_this)) {
    // we are inside of run_one_func() inside of try_run_loop(); we already have
    // the lock
    thread_exit_context();
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

/// Post this task to the braid queue, and attempt to take the lock and
/// start executing tasks on the braid.
std::coroutine_handle<>
ex_braid::task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
  queue.enqueue(std::move(Outer));
  if (tmc::detail::this_thread::exec_is(parent_executor) &&
      tmc::detail::this_thread::prio_is(Priority)) {
    // rather than posting to exec, we can just run the queue directly
    return try_run_loop(lock, destroyed_by_this_thread);
  } else {
    post_runloop_task(Priority);
    return std::noop_coroutine();
  }
}

namespace detail {

void executor_traits<tmc::ex_braid>::post(
  tmc::ex_braid& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
) {
  ex.post(std::move(Item), Priority, ThreadHint);
}

tmc::ex_any* executor_traits<tmc::ex_braid>::type_erased(tmc::ex_braid& ex) {
  return ex.type_erased();
}

std::coroutine_handle<> executor_traits<tmc::ex_braid>::task_enter_context(
  tmc::ex_braid& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.task_enter_context(Outer, Priority);
}

} // namespace detail
} // namespace tmc
