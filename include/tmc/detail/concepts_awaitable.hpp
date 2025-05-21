// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <optional>
#include <type_traits>

namespace tmc {
namespace detail {
/// Must be defined by each TMC executor. No default implementation is provided.
template <typename Executor> struct executor_traits;

// Non-default-constructible Results are wrapped in an optional.
template <typename Result>
using result_storage_t = std::conditional_t<
  std::is_default_constructible_v<Result>, Result, std::optional<Result>>;

template <typename Awaitable> struct unknown_awaitable_traits {
  // Try to guess at the awaiter type based on the expected function signatures.
  // This function is normally unevaluated and only used to get the decltype.
  template <typename T> static decltype(auto) guess_awaiter(T&& value) {
    if constexpr (requires { static_cast<T&&>(value).operator co_await(); }) {
      return static_cast<T&&>(value).operator co_await();
    } else if constexpr (requires {
                           operator co_await(static_cast<T&&>(value));
                         }) {
      return operator co_await(static_cast<T&&>(value));
    } else {
      return static_cast<T&&>(value);
    }
  }

  using awaiter_type = decltype(guess_awaiter(std::declval<Awaitable>()));

  // If you are looking at a compilation error on this line when awaiting a TMC
  // awaitable, you probably need to std::move() whatever you are co_await'ing.
  // co_await std::move(your_tmc_awaitable_variable_name)
  //
  // If you are awaiting a non-TMC awaitable, then you should consult the
  // documentation there to see why we can't deduce the awaiter type, or
  // specialize tmc::detail::awaitable_traits for it yourself.
  using result_type =
    std::remove_reference_t<decltype(std::declval<awaiter_type>().await_resume()
    )>;
};

enum configure_mode { TMC_TASK, COROUTINE, ASYNC_INITIATE, WRAPPER };

// The default implementation of awaitable_traits will wrap any unknown
// awaitables into a tmc::task trampoline that restores the awaiting task back
// to its original executor and priority. This prevents runtime errors when
// calling TMC utility functions (spawn, etc) in a task that has been
// unexpectedly moved to a non-TMC executor.
//
// However, this trampoline has a small runtime cost, so if you want to speed up
// your integration, you can specialize this to remove the trampoline.
template <typename Awaitable> struct awaitable_traits {
  static constexpr configure_mode mode = WRAPPER;

  // Try to guess at the result type based on the expected function signatures.
  // Awaiting is context-dependent, so this is not guaranteed to be correct.
  // If this doesn't behave as expected, you should specialize awaitable_traits
  // instead.
  using result_type =
    typename tmc::detail::unknown_awaitable_traits<Awaitable>::result_type;
};

// Details on how to specialize awaitable_traits:
/*
template <typename Awaitable> struct awaitable_traits {
{

// Define the result type of `co_await YourAwaitable;`
// This should be a value type, not a reference type.
using result_type = typename (result type of `co_await YourAwaitable;`);

// ## Declarations controlling the behavior when awaited directly in a tmc::task

// Tells the tmc::task await_transform how to get the awaitable type for this.
// It may be as simple as `return Awaitable.operator co_await();`
//
// However, by implementing this, you are committing to the contract that the
// awaiting TMC task will be resumed on its original executor, at its original
// priority. If you don't fulfill this contract, the program may unexpectedly
// crash.
//
// Implementing this is OPTIONAL; if unimplemented, each awaitable will be
// wrapped in a task that will automagically restore its executor and priority
// before returning, thus preventing errors out-of-the-box.

static awaiter_type get_awaiter(self_type& Awaitable) {
  return awaiter_type(Awaitable);
}

// ## Declarations controlling the behavior when wrapped by a utility function
// ## such as tmc::spawn_*()

// You MUST declare `static constexpr configure_mode mode;`
// If set to COROUTINE, when initiating the async process, the awaitable
// will be submitted to the TMC executor to be resumed.
// It may also be resumed directly using symmetric transfer.
// requires {std::coroutine_handle<> c = declval<YourAwaitable>();}

static constexpr configure_mode mode = COROUTINE;

// If set to ASYNC_INITIATE, you must define this function, which will be
// called to initiate the async process. The current TMC executor and priority
// will be passed in, but they are not required to be used.

static constexpr configure_mode mode = ASYNC_INITIATE;
static void async_initiate(
  Awaitable&& YourAwaitable, tmc::ex_any* Executor,
  size_t Priority
) {}

// If set to WRAPPER, the default behavior will be used, which is to wrap the
// awaitable in a task for submission.

static constexpr configure_mode mode = WRAPPER;

// If the mode is not WRAPPER, you must declare ALL of the following types and
// functions.

static void set_result_ptr(
  Awaitable& YourAwaitable,
  tmc::detail::result_storage_t<result_type>* ResultPtr
);

static void set_continuation(Awaitable& YourAwaitable, void* Continuation);

static void set_continuation_executor(Awaitable& YourAwaitable, void* ContExec);

static void set_done_count(Awaitable& YourAwaitable, void* DoneCount);

static void set_flags(Awaitable& YourAwaitable, size_t Flags);
};

*/
template <typename Awaitable>
using get_awaitable_traits = awaitable_traits<std::remove_cvref_t<Awaitable>>;

template <typename Awaitable>
using awaitable_result_t =
  typename awaitable_traits<std::remove_cvref_t<Awaitable>>::result_type;

/// Tag-based implementation of tmc::detail::awaitable_traits for
/// types that implement await_ready(), await_suspend(), await_resume()
/// directly. Inheriting from this prevents the tmc::detail::safe_wrap() wrapper
/// from being generated.
struct AwaitTagNoGroupAsIs {};

template <typename T>
concept HasAwaitTagNoGroupAsIs = std::is_base_of_v<AwaitTagNoGroupAsIs, T>;

template <HasAwaitTagNoGroupAsIs Awaitable> struct awaitable_traits<Awaitable> {
  static constexpr configure_mode mode = WRAPPER;

  static decltype(auto) get_awaiter(Awaitable&& awaitable) noexcept {
    return static_cast<Awaitable&&>(awaitable);
  }

  using result_type = std::remove_reference_t<
    decltype(get_awaiter(std::declval<Awaitable>()).await_resume())>;
};

/// Tag-based implementation of tmc::detail::awaitable_traits for
/// types that implement operator co_await for rvalues. Inheriting from this
/// prevents the tmc::detail::safe_wrap() wrapper from being generated.
/// This type is for awaitables that should be cast to rvalue and awaited only
/// once (the awaitable is consumed after it is awaited).
struct AwaitTagNoGroupCoAwait {};

template <typename T>
concept HasAwaitTagNoGroupCoAwait =
  std::is_base_of_v<AwaitTagNoGroupCoAwait, T>;

template <HasAwaitTagNoGroupCoAwait Awaitable>
struct awaitable_traits<Awaitable> {
  static constexpr configure_mode mode = WRAPPER;

  static decltype(auto) get_awaiter(Awaitable&& awaitable) noexcept {
    return static_cast<Awaitable&&>(awaitable).operator co_await();
  }

  using result_type = std::remove_reference_t<
    decltype(get_awaiter(std::declval<Awaitable>()).await_resume())>;
};

/// Tag-based implementation of tmc::detail::awaitable_traits for
/// types that implement operator co_await for lvalues only. Inheriting from
/// this prevents the tmc::detail::safe_wrap() wrapper from being generated.
/// This type is for awaitables that should be stored in an lvalue variable and
/// awaited multiple times.
struct AwaitTagNoGroupCoAwaitLvalue {};

template <typename T>
concept HasAwaitTagNoGroupCoAwaitLvalue =
  std::is_base_of_v<AwaitTagNoGroupCoAwaitLvalue, T>;

template <HasAwaitTagNoGroupCoAwaitLvalue Awaitable>
struct awaitable_traits<Awaitable> {
  static constexpr configure_mode mode = WRAPPER;

  static decltype(auto) get_awaiter(Awaitable& awaitable) noexcept {
    return awaitable.operator co_await();
  }

  using result_type = std::remove_reference_t<
    decltype(get_awaiter(std::declval<Awaitable&>()).await_resume())>;
};

template <typename T>
concept IsRange = requires(T a) {
  // This concept is somewhat incomplete - also need to test for
  // operator++ and operator*. However, along with the
  // iter_value_t template deduction, it is sufficient.
  a.begin() == a.end();
};

template <IsRange R> struct range_iter {
  using type = decltype(std::declval<R>().begin());
};

/// Given T&  -> holds T&
/// Given T&& -> holds T if T is move-constructible
/// Given T&& -> holds T&& if T is not move-constructible
template <typename T>
using forward_awaitable = std::conditional_t<
  std::is_rvalue_reference_v<T&&> &&
    std::is_move_constructible_v<std::decay_t<T>>,
  std::decay_t<T>, T&&>;

} // namespace detail
} // namespace tmc
