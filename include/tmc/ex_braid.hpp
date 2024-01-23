#pragma once
#ifdef TMC_USE_MUTEXQ
#include "tmc/detail/qu_mutex.hpp"
#else
#include "tmc/detail/qu_lockfree.hpp"
#endif
#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"
#include "tmc/task.hpp"
#include <coroutine>
#include <memory>

namespace tmc {
class ex_braid {
  friend class aw_ex_scope_enter<ex_braid>;

#ifdef TMC_USE_MUTEXQ
  using task_queue_t = detail::MutexQueue<work_item>;
#else
  using task_queue_t = tmc::queue::ConcurrentQueue<work_item>;
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
  bool* destroyed_by_this_thread;

  std::atomic<size_t> never_yield;
  detail::type_erased_executor type_erased_this;
  detail::type_erased_executor* parent;
  detail::running_task_data stored_context;

  /// The main loop of the braid; only 1 thread is allowed to enter the inner
  /// loop. If the lock is already taken, other threads will return immediately.
  tmc::task<void> try_run_loop(
    std::shared_ptr<tiny_lock> ThisBraidLock, bool* DestroyedByThisThread
  );

  /// Called after getting the lock. Update the thread locals so that spawn()
  /// will create tasks on the braid, and yield_requested() always returns
  /// false.
  void thread_enter_context();

  /// Called before releasing the lock. Reset the thread locals to what they
  /// were before calling thread_enter_context().
  void thread_exit_context();

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

public:
  /// Submits a single work_item to the braid, and attempts to take the lock and
  /// start executing tasks on the braid. Rather than calling this directly, it
  /// is recommended to use the `tmc::post()` free function template.
  void post(work_item&& Item, size_t Priority);

  /// Submits `count` items to the braid, and attempts to take the lock and
  /// start executing tasks on the braid. `It` must be an iterator
  /// type that implements `operator*()` and `It& operator++()`.
  template <typename It>
  void post_bulk(It Items, size_t Priority, size_t Count) {
    queue.enqueue_bulk(Items, Count);
    if (!lock->is_locked()) {
      parent->post(
        std::coroutine_handle<>(try_run_loop(lock, destroyed_by_this_thread)),
        Priority
      );
    }
  }

  /// Implements `tmc::TypeErasableExecutor` concept, but unlikely to be needed
  /// directly by users.
  inline detail::type_erased_executor* type_erased() {
    return &type_erased_this;
  }

private:
  ex_braid(detail::type_erased_executor* Parent);

public:
  /// Construct a braid with the current executor as its parent. It is an error
  /// to call this from a thread that is not running on an executor.
  ex_braid();

  /// Construct a braid with the specified executor as its parent.
  template <typename Executor>
  ex_braid(Executor& Parent) : ex_braid(Parent.type_erased()) {}
  ~ex_braid();

private:
  ex_braid& operator=(const ex_braid& Other) = delete;
  ex_braid(const ex_braid& Other) = delete;
  // not movable due to this_executor pointer being accessible by child threads
  ex_braid& operator=(const ex_braid&& Other) = delete;
  ex_braid(const ex_braid&& Other) = delete;
};

// I would LOVE to implement a scoped_lock but until we get async destructor
// that won't happen See https://wg21.link/p1662/status
// struct scoped_braid_lock
// {
//   ex_braid &b;
//   scoped_braid_lock(ex_braid &Braid) : b(Braid) {}
//   ~scoped_braid_lock() {
//     co_await b.exit();
//   }
// };

} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/ex_braid.ipp"
#endif
