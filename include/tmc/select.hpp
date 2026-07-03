// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/tuple_helpers.hpp"
#include "tmc/mux_tuple.hpp"
#include "tmc/task.hpp"
#include "tmc/traits.hpp"

#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace tmc {
namespace detail {
// Marker used as the `Canceller` of a `tmc::cancellable` when the awaitable is
// its own cancellation handle (the single-argument `tmc::cancellable`
// constructor). It is empty; with TMC_NO_UNIQUE_ADDRESS on the member, it occupies no
// storage.
struct cancel_self_t {};

// Wraps an object passed into `tmc::cancellable` constructor form 2 into a functor so
// that it matches the behavior of form 1. The forwarded result lets `select()` detect an
// awaitable `.cancel()` (an async cancel) and await it.
template <typename Target> struct cancel_caller {
  Target target;
  decltype(auto) operator()() { return target.cancel(); }
};

// An awaitable that does nothing. Used by `select()` in the async cancel branch for any
// sync canceller mixed in. The fold expression cannot handle mixed await / no-await, so
// we must provide an awaitable object for syntactic compatibility.
struct noop_awaitable : tmc::detail::AwaitTagNoGroupAsIs {
  static constexpr bool await_ready() noexcept { return true; }
  static void await_suspend(std::coroutine_handle<>) noexcept {}
  static void await_resume() noexcept {}
};

// A functor that does nothing. Used in the diagnostic `cancellable` overload so that
// everything else compiles, and the only error is the human-readable static_assert.
struct noop_func {
  void operator()() const noexcept {}
};

// Forward-declared here so `tmc::cancellable` can grant it friendship.
template <typename Awaitable, typename Canceller> consteval bool canceller_is_async();

} // namespace detail

/// Pairs an `Awaitable` with a `Canceller` for use with `tmc::select()`.
///
/// Construct one with class template argument deduction - `tmc::cancellable(...)`
/// selects the correct specialization automatically. Three forms are supported:
///
/// 1. `tmc::cancellable(awaitable, canceller)` where `canceller` is a nullary functor
/// that cancels the operation when invoked. It may return void (a sync cancel) or
/// an awaitable (an async cancel, which `select()` awaits). The canceller is
/// stored by value and invoked only for losers, so an awaitable-returning canceller
/// builds its awaitable only when the cancel is actually needed. Therefore, it is safe to
/// pass a capturing coroutine lambda here — it will be stored by value and outlive the
/// awaited invocation.
/// ```
/// // sync cancel
/// tmc::cancellable(timer.async_wait(tmc::aw_asio), [&]{ timer.cancel(); })
///
/// // async cancel via a functor lazily returning an awaitable
/// tmc::cancellable(op.async_do(), [&]{ return op.async_cancel(); })
///
/// // async cancel via a functor lazily returning a task
/// tmc::cancellable(op.async_do(), [&]() -> tmc::task<void> {
///   co_await op.async_cancel();
/// })
/// ```
///
/// 2. `tmc::cancellable(awaitable, object)` where `object` exposes a `.cancel()` method.
/// The `.cancel()` method may be sync or async (awaitable).
/// ```
/// // sync cancel - timer.cancel() runs synchronously (an Asio I/O object)
/// tmc::cancellable(timer.async_wait(tmc::aw_asio), timer)
///
/// // async cancel - op.cancel() returns an awaitable, which select() awaits
/// tmc::cancellable(op.async_do(), op)
/// ```
///
/// 3. `tmc::cancellable(object)` where `object` is itself both the awaitable and
/// the cancellation handle: it can be co_awaited and also exposes a `.cancel()`
/// method. The `.cancel()` method can be sync or async (awaitable); `select()` will
/// automatically await it if needed. This constructor only accepts lvalues or non-movable
/// rvalues. Movable rvalues are rejected to prevent lifetime issues.
/// ```
/// // sync cancel - op.cancel() runs synchronously
/// tmc::cancellable(op)
///
/// // async cancel - op2.cancel() returns an awaitable, which select() awaits
/// tmc::cancellable(op2)
/// ```
///
/// In every form, the cancellation action must be safe to run even
/// after the operation has already completed (it may race with completion) and
/// should not throw. `select()` runs the canceller of each losing awaitable
/// exactly once.
///
/// Value category is forwarded throughout: a movable rvalue is owned by value;
/// an lvalue or non-movable rvalue is borrowed (held by reference) and must
/// outlive the `tmc::select()` call. Constructor CTAD handles this automatically.
template <typename Awaitable, typename Canceller> class cancellable {
  Awaitable awaitable;
  // Empty cancellers (a captureless lambda, or `cancel_self_t`) cost no storage.
  TMC_NO_UNIQUE_ADDRESS Canceller canceller;

  template <typename... A, typename... C>
  friend tmc::task<std::variant<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<A>::result_type>...>>
  select(tmc::cancellable<A, C>... Pairs);

  template <typename A, typename C>
  friend consteval bool tmc::detail::canceller_is_async();

public:
  /// `tmc::cancellable(awaitable, canceller)` where `canceller` is a nullary functor
  /// that cancels the operation when invoked. It may return void (a sync cancel)
  /// or an awaitable (an async cancel, which `select()` awaits).
  ///
  /// The canceller is stored by value and invoked only for losers, so an
  /// awaitable-returning canceller builds its awaitable only when the cancel is actually
  /// needed. Therefore, it is safe to pass a capturing coroutine lambda here — it will be
  /// stored by value and outlive the awaited invocation.
  /// ```
  /// // sync cancel
  /// tmc::cancellable(timer.async_wait(tmc::aw_asio), [&]{ timer.cancel(); })
  ///
  /// // async cancel via a functor lazily returning an awaitable
  /// tmc::cancellable(op.async_do(), [&]{ return op.async_cancel(); })
  ///
  /// // async cancel via a functor lazily returning a task
  /// tmc::cancellable(op.async_do(), [&]() -> tmc::task<void> {
  ///   co_await op.async_cancel();
  /// })
  /// ```
  template <typename A, typename C>
    requires(tmc::traits::executable_kind_v<std::decay_t<C>> ==
             tmc::traits::executable_kind::CALLABLE)
  cancellable(A&& Aw, C&& Cancel)
      : awaitable(static_cast<A&&>(Aw)), canceller(static_cast<C&&>(Cancel)) {}

  /// `tmc::cancellable(awaitable, object)` where `object` exposes a `.cancel()`
  /// method. The `.cancel()` method may be sync or async (awaitable).
  /// ```
  /// // sync cancel - timer.cancel() runs synchronously (an Asio I/O object)
  /// tmc::cancellable(timer.async_wait(tmc::aw_asio), timer)
  ///
  /// // async cancel - op.cancel() returns an awaitable, which select() awaits
  /// tmc::cancellable(op.async_do(), op)
  template <typename A, typename C>
    requires(tmc::traits::executable_kind_v<std::decay_t<C>> ==
               tmc::traits::executable_kind::UNKNOWN &&
             requires(C& Obj) { Obj.cancel(); })
  cancellable(A&& Aw, C&& Obj)
      : awaitable(static_cast<A&&>(Aw)), canceller{static_cast<C&&>(Obj)} {}

  /// `tmc::cancellable(object)` where `object` is itself both the awaitable and
  /// the cancellation handle: it can be co_awaited and also exposes a `.cancel()`
  /// method. The `.cancel()` method can be sync or async (awaitable); `select()` will
  /// automatically await it if needed. This constructor only accepts lvalues or
  /// non-movable rvalues. Movable rvalues would be unsafe here, and are deliberately
  /// rejected.
  template <typename A>
    requires(tmc::detail::is_awaitable<std::remove_reference_t<A>> &&
             requires(A& Obj) { Obj.cancel(); } &&
             std::is_reference_v<tmc::detail::forward_awaitable<A>>)
  explicit cancellable(A&& Obj) : awaitable(static_cast<A&&>(Obj)), canceller{} {}

  /// This overload is invalid; it exists only to provide a useful compilation
  /// error. It rejects passing an awaitable directly as the canceller (the second
  /// argument). The canceller should instead be a nullary functor that returns the
  /// awaitable, e.g. `[&]{ return op.async_cancel(); }`. It will be lazily invoked as
  /// needed.
  template <typename A, typename C>
    requires(tmc::traits::executable_kind_v<std::decay_t<C>> ==
             tmc::traits::executable_kind::AWAITABLE)
  cancellable(A&& Aw, C&&) : awaitable(static_cast<A&&>(Aw)), canceller{} {
    static_assert(
      tmc::traits::executable_kind_v<std::decay_t<C>> !=
        tmc::traits::executable_kind::AWAITABLE,
      "The second argument to tmc::cancellable() should not be an awaitable. "
      "Instead, pass a nullary functor that returns the awaitable, e.g. "
      "[&]{ return op.async_cancel(); }. It will be lazily invoked as needed."
    );
  }
};

// Deduction guides pick the storage types that the matching constructor fills:
// the awaitable is forwarded (owned-by-value for a movable rvalue, by-reference
// otherwise); the canceller is stored as-is (form 1), wrapped in a
// `cancel_caller` (form 2), or replaced by the empty `cancel_self_t` (form 3).
// Their constraints mirror the constructors' so the right type is deduced.
template <typename A, typename C>
  requires(
    tmc::traits::executable_kind_v<std::decay_t<C>> ==
    tmc::traits::executable_kind::CALLABLE
  )
cancellable(A&&, C&&) -> cancellable<tmc::detail::forward_awaitable<A>, std::decay_t<C>>;

template <typename A, typename C>
  requires(
    tmc::traits::executable_kind_v<std::decay_t<C>> ==
      tmc::traits::executable_kind::UNKNOWN &&
    requires(C& Obj) { Obj.cancel(); }
  )
cancellable(A&&, C&&) -> cancellable<
  tmc::detail::forward_awaitable<A>,
  tmc::detail::cancel_caller<tmc::detail::forward_awaitable<C>>>;

template <typename A>
  requires(
    tmc::detail::is_awaitable<std::remove_reference_t<A>> &&
    requires(A& Obj) { Obj.cancel(); } &&
    std::is_reference_v<tmc::detail::forward_awaitable<A>>
  )
cancellable(A&&)
  -> cancellable<tmc::detail::forward_awaitable<A>, tmc::detail::cancel_self_t>;

// Diagnostic guide: routes the "awaitable passed as the canceller" mistake to
// the diagnostic constructor, which emits a readable static_assert. Uses a valid no-op
// canceller type so that the only error emitted is the static_assert.
template <typename A, typename C>
  requires(
    tmc::traits::executable_kind_v<std::decay_t<C>> ==
    tmc::traits::executable_kind::AWAITABLE
  )
cancellable(A&&, C&&)
  -> cancellable<tmc::detail::forward_awaitable<A>, tmc::detail::noop_func>;

namespace detail {
// Compile-time check whether the canceller of a `tmc::cancellable` returns an awaitable.
template <typename Awaitable, typename Canceller> consteval bool canceller_is_async() {
  using Pair = tmc::cancellable<Awaitable, Canceller>;
  if constexpr (std::is_same_v<Canceller, cancel_self_t>) {
    // self-cancel (form 3)
    return tmc::traits::is_awaitable<decltype(std::declval<Pair&>().awaitable.cancel())>;
  } else {
    // a callable canceller (form 1) or object with `cancel()` method (form 2)
    return tmc::traits::is_awaitable<decltype(std::declval<Pair&>().canceller())>;
  }
}
} // namespace detail

/// Awaits all of the provided awaitables and returns the result of the first
/// one to complete in a `std::variant`. The `index()` of the returned variant
/// indicates which awaitable completed first; its value holds that awaitable's
/// result. A void-returning awaitable's slot holds a `std::monostate`.
///
/// Each awaitable must be paired with a canceller using `tmc::cancellable()`.
/// As soon as the first awaitable completes (the winner), the cancellers of all of the
/// others (losers) are run; cancellers that return awaitables (async cancel) are awaited.
/// Results from losers are discarded.
///
/// If multiple awaitables complete simultaneously, the winner is the awaitable with the
/// lower index (leftmost parameter). All losers are cancelled, even those that may have
/// already completed. Therefore, it must be safe to cancel a completed awaitable.
///
/// Implementation note: `select()` waits for cancelled losers to complete before
/// returning. This is required since wrapped operations borrow storage from this
/// coroutine frame, so they must all complete before it is destroyed. Consequently, a
/// canceller that cannot truly stop its operation will delay the return of `select()`
/// until that operation completes on its own.
template <typename... Awaitable, typename... Canceller>
tmc::task<std::variant<tmc::detail::void_to_monostate<
  typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>...>>
select(tmc::cancellable<Awaitable, Canceller>... Pairs) {
  static_assert(sizeof...(Awaitable) > 0, "select() requires at least one awaitable.");
  static_assert(
    sizeof...(Awaitable) < TMC_PLATFORM_BITS,
    "select() supports at most 63 awaitables (31 on 32-bit platforms)."
  );
  using variant_type = std::variant<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>...>;
  constexpr size_t Count = sizeof...(Awaitable);

  // Forward each awaitable with its original value category so the awaitable's own
  // lvalue/rvalue qualification is respected. The mux_tuple makes each result
  // available as it becomes ready, so we can return as soon as the first one
  // completes.
  tmc::mux_tuple mux(static_cast<Awaitable&&>(Pairs.awaitable)...);

  // Wait for at least one operation to complete.
  size_t winner = co_await mux;

  // Move the winner's result into the variant slot for its index.
  std::optional<variant_type> result;
  auto storeWinner = [&]<size_t I>(std::integral_constant<size_t, I>) {
    using VarElem = std::variant_alternative_t<I, variant_type>;
    using Stored = std::remove_reference_t<decltype(mux.template get<I>())>;
    if constexpr (std::is_same_v<Stored, VarElem>) {
      result.emplace(std::in_place_index<I>, std::move(mux.template get<I>()));
    } else {
      // Non-default-constructible results are stored wrapped in std::optional.
      result.emplace(std::in_place_index<I>, std::move(*mux.template get<I>()));
    }
  };
  [&]<size_t... I>(std::index_sequence<I...>) {
    ((winner == I ? (storeWinner(std::integral_constant<size_t, I>{}), void()) : void()),
     ...);
  }(std::make_index_sequence<Count>{});

  // Cancel every awaitable except the winner. The cancellation action of a loser is, per
  // the cancellable forms:
  // - self-cancel (form 3): `Pair.awaitable.cancel()`;
  // - a callable canceller (form 1) or `cancel_caller` (form 2):
  //   `Pair.canceller()`.
  //
  // This is split on whether ANY canceller is async; a fold expression cannot contain a
  // per-element `if constexpr` check, so it must go outside.
  if constexpr (!(tmc::detail::canceller_is_async<Awaitable, Canceller>() || ...)) {
    // All cancellations are sync. The fold expression is a plain call.
    size_t idx = 0;
    auto cancelSync = [](auto& Pair) {
      if constexpr (std::is_same_v<
                      std::remove_reference_t<decltype(Pair.canceller)>,
                      tmc::detail::cancel_self_t>) {
        Pair.awaitable.cancel();
      } else {
        Pair.canceller();
      }
    };
    (((idx++ == winner) ? void() : cancelSync(Pairs)), ...);
  } else {
    // At least once cancellation is async. The fold expression's `co_await` must resolve
    // for all cancellations, so sync cancellations are projected with a dummy awaitable.
    size_t idx = 0;
    auto cancelAsync = [](auto& Pair) -> decltype(auto) {
      using Canceller_t = std::remove_reference_t<decltype(Pair.canceller)>;
      if constexpr (std::is_same_v<Canceller_t, tmc::detail::cancel_self_t>) {
        if constexpr (tmc::traits::is_awaitable<decltype(Pair.awaitable.cancel())>) {
          return Pair.awaitable.cancel();
        } else {
          Pair.awaitable.cancel();
          return tmc::detail::noop_awaitable{};
        }
      } else {
        if constexpr (tmc::traits::is_awaitable<decltype(Pair.canceller())>) {
          return Pair.canceller();
        } else {
          Pair.canceller();
          return tmc::detail::noop_awaitable{};
        }
      }
    };
    (((idx++ == winner) ? void() : (void)(co_await cancelAsync(Pairs))), ...);
  }

  // Drain the remaining (now-cancelled) awaitables. Their results are
  // discarded, but they must all complete before `mux` is destroyed.
  for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
    // discard
  }

  co_return std::move(*result);
}
} // namespace tmc
