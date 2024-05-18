// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <type_traits>

namespace tmc {
class coro_functor32 {
  // use high bit of pointer for pointer tagging
  // low bit is not safe to use as function addresses may be unaligned
  static constexpr uintptr_t IS_FUNC_BIT = 1ULL << 60;
  static_assert(sizeof(void*) == 8); // requires 64-bit

  void* func; // coroutine address or function pointer. tagged via the above bit
  void* obj;  // pointer to functor object. will be null if func is not a member
  void* obj2; // currently unused
  void (*deleter)(void*); // typed deleter, used for owned objects

public:
  // Resumes the provided coroutine, or calls the provided function/functor. If
  // a functor was provided by rvalue reference (&&), the allocated functor
  // owned by this object will be deleted after this call.
  inline void operator()() const noexcept {
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
        void (*memberFunc)(void*) = reinterpret_cast<void (*)(void*)>(funcAddr);
        memberFunc(obj);
      }
    }
  }

  // Returns true if this was constructed with a coroutine type.
  inline bool is_coroutine() noexcept {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(func);
    return (funcAddr & IS_FUNC_BIT) == 0;
  }

  // Returns the pointer as a coroutine handle. This is only valid if this
  // was constructed with a coroutine type. `as_coroutine()` will not
  // convert a regular function into a coroutine.
  inline std::coroutine_handle<> as_coroutine() noexcept {
    return std::coroutine_handle<>::from_address(func);
  }

  // Coroutine handle constructor
  template <typename T>
  coro_functor32(T&& CoroutineHandle) noexcept
    requires(std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(
      std::coroutine_handle<>(static_cast<T&&>(CoroutineHandle)).address()
    );
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr);
    obj = nullptr;
    deleter = nullptr;
  }

  // Free function void() constructor
  inline coro_functor32(void (*FreeFunction)()) noexcept {
    uintptr_t funcAddr = reinterpret_cast<uintptr_t>(FreeFunction);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = nullptr;
    deleter = nullptr;
  }

private:
  template <typename T> static void cast_call(void* TypeErasedObject) {
    T* typedObj = reinterpret_cast<T*>(TypeErasedObject);
    typedObj->operator()();
  }

  template <typename T> static void cast_delete(void* TypeErasedObject) {
    T* typedObj = reinterpret_cast<T*>(TypeErasedObject);
    delete typedObj;
  }

public:
  // Pointer to function object constructor. The caller must manage the lifetime
  // of the parameter and ensure that the pointer remains valid until operator()
  // is called.
  template <typename T>
  coro_functor32(T* Functor) noexcept
    requires(!std::is_same_v<std::remove_cvref_t<T>, coro_functor32> && !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr =
      reinterpret_cast<uintptr_t>(&cast_call<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = Functor;
    deleter = nullptr;
  }

  // The following lvalue/rvalue reference constructors could be collapsed into
  // a single constructor using perfect forwarding. However, I prefer to make it
  // obvious to the caller which overload is being called, and how their data
  // will be treated, by the differing doc comments.

  // Lvalue function object constructor. This always copies the parameter into a
  // new allocation owned by this object.
  template <typename T>
  coro_functor32(const T& Functor) noexcept
    requires(!std::is_same_v<std::remove_cvref_t<T>, coro_functor32> && !std::is_convertible_v<T, std::coroutine_handle<>> && std::is_copy_constructible_v<T>)
  {
    uintptr_t funcAddr =
      reinterpret_cast<uintptr_t>(&cast_call<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = new T(Functor);
    deleter = &cast_delete<std::remove_reference_t<T>>;
  }

  // Rvalue function object constructor. This always moves the parameter into a
  // new allocation owned by this object.
  template <typename T>
  coro_functor32(T &&Functor) noexcept
    requires( // prevent lvalues from choosing this overload
              // https://stackoverflow.com/a/46936145/100443
        !std::is_reference_v<T> &&
        !std::is_same_v<std::remove_cvref_t<T>, coro_functor32> &&
        !std::is_convertible_v<T, std::coroutine_handle<>>)
  {
    uintptr_t funcAddr =
      reinterpret_cast<uintptr_t>(&cast_call<std::remove_reference_t<T>>);
    assert((funcAddr & IS_FUNC_BIT) == 0);
    func = reinterpret_cast<void*>(funcAddr | IS_FUNC_BIT);
    obj = new T(static_cast<T&&>(Functor));
    deleter = &cast_delete<std::remove_reference_t<T>>;
  }

  // Default constructor is provided for use with data structures that
  // initialize the passed-in type by reference.
  inline coro_functor32() noexcept { deleter = nullptr; }

  // Copy constructor is the same as move constructor. This is unusual but this
  // class is not designed for shared ownership of objects. Hence we need to
  // clear deleter of other.
  inline coro_functor32(coro_functor32& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
    deleter = Other.deleter;
    Other.deleter = nullptr;
#ifndef NDEBUG
    Other.func = nullptr;
    Other.obj = nullptr;
#endif
  }

  inline coro_functor32& operator=(coro_functor32& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
    deleter = Other.deleter;
    Other.deleter = nullptr;
#ifndef NDEBUG
    Other.func = nullptr;
    Other.obj = nullptr;
#endif
    return *this;
  }

  inline coro_functor32(coro_functor32&& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
    deleter = Other.deleter;
    Other.deleter = nullptr;
#ifndef NDEBUG
    Other.func = nullptr;
    Other.obj = nullptr;
#endif
  }

  inline coro_functor32& operator=(coro_functor32&& Other) noexcept {
    func = Other.func;
    obj = Other.obj;
    deleter = Other.deleter;
    Other.deleter = nullptr;
#ifndef NDEBUG
    Other.func = nullptr;
    Other.obj = nullptr;
#endif
    return *this;
  }

  inline ~coro_functor32() {
    if (deleter != nullptr) {
      deleter(obj);
    }
  }
};
} // namespace tmc
