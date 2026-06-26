// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/spawn_tuple.hpp"               // spawn_tuple, void_to_monostate
#include "tmc/task.hpp"
#include "tmc/traits.hpp" // is_awaitable, executable_kind_v

#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace tmc {
namespace detail {
// Marker used as the `Canceller` of a `cancellable_pair` when the awaitable is
// its own cancellation handle (the single-argument `tmc::cancellable()`
// overload). It is empty, so combined with TMC_NO_UNIQUE_ADDRESS on the pair's
// canceller member it occupies no storage. `select()` detects it at compile
// time and cancels by calling `.cancel()` on the awaitable itself.
struct cancel_self_t {};

// Pairs an awaitable with a callable that can cancel it. Produced by
// `tmc::cancellable()` and consumed by `tmc::select()`. Not intended to be
// named, stored, or constructed directly.
template <typename Awaitable, typename Canceller> struct cancellable_pair {
  Awaitable awaitable;
  // Empty cancellers (a captureless lambda, or `cancel_self_t`) cost no storage.
  TMC_NO_UNIQUE_ADDRESS Canceller canceller;
};

// A nullary canceller that holds a cancellation target and invokes its
// `.cancel()` method, forwarding whatever that method returns. `Target` is
// instantiated as `forward_awaitable<T>`, so the target is owned by value when
// it was passed as a movable rvalue, and held by reference (lvalue or
// non-movable rvalue) otherwise. Produced by the object-taking overloads of
// `tmc::cancellable()`. The forwarded result lets `select()` detect an awaitable
// `.cancel()` (an asynchronous cancel) and await it.
template <typename Target> struct cancel_caller {
  Target target;
  decltype(auto) operator()() { return target.cancel(); }
};

// A trivial, always-ready awaiter used by `select()` only in its slow (has an
// asynchronous canceller) cancellation branch, for any *synchronous* canceller
// mixed into that branch: the cancel has already run by the time this is awaited,
// so awaiting it does nothing.
struct cancel_noop : tmc::detail::AwaitTagNoGroupAsIs {
  static constexpr bool await_ready() noexcept { return true; }
  static void await_suspend(std::coroutine_handle<>) noexcept {}
  static void await_resume() noexcept {}
};

// Detects, at compile time, whether the canceller of a `cancellable_pair` is
// *asynchronous* (its cancellation action is itself awaitable), in the same three
// cases that `select()`'s inline cancellation handles:
//  - self-cancel (overload 3): `Pair.awaitable.cancel()` returns an awaitable;
//  - a task / awaitable canceller (overload 1): always asynchronous;
//  - a callable canceller (overload 1) or `cancel_caller` (overload 2):
//    `Pair.canceller()` returns an awaitable.
// `select()` uses this to choose between a purely synchronous cancellation fold
// (no `co_await`, no coroutine frames) and one that must suspend.
template <typename Awaitable, typename Canceller> consteval bool canceller_is_async() {
  using Pair = cancellable_pair<Awaitable, Canceller>;
  if constexpr (std::is_same_v<Canceller, cancel_self_t>) {
    return tmc::traits::is_awaitable<decltype(std::declval<Pair&>().awaitable.cancel())>;
  } else if constexpr (tmc::traits::is_awaitable<Canceller>) {
    return true;
  } else {
    return tmc::traits::is_awaitable<decltype(std::declval<Pair&>().canceller())>;
  }
}

/// Releases the canceller of the *winning* pair, which `select()` never runs.
/// A task / awaitable canceller (overload 1) owns a coroutine frame that would
/// otherwise leak (and `tmc::task` asserts in its destructor unless consumed),
/// so it is destroyed in place without being awaited. Every other canceller (a
/// plain callable, the empty `cancel_self_t`, or `cancel_caller`) is a value
/// that cleans up via its own destructor, so this is a no-op for them.
template <typename Awaitable, typename Canceller>
TMC_FORCE_INLINE inline void
select_discard_canceller(cancellable_pair<Awaitable, Canceller>& Pair) {
  if constexpr (tmc::traits::is_awaitable<Canceller> &&
                std::is_convertible_v<Canceller&&, std::coroutine_handle<>>) {
    std::coroutine_handle<> Frame = static_cast<Canceller&&>(Pair.canceller);
    Frame.destroy();
  }
}
} // namespace detail

/// Pairs an `Awaitable` with a `Canceller` for use with `tmc::select()`.
///
/// `Canceller` is the cancellation action for the operation backing `Awaitable`.
/// It may be either:
///  - a nullary callable (a *function*) - typically a lambda capturing a
///    reference to the object that owns the operation - that requests
///    cancellation when invoked. For example, an Asio operation can be cancelled
///    by capturing its I/O object:
///    ```
///    tmc::cancellable(timer.async_wait(tmc::aw_asio), [&]{ timer.cancel(); })
///    ```
///  - an awaitable (a *task*) that, when awaited, performs the cancellation. Use
///    this for an asynchronous cancel. The callable form may also *return* an
///    awaitable; either way `select()` awaits it before proceeding.
///
/// (A *task* or *function* is detected via `tmc::traits::executable_kind_v`.)
///
/// The cancellation action must be safe to run even after the operation has
/// already completed (it may race with completion) and should not throw.
/// `select()` runs the canceller of each losing awaitable exactly once.
template <typename Awaitable, typename Canceller>
  requires(
    tmc::traits::executable_kind_v<std::decay_t<Canceller>> !=
    tmc::traits::executable_kind::UNKNOWN
  )
tmc::detail::cancellable_pair<
  tmc::detail::forward_awaitable<Awaitable>, std::decay_t<Canceller>>
cancellable(Awaitable&& Aw, Canceller&& Cancel) {
  return {static_cast<Awaitable&&>(Aw), static_cast<Canceller&&>(Cancel)};
}

/// Convenience overload of `tmc::cancellable()` that pairs an `Awaitable` with
/// an object that exposes a `.cancel()` member function. A canceller that calls
/// `Obj.cancel()` is synthesized automatically. For example, an Asio operation
/// can be paired directly with its I/O object:
/// ```
/// tmc::cancellable(timer.async_wait(tmc::aw_asio), timer)
/// ```
///
/// `Obj.cancel()` may be synchronous (e.g. return `void`) or return an awaitable
/// for an asynchronous cancel; `select()` awaits the latter before proceeding.
///
/// `Obj`'s value category is forwarded (like `Awaitable`): a movable rvalue is
/// moved into the canceller, while an lvalue (or a non-movable rvalue) is held
/// by reference and must outlive the `tmc::select()` call. This overload is
/// selected only for objects that are neither a task nor a function (otherwise
/// the task/function overload above is used) - i.e. a plain object whose only
/// relevant member is `.cancel()`.
template <typename Awaitable, typename Cancellable>
  requires(
    tmc::traits::executable_kind_v<std::decay_t<Cancellable>> ==
      tmc::traits::executable_kind::UNKNOWN &&
    requires(Cancellable& C) { C.cancel(); }
  )
auto cancellable(Awaitable&& Aw, Cancellable&& Obj) {
  return tmc::cancellable(
    static_cast<Awaitable&&>(Aw),
    tmc::detail::cancel_caller<tmc::detail::forward_awaitable<Cancellable>>{
      static_cast<Cancellable&&>(Obj)
    }
  );
}

/// Convenience overload of `tmc::cancellable()` for an object that is itself
/// both the awaitable and the cancellation handle: it can be co_awaited and
/// also exposes a `.cancel()` method. The object is awaited and cancelled (via
/// `Obj.cancel()`) in place. As with the other overloads, `Obj.cancel()` may be
/// synchronous or return an awaitable for an asynchronous cancel (which
/// `select()` awaits before proceeding).
///
/// `Obj`'s value category is forwarded for both storage and awaiting. Storage:
/// an lvalue or a non-movable rvalue is held by reference (and must outlive the
/// `tmc::select()` call - a temporary passed here lives for the duration of the
/// enclosing `co_await select(...)`). Awaiting: the awaitable is awaited with
/// its original value category, so lvalue-qualified (re-awaitable) and
/// rvalue-qualified (consume-once) awaitables are both supported.
///
/// For cancellation to reach the awaitable in place it must remain valid after
/// being awaited, which holds only for borrowed (lvalue or non-movable-rvalue)
/// inputs. A *movable rvalue* would instead be owned by value and moved into the
/// await machinery, leaving cancellation to target a moved-from husk; this
/// overload therefore rejects movable rvalues at compile time (see the
/// `is_reference_v<forward_awaitable<...>>` constraint below). Pass such an
/// awaitable as an lvalue, or use the two-argument overload with a separate
/// stable cancel object.
///
/// No separate canceller is stored: the pair uses the empty `cancel_self_t`
/// marker (which costs no space), and `select()` cancels by calling
/// `Obj.cancel()` on the awaitable directly.
template <typename AwaitableCancellable>
  requires(
    tmc::detail::is_awaitable<std::remove_reference_t<AwaitableCancellable>> &&
    requires(AwaitableCancellable& C) { C.cancel(); } &&
    // The awaitable is awaited in place and cancelled via its own `.cancel()`.
    // For cancellation to reach the live object, the pair must *borrow* it
    // (store a reference) rather than own it: `forward_awaitable` owns a movable
    // rvalue by value, which then migrates into `spawn_tuple` and leaves the
    // pair holding a moved-from husk, so cancelling a loser would target the
    // husk and `select()` would hang in its drain loop. A movable awaitable is
    // therefore rejected here at compile time (pass it as an lvalue, or use the
    // two-argument overload with a separate stable cancel object). Lvalues and
    // non-movable rvalues forward to a reference and are accepted.
    std::is_reference_v<tmc::detail::forward_awaitable<AwaitableCancellable>>
  )
auto cancellable(AwaitableCancellable&& Obj) {
  return tmc::detail::cancellable_pair<
    tmc::detail::forward_awaitable<AwaitableCancellable>, tmc::detail::cancel_self_t>{
    static_cast<AwaitableCancellable&&>(Obj), {}
  };
}

/// Awaits all of the provided awaitables and returns the result of the first
/// one to complete, in a `std::variant`. The `index()` of the returned variant
/// indicates which awaitable completed first; its value holds that awaitable's
/// result. A void-returning awaitable's slot is represented by `std::monostate`.
///
/// Each awaitable must be paired with a canceller using `tmc::cancellable()`.
/// As soon as the first awaitable completes, the cancellers of all of the
/// others are run; a canceller that is itself awaitable (an asynchronous cancel)
/// is awaited. `select()` then waits for those remaining awaitables to finish -
/// discarding their results - before returning. This drain is mandatory: the
/// wrapped operations borrow storage from this coroutine frame, so they must all
/// complete before it is destroyed. Consequently, a canceller that cannot truly
/// stop its operation will delay the return of `select()` until that operation
/// completes on its own.
///
/// If multiple awaitables complete simultaneously, the result from the awaitable with the
/// lower index is chosen. All non-chosen awaitables are cancelled - even those that may
/// have already been completed. It must be safe to cancel a completed awaitable.
template <typename... Awaitable, typename... Canceller>
tmc::task<std::variant<tmc::detail::void_to_monostate<
  typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>...>>
select(tmc::detail::cancellable_pair<Awaitable, Canceller>... Pairs) {
  static_assert(sizeof...(Awaitable) > 0, "select() requires at least one awaitable.");
  using variant_type = std::variant<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>...>;
  constexpr size_t Count = sizeof...(Awaitable);

  // Spawn all of the awaitables and prepare to receive their results one at a
  // time, in completion order. Each awaitable is forwarded with its original
  // value category (recovered from the pair's storage type by static_cast), so
  // the awaitable's own lvalue/rvalue qualification is respected.
  auto each =
    tmc::spawn_tuple(static_cast<Awaitable&&>(Pairs.awaitable)...).result_each();

  // Wait for the first awaitable to become ready. Because Count > 0, this is
  // guaranteed to return a real index rather than end().
  size_t winner = co_await each;

  // Move the winner's result into the variant slot for its index.
  std::optional<variant_type> result;
  auto storeWinner = [&]<size_t I>(std::integral_constant<size_t, I>) {
    using VarElem = std::variant_alternative_t<I, variant_type>;
    using Stored = std::remove_reference_t<decltype(each.template get<I>())>;
    if constexpr (std::is_same_v<Stored, VarElem>) {
      result.emplace(std::in_place_index<I>, std::move(each.template get<I>()));
    } else {
      // Non-default-constructible results are stored wrapped in std::optional.
      result.emplace(std::in_place_index<I>, std::move(*each.template get<I>()));
    }
  };
  [&]<size_t... I>(std::index_sequence<I...>) {
    ((winner == I ? (storeWinner(std::integral_constant<size_t, I>{}), void()) : void()),
     ...);
  }(std::make_index_sequence<Count>{});

  // Cancel every awaitable except the winner. The winner's canceller is never
  // run, so it is discarded instead (freeing it if it owns a frame). The
  // cancellation action of a loser is, per the three `cancellable()` overloads:
  //  - self-cancel (overload 3): `Pair.awaitable.cancel()`;
  //  - a task / awaitable canceller (overload 1): the canceller itself;
  //  - a callable canceller (overload 1) or `cancel_caller` (overload 2):
  //    `Pair.canceller()`.
  //
  // This is split on whether ANY canceller is asynchronous, so that a canceller
  // that is not awaitable is cancelled by a plain function call with no
  // `co_await`:
  //  - If none are asynchronous (the common case), cancellation is purely
  //    synchronous - a fold of plain calls, with no `co_await` and no coroutine
  //    frames at all.
  //  - If at least one is asynchronous, we must suspend, so each loser is
  //    cancelled via `co_await`. A synchronous canceller mixed in runs in place
  //    and yields the empty, always-ready `cancel_noop` (a free `co_await`); an
  //    asynchronous canceller's awaitable is awaited directly, with no
  //    intermediate `tmc::task<void>` wrapper. Awaiting only the async ones while
  //    skipping `co_await` for the sync ones is not possible here: a fold
  //    expression cannot host the per-element `if constexpr`.
  if constexpr (!(tmc::detail::canceller_is_async<Awaitable, Canceller>() || ...)) {
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
    (((idx++ == winner) ? tmc::detail::select_discard_canceller(Pairs)
                        : cancelSync(Pairs)),
     ...);
  } else {
    size_t idx = 0;
    // Returns the awaitable to await (asynchronous cancel), or runs the
    // synchronous cancel in place and returns the empty `cancel_noop`.
    auto cancelAsync = [](auto& Pair) -> decltype(auto) {
      using Canceller_t = std::remove_reference_t<decltype(Pair.canceller)>;
      if constexpr (std::is_same_v<Canceller_t, tmc::detail::cancel_self_t>) {
        if constexpr (tmc::traits::is_awaitable<decltype(Pair.awaitable.cancel())>) {
          return Pair.awaitable.cancel();
        } else {
          Pair.awaitable.cancel();
          return tmc::detail::cancel_noop{};
        }
      } else if constexpr (tmc::traits::is_awaitable<Canceller_t>) {
        return static_cast<Canceller_t&&>(Pair.canceller);
      } else {
        if constexpr (tmc::traits::is_awaitable<decltype(Pair.canceller())>) {
          return Pair.canceller();
        } else {
          Pair.canceller();
          return tmc::detail::cancel_noop{};
        }
      }
    };
    (((idx++ == winner) ? tmc::detail::select_discard_canceller(Pairs)
                        : (void)(co_await cancelAsync(Pairs))),
     ...);
  }

  // Drain the remaining (now-cancelled) awaitables. Their results are
  // discarded, but they must all complete before `each` is destroyed.
  for (size_t i = co_await each; i != each.end(); i = co_await each) {
    // discard
  }

  co_return std::move(*result);
}
} // namespace tmc
