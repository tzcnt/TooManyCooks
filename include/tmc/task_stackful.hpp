// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/task_wrapper.hpp"       // IWYU pragma: keep
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
#include <atomic>
#endif

#if TMC_HAS_EXCEPTIONS
#include <exception>
#endif

namespace tmc {
namespace detail {
#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
inline std::atomic<size_t> g_task_stackful_alloc_count;
#endif

class task_stackful_bump_allocator {
  static constexpr size_t MAX_ALLOCATIONS = 64;

  struct allocation_header {
    task_stackful_bump_allocator* owner;
    uint32_t slot;
    uint32_t padding;
  };

  std::array<std::byte, 64 * 1024> storage;
  std::array<std::atomic<void*>, MAX_ALLOCATIONS> allocation_ptrs;
  std::array<std::atomic<size_t>, MAX_ALLOCATIONS> allocation_sizes;
  std::atomic<uint64_t> active_bitmap;

  void deallocate_internal(void* ptr, uint32_t slot) noexcept {
    active_bitmap.fetch_and(~(uint64_t(1) << slot), std::memory_order_release);
    allocation_ptrs[slot].store(nullptr, std::memory_order_release);
    allocation_sizes[slot].store(0, std::memory_order_release);
  }

public:
  task_stackful_bump_allocator() noexcept : active_bitmap(0) {
    for (size_t i = 0; i < MAX_ALLOCATIONS; ++i) {
      allocation_ptrs[i].store(nullptr, std::memory_order_relaxed);
      allocation_sizes[i].store(0, std::memory_order_relaxed);
    }
  }

  void* allocate(size_t size, size_t align) noexcept {
    uint64_t bitmap = active_bitmap.load(std::memory_order_acquire);
    if (bitmap == ~uint64_t(0)) {
      return nullptr;
    }

    int slot = std::countr_one(bitmap);
    if (slot >= MAX_ALLOCATIONS) {
      return nullptr;
    }

    size_t offset = 0;
    for (int i = 0; i < slot; ++i) {
      offset += allocation_sizes[i].load(std::memory_order_acquire);
    }

    size_t header_size = sizeof(allocation_header);
    std::byte* base = storage.data() + offset;
    uintptr_t user_addr = reinterpret_cast<uintptr_t>(base + header_size);
    user_addr = (user_addr + align - 1) & ~(align - 1);
    std::byte* user_ptr = reinterpret_cast<std::byte*>(user_addr);
    allocation_header* header = reinterpret_cast<allocation_header*>(user_ptr) - 1;

    if (reinterpret_cast<uintptr_t>(user_ptr) + size >
        reinterpret_cast<uintptr_t>(storage.data()) + storage.size()) {
      return nullptr;
    }

    new (header) allocation_header{this, static_cast<uint32_t>(slot), 0};

    allocation_ptrs[slot].store(user_ptr, std::memory_order_release);
    allocation_sizes[slot].store((user_ptr - base) + size, std::memory_order_release);
    active_bitmap.fetch_or(uint64_t(1) << slot, std::memory_order_release);

    return user_ptr;
  }

  void deallocate(void* ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }

    allocation_header* header = reinterpret_cast<allocation_header*>(ptr) - 1;
    
    if (header->owner != this) {
      if (header->owner != nullptr) {
        header->owner->deallocate_internal(ptr, header->slot);
      } else {
        ::operator delete(ptr);
      }
      return;
    }

    deallocate_internal(ptr, header->slot);
  }
};

inline thread_local task_stackful_bump_allocator g_task_stackful_allocator;

template <typename Result> struct task_stackful_promise;
} // namespace detail

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
namespace debug {
/// Returns the current value of the tmc::task_stackful allocation counter.
/// This is useful to determine if HALO is working; task_stackfuls that have
/// their allocations folded into the parent allocation by HALO do not increase
/// this counter.
inline size_t get_task_stackful_alloc_count() {
  return tmc::detail::g_task_stackful_alloc_count.load(
    std::memory_order_seq_cst
  );
}

/// Allows you to reset the tmc::task_stackful allocation counter, in order to
/// count the number of allocations in a specific program section.
inline void set_task_stackful_alloc_count(size_t Value) {
  tmc::detail::g_task_stackful_alloc_count.store(
    Value, std::memory_order_seq_cst
  );
}
} // namespace debug
#endif

template <typename Awaitable, typename Result> class aw_task_stackful;

/// The main coroutine type used by TooManyCooks. `task_stackful` is a lazy /
/// cold coroutine and will not begin running immediately. To start running a
/// `task_stackful`, you can:
///
/// Use `co_await` directly on the task_stackful to run it and await the
/// results.
///
/// Call `tmc::spawn()` to create a task_stackful wrapper that can be configured
/// before `co_await` ing the results.
///
/// Call `tmc::spawn_many()` to submit and await multiple task_stackfuls at
/// once. This task_stackful group can be configured before `co_await` ing the
/// results.
///
/// Call `tmc::post()` / `tmc::post_waitable()` to submit this task_stackful for
/// execution to an async executor from external (non-async) calling code.
template <typename Result>
struct [[nodiscard(
  "You must submit or co_await task_stackful for execution. Failure to "
  "do so will result in a memory leak."
)]] TMC_CORO_AWAIT_ELIDABLE task_stackful {
  using result_type = Result;
  using promise_type = tmc::detail::task_stackful_promise<Result>;
  std::coroutine_handle<promise_type> handle;

  /// Suspend the outer coroutine and run this task_stackful directly. The
  /// intermediate awaitable type `aw_task_stackful` cannot be used directly;
  /// the return type of the `co_await` expression will be `Result` or `void`.
  aw_task_stackful<task_stackful<Result>, Result>
  operator co_await() && noexcept {
    return aw_task_stackful<task_stackful<Result>, Result>(std::move(*this));
  }

  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] inline task_stackful&
  resume_on(tmc::ex_any* Executor) & noexcept {
    // This overload is called by the other overloads.
    handle.promise().customizer.continuation_executor = Executor;
    return *this;
  }
  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] task_stackful&
  resume_on(Exec&& Executor) & noexcept {
    return resume_on(
      tmc::detail::get_executor_traits<Exec>::type_erased(Executor)
    );
  }
  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] task_stackful&
  resume_on(Exec* Executor) & noexcept {
    return resume_on(
      tmc::detail::get_executor_traits<Exec>::type_erased(*Executor)
    );
  }

  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] inline task_stackful&&
  resume_on(tmc::ex_any* Executor) && noexcept {
    // This overload is called by the other overloads.
    handle.promise().customizer.continuation_executor = Executor;
    return std::move(*this);
  }
  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] task_stackful&&
  resume_on(Exec&& Executor) && noexcept {
    handle.promise().customizer.continuation_executor =
      tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    return std::move(*this);
  }
  /// When this task_stackful completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task_stackful for execution. Failure to "
    "do so will result in a memory leak."
  )]] task_stackful&&
  resume_on(Exec* Executor) && noexcept {
    handle.promise().customizer.continuation_executor =
      tmc::detail::get_executor_traits<Exec>::type_erased(*Executor);
    return std::move(*this);
  }

  inline task_stackful() noexcept : handle(nullptr) {}

#ifndef TMC_TRIVIAL_TASK
  /// Tasks are move-only
  task_stackful(std::coroutine_handle<promise_type>&& Other) noexcept {
    handle = Other;
    Other = nullptr;
  }
  task_stackful&
  operator=(std::coroutine_handle<promise_type>&& Other) noexcept {
    handle = Other;
    Other = nullptr;
    return *this;
  }

  task_stackful(task_stackful&& Other) noexcept {
    handle = Other.handle;
    Other.handle = nullptr;
  }

  task_stackful& operator=(task_stackful&& Other) noexcept {
    handle = Other.handle;
    Other.handle = nullptr;
    return *this;
  }

  /// Non-copyable
  task_stackful(const task_stackful& other) = delete;
  task_stackful& operator=(const task_stackful& other) = delete;

  /// When this task_stackful is destroyed, it should already have been
  /// deinitialized. Either because it was moved-from, or because the coroutine
  /// completed.
  ~task_stackful() { assert(!handle && "You must submit or co_await this."); }
#endif

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<>() && noexcept {
    auto addr = handle.address();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
    return std::coroutine_handle<>::from_address(addr);
  }

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<promise_type>() && noexcept {
    auto addr = handle.address();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
    return std::coroutine_handle<promise_type>::from_address(addr);
  }

  static task_stackful from_address(void* addr) noexcept {
    task_stackful t;
    t.handle = std::coroutine_handle<promise_type>::from_address(addr);
    return t;
  }

  static task_stackful from_promise(promise_type& prom) noexcept {
    task_stackful t;
    t.handle = std::coroutine_handle<promise_type>::from_promise(prom);
    return t;
  }

  bool done() const noexcept { return handle.done(); }

  inline void* address() const noexcept { return handle.address(); }

  // std::coroutine_handle::destroy() is const, but this isn't - it nulls the
  // pointer afterward
  void destroy() noexcept {
    handle.destroy();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }

  void resume() & noexcept { handle.resume(); }
  void operator()() & noexcept { handle.resume(); }

  void resume() && noexcept {
    handle.resume();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }
  void operator()() && noexcept {
    handle.resume();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }

  operator bool() const noexcept { return handle.operator bool(); }

  auto& promise() const noexcept { return handle.promise(); }
};
namespace detail {

template <typename Result> struct task_stackful_promise {
  awaitable_customizer<Result> customizer;

  task_stackful_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<task_stackful_promise>
  final_suspend() const noexcept {
    return {};
  }
  task_stackful<Result> get_return_object() noexcept {
    return {task_stackful<Result>::from_promise(*this)};
  }
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }

  template <typename RV>
  void
  return_value(RV&& Value) noexcept(std::is_nothrow_move_constructible_v<RV>) {
    *customizer.result_ptr = static_cast<RV&&>(Value);
  }

  template <typename Awaitable>
  decltype(auto) await_transform(Awaitable&& awaitable) noexcept
    requires has_awaitable_traits<Awaitable>::value
  {
    // If you are looking at a compilation error on this line when awaiting
    // a TMC awaitable, you probably need to std::move() whatever you are
    // co_await'ing. co_await std::move(your_tmc_awaitable_variable_name)
    return tmc::detail::get_awaitable_traits<Awaitable>::get_awaiter(
      std::forward<Awaitable>(awaitable)
    );
  }

#ifndef TMC_NO_UNKNOWN_AWAITABLES
  template <typename Awaitable>
  decltype(auto) await_transform(
    Awaitable&& awaitable
  ) noexcept(std::is_nothrow_move_constructible_v<Awaitable>)
    requires(!has_awaitable_traits<Awaitable>::value)
  {
    // If you are awaiting a non-TMC awaitable, then you should consult the
    // documentation there to see why we can't deduce the awaiter type, or
    // specialize tmc::detail::awaitable_traits for it yourself.
    return tmc::detail::safe_wrap(std::forward<Awaitable>(awaitable));
  }
#endif

#ifndef __clang__
  // GCC creates a TON of warnings if this is missing with the noexcept new
  static task_stackful<Result>
  get_return_object_on_allocation_failure() noexcept {
    return {};
  }
#endif

  static void* operator new(size_t size) noexcept {
    void* ptr =
      g_task_stackful_allocator.allocate(size, alignof(std::max_align_t));
    if (ptr == nullptr) {
      ptr = ::operator new(size, std::nothrow);
    }
    return ptr;
  }

  static void operator delete(void* ptr) noexcept {
    g_task_stackful_allocator.deallocate(ptr);
  }
};

template <> struct task_stackful_promise<void> {
  awaitable_customizer<void> customizer;

  task_stackful_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<task_stackful_promise>
  final_suspend() const noexcept {
    return {};
  }
  task_stackful<void> get_return_object() noexcept {
    return {task_stackful<void>::from_promise(*this)};
  }
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }

  void return_void() noexcept {}

  template <typename Awaitable>
  decltype(auto) await_transform(Awaitable&& awaitable) noexcept
    requires has_awaitable_traits<Awaitable>::value
  {
    // If you are looking at a compilation error on this line when awaiting
    // a TMC awaitable, you probably need to std::move() whatever you are
    // co_await'ing. co_await std::move(your_tmc_awaitable_variable_name)
    return tmc::detail::get_awaitable_traits<Awaitable>::get_awaiter(
      std::forward<Awaitable>(awaitable)
    );
  }

#ifndef TMC_NO_UNKNOWN_AWAITABLES
  template <typename Awaitable>
  decltype(auto) await_transform(
    Awaitable&& awaitable
  ) noexcept(std::is_nothrow_move_constructible_v<Awaitable>)
    requires(!has_awaitable_traits<Awaitable>::value)
  {
    // If you are awaiting a non-TMC awaitable, then you should consult the
    // documentation there to see why we can't deduce the awaiter type, or
    // specialize tmc::detail::awaitable_traits for it yourself.
    return tmc::detail::safe_wrap(std::forward<Awaitable>(awaitable));
  }
#endif

  static void* operator new(size_t size) noexcept {
    void* ptr =
      g_task_stackful_allocator.allocate(size, alignof(std::max_align_t));
    if (ptr == nullptr) {
      ptr = ::operator new(size, std::nothrow);
    }
    return ptr;
  }

  static void operator delete(void* ptr) noexcept {
    g_task_stackful_allocator.deallocate(ptr);
  }
};
} // namespace detail

template <typename Awaitable, typename Result> class aw_task_stackful {
  Awaitable handle;
  tmc::detail::result_storage_t<Result> result;

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  aw_task_stackful(Awaitable&& Handle) noexcept : handle(std::move(Handle)) {
    assert(
      handle.address() != nullptr &&
      "You may only submit or co_await this once."
    );
  }

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::get_awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    tmc::detail::get_awaitable_traits<Awaitable>::set_result_ptr(
      handle, &result
    );
    return std::move(handle);
  }

  /// Returns the value provided by the awaited task_stackful.
  inline Result&& await_resume() noexcept {
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  // Not movable or copyable due to holding result storage
  aw_task_stackful(const aw_task_stackful& other) = delete;
  aw_task_stackful& operator=(const aw_task_stackful& other) = delete;
  aw_task_stackful(aw_task_stackful&& other) = delete;
  aw_task_stackful&& operator=(aw_task_stackful&& other) = delete;
};

template <typename Awaitable> class aw_task_stackful<Awaitable, void> {
  Awaitable handle;

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  aw_task_stackful(Awaitable&& Handle) noexcept : handle(std::move(Handle)) {
    assert(
      handle.address() != nullptr &&
      "You may only submit or co_await this once."
    );
  }

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::get_awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    return std::move(handle);
  }
  inline void await_resume() noexcept {}

  // Could be movable, but prefer to make it consistent with the others
  aw_task_stackful(const aw_task_stackful& other) = delete;
  aw_task_stackful& operator=(const aw_task_stackful& other) = delete;
  aw_task_stackful(aw_task_stackful&& other) = delete;
  aw_task_stackful&& operator=(aw_task_stackful&& other) = delete;
};

namespace detail {

template <typename Result> struct awaitable_traits<tmc::task_stackful<Result>> {
  using result_type = Result;
  using self_type = tmc::task_stackful<Result>;
  using awaiter_type = tmc::aw_task_stackful<self_type, Result>;

  // Values controlling the behavior when awaited directly in a
  // tmc::task_stackful
  static awaiter_type get_awaiter(self_type&& Awaitable) noexcept {
    return awaiter_type(static_cast<self_type&&>(Awaitable));
  }

  // Values controlling the behavior when wrapped by a utility function
  // such as tmc::spawn_*()
  static constexpr configure_mode mode = TMC_TASK;

  static void set_result_ptr(
    self_type& Awaitable, tmc::detail::result_storage_t<Result>* ResultPtr
  ) noexcept {
    Awaitable.promise().customizer.result_ptr = ResultPtr;
  }

  static void
  set_continuation(self_type& Awaitable, void* Continuation) noexcept {
    Awaitable.promise().customizer.continuation = Continuation;
  }

  static void
  set_continuation_executor(self_type& Awaitable, void* ContExec) noexcept {
    Awaitable.promise().customizer.continuation_executor = ContExec;
  }

  static void set_done_count(self_type& Awaitable, void* DoneCount) noexcept {
    Awaitable.promise().customizer.done_count = DoneCount;
  }

  static void set_flags(self_type& Awaitable, size_t Flags) noexcept {
    Awaitable.promise().customizer.flags = Flags;
  }
};
} // namespace detail
} // namespace tmc
