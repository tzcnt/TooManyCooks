// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// coro_functor is a lightweight implementation of a move-only functor that can
// hold either a coroutine, a function pointer, or a function object pointer. It
// uses pointer tagging to denote the stored type.

// It requires a 64-bit system, as 32-bit systems don't have free address bits.

#pragma once

#include "tmc/detail/compat.hpp"

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <type_traits>

namespace tmc {
class coro_functor {
  static constexpr uintptr_t IS_COROUTINE = 0x0;
  static constexpr uintptr_t IS_FREE_FUNC = 0x1;

  // coroutine address or function pointer
  void* func;

  // pointer to functor object.
  // will be null if func is a coroutine.
  // will be 0x1 is func is a free function (not a class method & not a closure)
  void* obj;

public:
  /// Resumes the provided coroutine, or calls the provided function/functor.
  inline void operator()() const noexcept {
    uintptr_t mode = reinterpret_cast<uintptr_t>(obj);
    if (mode == IS_COROUTINE) {
      std::coroutine_handle<> coro =
        std::coroutine_handle<>::from_address(func);
      coro.resume();
    } else if (mode == IS_FREE_FUNC) {
      void (*freeFunc)() = reinterpret_cast<void (*)()>(func);
      freeFunc();
    } else {
      void (*memberFunc)(void*, bool) =
        reinterpret_cast<void (*)(void*, bool)>(func);
      memberFunc(obj, true);
    }
  }

  /// Returns true if this was constructed with a coroutine type.
  inline bool is_coroutine() noexcept {
    uintptr_t mode = reinterpret_cast<uintptr_t>(obj);
    return mode == IS_COROUTINE;
  }

  /// Returns the pointer as a coroutine handle. This is only valid if this
  /// was constructed with a coroutine type. `as_coroutine()` will not
  /// convert a regular function into a coroutine.
  inline std::coroutine_handle<> as_coroutine() noexcept {
    return std::coroutine_handle<>::from_address(func);
  }

  /// Coroutine handle constructor
  template <typename T>
  coro_functor(T&& CoroutineHandle) noexcept
    requires(std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    func = std::coroutine_handle<>(static_cast<T&&>(CoroutineHandle)).address();
    obj = nullptr;
  }

  /// Free function void() constructor
  inline coro_functor(void (*FreeFunction)()) noexcept {
    func = reinterpret_cast<void*>(FreeFunction);
    obj = reinterpret_cast<void*>(IS_FREE_FUNC);
  }

private:
  template <typename T>
  static void cast_call_or_nothing(void* TypeErasedObject, bool Call) {
    T* typedObj = static_cast<T*>(TypeErasedObject);
    if (Call) {
      typedObj->operator()();
    }
  }

  template <typename T>
  static void cast_call_or_delete(void* TypeErasedObject, bool Call) {
    T* typedObj = static_cast<T*>(TypeErasedObject);
    if (Call) {
      typedObj->operator()();
    } else {
      delete typedObj;
    }
  }

public:
  /// Pointer to function object constructor. The caller must manage the
  /// lifetime of the parameter and ensure that the pointer remains valid until
  /// operator() is called.
  template <typename T>
  coro_functor(T* Functor) noexcept
    requires(!std::is_same_v<std::remove_cvref_t<T>, coro_functor> && !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    func = reinterpret_cast<
      void*>(&cast_call_or_nothing<std::remove_reference_t<T>>);
    obj = reinterpret_cast<void*>(Functor);
  }

  // The following lvalue/rvalue reference constructors could be collapsed into
  // a single constructor using perfect forwarding. However, I prefer to make it
  // obvious to the caller which overload is being called, and how their data
  // will be treated, by the differing doc comments.

  /// Lvalue function object constructor. Copies the parameter into a
  /// new allocation owned by the coro_functor.
  template <typename T>
  coro_functor(const T& Functor) noexcept
    requires(!std::is_same_v<std::remove_cvref_t<T>, coro_functor> && !std::is_convertible_v<T, std::coroutine_handle<>> && std::is_copy_constructible_v<T>)
  {
    func =
      reinterpret_cast<void*>(&cast_call_or_delete<std::remove_reference_t<T>>);
    obj = reinterpret_cast<void*>(new T(Functor));
  }

  /// Rvalue function object constructor. Moves the parameter into a
  /// new allocation owned by the coro_functor.
  template <typename T>
  coro_functor(T &&Functor) noexcept
    requires( // prevent lvalues from choosing this overload
              // https://stackoverflow.com/a/46936145/100443
        !std::is_reference_v<T> &&
        !std::is_same_v<std::remove_cvref_t<T>, coro_functor> &&
        !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    func =
      reinterpret_cast<void*>(&cast_call_or_delete<std::remove_reference_t<T>>);
    obj = reinterpret_cast<void*>(new T(static_cast<T&&>(Functor)));
  }

  /// Default constructor is provided for use with data structures that
  /// initialize the passed-in type by reference.
  inline coro_functor() noexcept : obj{nullptr} {
#ifndef NDEBUG
    func = nullptr;
#endif
  }

  /// Not copy-constructible. Holds an owning pointer to the functor object.
  inline coro_functor(const coro_functor& Other) = delete;
  inline coro_functor& operator=(const coro_functor& Other) = delete;

  inline coro_functor(coro_functor&& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
#ifndef NDEBUG
    Other.func = nullptr;
#endif
    Other.obj = nullptr;
  }

  inline coro_functor& operator=(coro_functor&& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
#ifndef NDEBUG
    Other.func = nullptr;
#endif
    Other.obj = nullptr;
    return *this;
  }

  inline ~coro_functor() {
    uintptr_t mode = reinterpret_cast<uintptr_t>(obj);
    if (mode <= IS_FREE_FUNC) {
      return;
    }

    void (*memberFunc)(void*, bool) =
      reinterpret_cast<void (*)(void*, bool)>(func);
    // pass false to cast_call_or_delete to delete the owned object
    // cast_call_or_nothing will ignore the parameter
    memberFunc(obj, false);
  }
};
} // namespace tmc
