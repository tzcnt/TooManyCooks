// Copyright (c) 2022 Klemens Morgenstern (klemens.morgenstern@gmx.net)
// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// inspired by
// https://github.com/boostorg/cobalt/blob/develop/include/boost/cobalt/op.hpp

#pragma once
#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/ex_any.hpp"

#ifdef TMC_USE_BOOST_ASIO
#include <boost/asio/async_result.hpp>
#else
#include <asio/async_result.hpp>
#endif

#include <coroutine>
#include <tuple>

namespace tmc {
namespace detail {
struct AwAsioTag {};
} // namespace detail

template <typename Awaitable> struct aw_asio_impl;

/// Base class used to implement TMC awaitables for Asio operations.
template <typename... ResultArgs> class aw_asio_base {
protected:
  tmc::detail::awaitable_customizer<std::tuple<ResultArgs...>> customizer;

  struct callback {
    // The lifetime of callback may outlive the lifetime of aw_asio, so move the
    // customizer into it. Asio will move this callback into its own storage.
    tmc::detail::awaitable_customizer<std::tuple<ResultArgs...>> customizer;
    template <typename... ResultArgs_> void operator()(ResultArgs_&&... Args) {
      if constexpr (std::is_default_constructible_v<
                      std::tuple<ResultArgs...>>) {
        *customizer.result_ptr =
          std::tuple<ResultArgs...>(std::forward<ResultArgs_>(Args)...);
      } else {
        customizer.result_ptr->emplace(static_cast<ResultArgs_&&>(Args)...);
      }

      auto next = customizer.resume_continuation();
      if (next != std::noop_coroutine()) {
        next.resume();
      }
    }
  };

  void async_initiate() { initiate_await(callback{customizer}); }

  virtual void initiate_await(callback Callback) = 0;

  aw_asio_base() {}

public:
  aw_asio_base(const aw_asio_base&) = default;
  aw_asio_base(aw_asio_base&&) = default;
  aw_asio_base& operator=(const aw_asio_base&) = default;
  aw_asio_base& operator=(aw_asio_base&&) = default;
  virtual ~aw_asio_base() = default;
};

namespace detail {
template <typename T>
concept IsAwAsio = std::is_base_of_v<tmc::detail::AwAsioTag, T>;

template <IsAwAsio Awaitable> struct awaitable_traits<Awaitable> {
  using result_type = typename Awaitable::result_type;
  using self_type = Awaitable;

  // Values controlling the behavior when awaited directly in a tmc::task
  static decltype(auto) get_awaiter(self_type&& awaitable) {
    return std::forward<self_type>(awaitable).operator co_await();
  }

  // Values controlling the behavior when wrapped by a utility function
  // such as tmc::spawn_*()
  static constexpr configure_mode mode = ASYNC_INITIATE;
  static void async_initiate(
    self_type&& awaitable, [[maybe_unused]] tmc::ex_any* Executor,
    [[maybe_unused]] size_t Priority
  ) {
    awaitable.async_initiate();
  }

  static void set_result_ptr(
    self_type& awaitable,
    tmc::detail::result_storage_t<typename Awaitable::result_type>* ResultPtr
  ) {
    awaitable.customizer.result_ptr = ResultPtr;
  }

  static void set_continuation(self_type& awaitable, void* Continuation) {
    awaitable.customizer.continuation = Continuation;
  }

  static void set_continuation_executor(self_type& awaitable, void* ContExec) {
    awaitable.customizer.continuation_executor = ContExec;
  }

  static void set_done_count(self_type& awaitable, void* DoneCount) {
    awaitable.customizer.done_count = DoneCount;
  }

  static void set_flags(self_type& awaitable, size_t Flags) {
    awaitable.customizer.flags = Flags;
  }
};
} // namespace detail

template <typename Awaitable> struct aw_asio_impl {
  // Keep an lvalue reference to the awaitable. The reference is only used
  // during await_suspend, but still depends on temporary lifetime extension
  // when used in some contexts. Safe as long as you don't call
  // aw_asio.operator co_await() on a temporary and try to save this for later.
  Awaitable& handle;
  tmc::detail::result_storage_t<typename Awaitable::result_type> result;

  friend Awaitable;
  aw_asio_impl(Awaitable& Handle) : handle(Handle) {}

  bool await_ready() { return false; }

  inline void await_suspend(std::coroutine_handle<> Outer) noexcept {
    handle.customizer.continuation = Outer.address();
    handle.customizer.result_ptr = &result;
    handle.async_initiate();
  }

  TMC_AWAIT_RESUME auto await_resume() noexcept {
    // Move the result out of the optional
    // (returns tuple<Result>, not optional<tuple<Result>>)
    if constexpr (std::is_default_constructible_v<
                    typename Awaitable::result_type>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }
};

struct aw_asio_t {
  constexpr aw_asio_t() {}

  // Adapts an executor to add the `aw_asio_t` completion token as the default.
  template <typename InnerExecutor>
  struct executor_with_default : InnerExecutor {
    typedef aw_asio_t default_completion_token_type;

    executor_with_default(const InnerExecutor& Executor) noexcept
        : InnerExecutor(Executor) {}

    template <typename InnerExecutor1>
    executor_with_default(
      const InnerExecutor1& Executor,
      typename std::enable_if<std::conditional<
        !std::is_same<InnerExecutor1, executor_with_default>::value,
        std::is_convertible<InnerExecutor1, InnerExecutor>,
        std::false_type>::type::value>::type = 0
    ) noexcept
        : InnerExecutor(Executor) {}
  };

  // Type alias to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename T>
  using as_default_on_t = typename T::template rebind_executor<
    executor_with_default<typename T::executor_type>>::other;

  // Function helper to adapt an I/O object to use `aw_asio_t` as its
  // default completion token type.
  template <typename AsioIoType>
  static typename std::decay_t<AsioIoType>::template rebind_executor<
    executor_with_default<typename std::decay_t<AsioIoType>::executor_type>>::
    other
    as_default_on(AsioIoType&& AsioIoObject) {
    return typename std::decay_t<AsioIoType>::template rebind_executor<
      executor_with_default<typename std::decay_t<AsioIoType>::executor_type>>::
      other(static_cast<AsioIoType&&>(AsioIoObject));
  }
};

// Static completion token object that tells asio to produce a TMC awaitable.
constexpr aw_asio_t aw_asio{};

} // namespace tmc

#ifdef TMC_USE_BOOST_ASIO
namespace boost::asio {
#else
namespace asio {
#endif

// Specialization of asio::async_result to produce a TMC awaitable
template <typename... ResultArgs>
struct async_result<tmc::aw_asio_t, void(ResultArgs...)> {
  /// TMC awaitable for an Asio operation
  template <typename Init, typename... InitArgs>
  class aw_asio final : public tmc::aw_asio_base<std::decay_t<ResultArgs>...>,
                        tmc::detail::AwAsioTag {
    friend async_result;
    friend tmc::detail::awaitable_traits<aw_asio>;
    friend tmc::aw_asio_impl<aw_asio>;
    using result_type = std::tuple<std::decay_t<ResultArgs>...>;

    Init initiation;
    std::tuple<InitArgs...> init_args;
    template <typename Init_, typename... InitArgs_>
    aw_asio(Init_&& Initiation, InitArgs_&&... Args)
        : initiation(static_cast<Init_&&>(Initiation)),
          init_args(static_cast<InitArgs_&&>(Args)...) {}

    void initiate_await(
      typename tmc::aw_asio_base<std::decay_t<ResultArgs>...>::callback Callback
    ) final override {
      std::apply(
        [&](InitArgs&&... Args) {
          std::move(initiation)(std::move(Callback), std::move(Args)...);
        },
        std::move(init_args)
      );
    }

    tmc::aw_asio_impl<aw_asio> operator co_await() && {
      return tmc::aw_asio_impl<aw_asio>(*this);
    }

  public:
    /// When co_awaited, the outer coroutine will resume on the provided
    /// executor.
    template <typename Exec>
    [[nodiscard]] aw_asio& resume_on(Exec&& Executor) & {
      this->customizer.continuation_executor =
        tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
      return *this;
    }
    /// When co_awaited, the outer coroutine will resume on the provided
    /// executor.
    template <typename Exec>
    [[nodiscard]] aw_asio& resume_on(Exec* Executor) & {
      this->customizer.continuation_executor =
        tmc::detail::get_executor_traits<Exec>::type_erased(*Executor);
      return *this;
    }

    /// When co_awaited, the outer coroutine will resume on the provided
    /// executor.
    template <typename Exec>
    [[nodiscard]] aw_asio&& resume_on(Exec&& Executor) && {
      this->customizer.continuation_executor =
        tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
      return std::move(*this);
    }
    /// When co_awaited, the outer coroutine will resume on the provided
    /// executor.
    template <typename Exec>
    [[nodiscard]] aw_asio&& resume_on(Exec* Executor) && {
      this->customizer.continuation_executor =
        tmc::detail::get_executor_traits<Exec>::type_erased(*Executor);
      return std::move(*this);
    }
  };

  // This doesn't actually initiate the operation, just returns the awaitable.
  // Initiation happens in aw_asio_base::await_suspend();
  template <typename Init, typename... InitArgs>
  static aw_asio<std::decay_t<Init>, std::decay_t<InitArgs>...>
  initiate(Init&& Initiation, tmc::aw_asio_t, InitArgs&&... Args) {
    return aw_asio<std::decay_t<Init>, std::decay_t<InitArgs>...>(
      static_cast<Init&&>(Initiation), static_cast<InitArgs&&>(Args)...
    );
  }
};

} // namespace asio
