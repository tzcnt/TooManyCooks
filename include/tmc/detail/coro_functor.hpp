// MIT License

// Copyright (c) 2023 Logan McDougall

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tmc {
class coro_functor {
  // use high bit of pointer for pointer tagging
  // low bit is not safe to use as function addresses may be unaligned
  static constexpr uintptr_t IS_FUNC_BIT = 1ULL << 60;
  static_assert(sizeof(void*) == 8); // requires 64-bit

  void* func; // coroutine address or function pointer. tagged via the above bit
  void* obj;  // pointer to functor object. will be null if func is not a member

public:
  /// Resumes the provided coroutine, or calls the provided function/functor.
  inline void operator()() noexcept {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(func);
    if ((funcAddr & IS_FUNC_BIT) == 0) {
      std::coroutine_handle<> coro =
        std::coroutine_handle<>::from_address(reinterpret_cast<void*>(funcAddr)
        );
      coro.resume();
    } else {
      // fixup the pointer by resetting the bit
      funcAddr = funcAddr & ~IS_FUNC_BIT;
      if (obj == nullptr) {
        void (*freeFunc)() = reinterpret_cast<void (*)()>(funcAddr);
        freeFunc();
      } else {
        void (*memberFunc)(void*, bool) =
          reinterpret_cast<void (*)(void*, bool)>(funcAddr);
        memberFunc(obj, true);
      }
    }
  }

  /// Returns true if this was constructed with a coroutine type.
  inline bool is_coroutine() noexcept {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(func);
    return (funcAddr & IS_FUNC_BIT) == 0;
  }

  /// Returns the pointer as a coroutine handle. This is only valid if this
  /// was constructed with a coroutine type. `as_coroutine()` will not
  /// convert a regular function into a coroutine.
  inline std::coroutine_handle<> as_coroutine() noexcept {
    return std::coroutine_handle<>::from_address(func);
  }

  /// Coroutine handle constructor
  template <typename T>
  coro_functor(const T& CoroutineHandle) noexcept
    requires(std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(
      std::coroutine_handle<>(CoroutineHandle).address()
    );
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr);
    obj = nullptr;
  }

  /// Free function void() constructor
  inline coro_functor(void (*FreeFunction)()) noexcept {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(FreeFunction);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = nullptr;
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
    requires(!std::is_same_v<std::remove_reference_t<T>, coro_functor> && !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr = reinterpret_cast<
      uintptr_t>(&cast_call_or_nothing<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = Functor;
  }

  /// Lvalue function object constructor. Copies the parameter into a
  /// new allocation owned by the coro_functor.
  template <typename T>
  coro_functor(const T& Functor) noexcept
    requires(!std::is_same_v<std::remove_reference_t<T>, coro_functor> && !std::is_convertible_v<T, std::coroutine_handle<>> && std::is_copy_constructible_v<T>)
  {
    uintptr_t funcAddr = reinterpret_cast<
      uintptr_t>(&cast_call_or_delete<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = new T(Functor);
  }

  /// Rvalue function object constructor. Moves the parameter into a
  /// new allocation owned by the coro_functor.
  template <typename T>
  coro_functor(T &&Functor) noexcept
    requires( // prevent lvalues from choosing this overload
              // https://stackoverflow.com/a/46936145/100443
        !std::is_reference_v<T> &&
        !std::is_same_v<std::remove_reference_t<T>, coro_functor> &&
        !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr = reinterpret_cast<
      uintptr_t>(&cast_call_or_delete<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = new T(std::move(Functor));
  }

  /// Default constructor is provided for use with data structures that
  /// initialize the passed-in type by reference.
  inline coro_functor() noexcept : obj{nullptr} {
#ifndef NDEBUG
    func = nullptr;
#endif
  }

  inline coro_functor(const coro_functor& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
  }

  inline coro_functor& operator=(const coro_functor& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
    return *this;
  }

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
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(func);
    if (obj == nullptr || (funcAddr & IS_FUNC_BIT) == 0) {
      return;
    }
    // fixup the pointer by resetting the bit
    funcAddr = funcAddr & ~IS_FUNC_BIT;
    void (*memberFunc)(void*, bool) =
      reinterpret_cast<void (*)(void*, bool)>(funcAddr);
    // pass false to cast_call_or_delete to delete the owned object
    // cast_call_or_nothing will ignore the parameter
    memberFunc(obj, false);
  }
};
} // namespace tmc
