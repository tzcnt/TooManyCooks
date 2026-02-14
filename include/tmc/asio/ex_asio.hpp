// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/init_params.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <thread>

#ifdef TMC_USE_BOOST_ASIO
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#else
#include <asio/any_io_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#endif

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_forward_defs.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/thread_layout.hpp"
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cstdio>
#endif

namespace tmc {
/// A wrapper over `asio::io_context` with a single executor thread.
/// It is both a TooManyCooks executor and an Asio executor,
/// so it can be passed to functions from either library.
class ex_asio {
  tmc::detail::InitParams* init_params;

  inline tmc::detail::InitParams* set_init_params() {
    assert(!initialized.load(std::memory_order_relaxed));
    if (init_params == nullptr) {
      init_params = new tmc::detail::InitParams;
    }
    return init_params;
  }

  inline void init_thread_locals() {
    tmc::detail::this_thread::executor() = &type_erased_this;
  }

  inline void clear_thread_locals() {
    tmc::detail::this_thread::executor() = nullptr;
  }

public:
#ifdef TMC_USE_BOOST_ASIO
  using ioc_t = boost::asio::io_context;
#else
  using ioc_t = asio::io_context;
#endif
  ioc_t ioc;
  std::jthread ioc_thread;
  tmc::ex_any type_erased_this;
  std::atomic<bool> initialized;

#ifdef TMC_USE_HWLOC
  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to limit the executor to a subset of the available CPUs.
  /// This should only be called once, as this is a single-threaded executor.
  inline ex_asio& add_partition(tmc::topology::topology_filter Filter) {
    set_init_params()->add_partition(Filter);
    return *this;
  }
#endif

  /// Builder func to set a hook that will be invoked at the startup of the
  /// executor thread, and passed the ordinal index of the thread (which is
  /// always 0, since this is a single-threaded executor).
  inline ex_asio& set_thread_init_hook(std::function<void(size_t)> Hook) {
    set_init_params()->set_thread_init_hook(Hook);
    return *this;
  }

  /// Builder func to set a hook that will be invoked before destruction of each
  /// thread owned by this executor, and passed the ordinal index of the thread
  /// (which is always 0, since this is a single-threaded executor).
  inline ex_asio& set_thread_teardown_hook(std::function<void(size_t)> Hook) {
    set_init_params()->set_thread_teardown_hook(Hook);
    return *this;
  }

  inline void init() {
    bool expected = false;
    if (!initialized.compare_exchange_strong(expected, true)) {
      return;
    }

    if (ioc.stopped()) {
      ioc.restart();
    }
    // replaces need for an executor_work_guard
    ioc.get_executor().on_work_started();

#ifdef TMC_USE_HWLOC
    hwloc_topology* topo;
    auto internal_topo = tmc::topology::detail::query_internal(topo);

    // Create partition cpuset based on user configuration
    tmc::detail::hwloc_unique_bitmap partitionCpuset;
    tmc::topology::cpu_kind::value cpuKind = tmc::topology::cpu_kind::ALL;
    if (init_params != nullptr && !init_params->partitions.empty()) {
      partitionCpuset = tmc::detail::make_partition_cpuset(
        topo, internal_topo, init_params->partitions[0], cpuKind
      );
#ifdef TMC_DEBUG_THREAD_CREATION
      std::printf("ex_asio partition cpuset bitmap: ");
      partitionCpuset.print();
#endif
    }
#endif

    // Copy this since it outlives init_params
    std::function<void(tmc::topology::thread_info)> ThreadTeardownHook =
      nullptr;
    if (init_params != nullptr &&
        init_params->thread_teardown_hook != nullptr) {
      ThreadTeardownHook = init_params->thread_teardown_hook;
    }

    std::atomic<tmc::detail::atomic_wait_t> initThreadsBarrier(1);
    tmc::detail::memory_barrier();
    ioc_thread = std::jthread([this, &initThreadsBarrier, ThreadTeardownHook
#ifdef TMC_USE_HWLOC
                               ,
                               topo, myCpuSet = partitionCpuset.clone(),
                               Kind = cpuKind
#endif
    ]() mutable {
#ifdef TMC_USE_HWLOC
      if (myCpuSet != nullptr) {
        tmc::detail::pin_thread(topo, myCpuSet, Kind);
        myCpuSet.free();
      }
#endif

      // Setup
      init_thread_locals();

      if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
        tmc::topology::thread_info info{};
        info.index = 0;
        init_params->thread_init_hook(info);
      }

      initThreadsBarrier.fetch_sub(1);
      initThreadsBarrier.notify_all();

      // Run loop
      ioc.run();

      // Teardown
      if (ThreadTeardownHook != nullptr) {
        tmc::topology::thread_info info{};
        info.index = 0;
        ThreadTeardownHook(info);
      }
    });

    // Wait for worker to finish init
    auto barrierVal = initThreadsBarrier.load();
    while (barrierVal != 0) {
      initThreadsBarrier.wait(barrierVal);
      barrierVal = initThreadsBarrier.load();
    }

    if (init_params != nullptr) {
      delete init_params;
      init_params = nullptr;
    }
  }

  /// Stops the executor, joins the worker thread, and destroys resources. Does
  /// not wait for any queued work to complete. `teardown()` must not be
  /// called from this executor's thread.
  ///
  /// Restores the executor to an uninitialized state. After
  /// calling `teardown()`, you may call `set_X()` to reconfigure the executor
  /// and call `init()` again.
  ///
  /// If the executor is not initialized, calling `teardown()` will do nothing.
  inline void teardown() {
    bool expected = true;
    if (!initialized.compare_exchange_strong(expected, false)) {
      return;
    }
    // replaces need for an executor_work_guard
    ioc.get_executor().on_work_finished();
    ioc.stop();
    ioc_thread.join();
  }

  inline ex_asio()
      : init_params{nullptr}, ioc(1), type_erased_this(this),
        initialized(false) {}

  /// Invokes `teardown()`. Must not be called from this executor's thread.
  inline ~ex_asio() { teardown(); }

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_asio(ex_asio const&) = delete;
  ex_asio& operator=(ex_asio const&) = delete;
  ex_asio(ex_asio&&) = delete;
  ex_asio& operator=(ex_asio&&) = delete;

  /// Returns a pointer to the type erased `ex_any` version of this executor.
  /// This object shares a lifetime with this executor, and can be used for
  /// pointer-based equality comparison against
  /// the thread-local `tmc::current_executor()`.
  inline tmc::ex_any* type_erased() { return &type_erased_this; }

  inline void post(
    work_item&& Item, size_t Priority = 0,
    [[maybe_unused]] size_t ThreadHint = NO_HINT
  ) {
#ifdef TMC_USE_BOOST_ASIO
    boost::asio::post(
      ioc.get_executor(), [Priority, item = std::move(Item)]() mutable -> void {
        tmc::detail::this_thread::this_task().prio = Priority;
        item();
      }
    );
#else
    asio::post(
      ioc.get_executor(), [Priority, item = std::move(Item)]() mutable -> void {
        tmc::detail::this_thread::this_task().prio = Priority;
        item();
      }
    );
#endif
  }

  template <typename It>
  void post_bulk(
    It Items, size_t Count, [[maybe_unused]] size_t Priority = 0,
    [[maybe_unused]] size_t ThreadHint = NO_HINT
  ) {
    for (size_t i = 0; i < Count; ++i) {
#ifdef TMC_USE_BOOST_ASIO
      boost::asio::post(
        ioc.get_executor(),
        [Priority,
         Item =
           tmc::detail::into_work_item(std::move(*Items))]() mutable -> void {
          tmc::detail::this_thread::this_task().prio = Priority;
          Item();
        }
      );
#else
      asio::post(
        ioc.get_executor(),
        [Priority,
         Item =
           tmc::detail::into_work_item(std::move(*Items))]() mutable -> void {
          tmc::detail::this_thread::this_task().prio = Priority;
          Item();
        }
      );
#endif
      ++Items;
    }
  }

  // Make it possible to pass this directly to asio functions,
  // Whether you are using the default type-erased any_io_executor
  // or the specialized io_context executor
  using executor_type = ioc_t::executor_type;
  inline operator executor_type() { return ioc.get_executor(); }
#ifdef TMC_USE_BOOST_ASIO
  inline operator boost::asio::any_io_executor() { return ioc.get_executor(); }
#else
  inline operator asio::any_io_executor() { return ioc.get_executor(); }
#endif

private:
  friend class aw_ex_scope_enter<ex_asio>;
  friend tmc::detail::executor_traits<ex_asio>;
  inline std::coroutine_handle<>
  dispatch(std::coroutine_handle<> Outer, size_t Priority) {
    if (tmc::detail::this_thread::exec_prio_is(&type_erased_this, Priority)) {
      return Outer;
    } else {
      post(std::move(Outer), Priority);
      return std::noop_coroutine();
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_asio> {
  static inline void post(
    tmc::ex_asio& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  ) {
    ex.post(std::move(Item), Priority, ThreadHint);
  }

  template <typename It>
  static inline void post_bulk(
    tmc::ex_asio& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(std::forward<It>(Items), Count, Priority, ThreadHint);
  }

  static inline tmc::ex_any* type_erased(tmc::ex_asio& ex) {
    return ex.type_erased();
  }

  static inline std::coroutine_handle<>
  dispatch(tmc::ex_asio& ex, std::coroutine_handle<> Outer, size_t Priority) {
    return ex.dispatch(Outer, Priority);
  }
};

#ifdef TMC_WINDOWS_DLL
TMC_DECL extern ex_asio g_ex_asio;
#ifdef TMC_IMPL
TMC_DECL ex_asio g_ex_asio;
#endif
#else
inline ex_asio g_ex_asio;
#endif
} // namespace detail

/// Returns a reference to the global instance of `tmc::ex_asio`.
constexpr ex_asio& asio_executor() { return tmc::detail::g_ex_asio; }

} // namespace tmc
