#pragma once
#include <type_traits>

namespace tmc {
template <typename T>
concept IsVoid = std::is_void_v<T>;
template <typename T>
concept IsNotVoid = !std::is_void_v<T>;
namespace detail {
template <typename E>
concept TypeErasableExecutor = requires(E e) { e.type_erased(); };
} // namespace detail
} // namespace tmc
