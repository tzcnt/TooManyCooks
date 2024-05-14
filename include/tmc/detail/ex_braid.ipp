// Implementation definition file for tmc::ex_braid. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.
#include "tmc/ex_braid.hpp"

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
    // check queue again after unlocking to prevent missing work items
  } while (!queue.empty());
}

void ex_braid::thread_enter_context() {
  // save
  stored_context = detail::this_thread::this_task;
  // DO NOT modify this - it's used outside the lock by post_*()
  // type_erased_this.parent = detail::this_thread::executor;

  // enter
  detail::this_thread::this_task.yield_priority = &detail::never_yield;
  detail::this_thread::executor = &type_erased_this;
}

void ex_braid::thread_exit_context() {
  // restore
  // the priority that is restored here is that of the try_run_loop() call
  // individual tasks are resumed on parent according to their own priority
  // values
  detail::this_thread::this_task = stored_context;
  detail::this_thread::executor = parent_executor;
}

void ex_braid::post(work_item&& Item, size_t Priority) {
  queue.enqueue(std::move(Item));
  // If someone already has the lock, we don't need to post, as they will see
  // this item in queue.
  if (!lock->is_locked()) {
    parent_executor->post(
      std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
      Priority
    );
  }
}

ex_braid::ex_braid(detail::type_erased_executor* Parent)
    : queue(32), lock{std::make_shared<tiny_lock>()},
      destroyed_by_this_thread{new bool(false)}, type_erased_this(this),
      parent_executor(Parent) {}

ex_braid::ex_braid() : ex_braid(detail::this_thread::executor) {}

ex_braid::~ex_braid() {
  if (detail::this_thread::exec_is(&type_erased_this)) {
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
  if (detail::this_thread::exec_is(&type_erased_this)) {
    // we are already inside of try_run_loop() - don't need to do anything
    return std::noop_coroutine();
  } else if (detail::this_thread::exec_is(parent_executor)) {
    // rather than posting to exec, we can just run the queue directly
    return try_run_loop(lock, destroyed_by_this_thread);
  } else {
    // don't need to post if another thread is running try_run_loop already
    if (!lock->is_locked()) {
      // post try_run_loop to braid's parent executor
      // (don't allow braids to migrate across thread pools)
      parent_executor->post(
        std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
        Priority
      );
    }
    return std::noop_coroutine();
  }
}

} // namespace tmc
