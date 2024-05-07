#pragma once
#include <coroutine>
namespace tmc {

/// [coroutine.noop]
struct noop_coroutine_promise {};
constexpr inline __attribute__((always_inline)) void __dummy_resume_destroy() {}

struct noop_frame {
  void (*const __r)() = __dummy_resume_destroy;
  void (*const __d)() = __dummy_resume_destroy;
  struct noop_coroutine_promise __p;
};
static inline const constinit noop_frame _S_fr{};

// 17.12.4.1 Class noop_coroutine_promise
/// [coroutine.promise.noop]
struct noop_task {
  // _GLIBCXX_RESOLVE_LIB_DEFECTS
  // 3460. Unimplementable noop_coroutine_handle guarantees
  // [coroutine.handle.noop.conv], conversion
  constexpr operator std::coroutine_handle<>() const noexcept {
    return std::coroutine_handle<>::from_address((void*)&_S_fr);
  }

  // [coroutine.handle.noop.observers], observers
  constexpr explicit operator bool() const noexcept { return true; }

  constexpr bool done() const noexcept { return false; }

  // [coroutine.handle.noop.resumption], resumption
  void operator()() const noexcept {}

  void resume() const noexcept {}

  void destroy() const noexcept {}

  // [coroutine.handle.noop.promise], promise access
  noop_coroutine_promise const& promise() const noexcept { return _S_fr.__p; }

  // [coroutine.handle.noop.address], address
  constexpr void* address() const noexcept { return (void*)&_S_fr; }
};
static inline noop_task _S_ta{};

constexpr inline noop_task noop() noexcept { return _S_ta; }

} // namespace tmc