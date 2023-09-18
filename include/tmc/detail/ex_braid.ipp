
// Implementation definition file for ex_braid. This will be included anywhere
// TMC_IMPL is defined.
// If you prefer to manually separate compilation units, you can instead include
// this file directly in a CPP file.
#include "tmc/ex_braid.hpp"

#if !defined(TMC_PRIORITY_COUNT) || (TMC_PRIORITY_COUNT > 1)
// braid_work_item's purpose is to save and restore priorities for work items.
// if there is only one priority level, we don't need this.
#define USE_BRAID_WORK_ITEM
#endif

namespace tmc {
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
    braid_work_item_t item;
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
  s.queue.enqueue(ex_braid::braid_work_item_t{outer, prio});
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

} // namespace tmc
