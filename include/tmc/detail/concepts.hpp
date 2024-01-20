#pragma once

namespace tmc {
namespace detail {
template <typename E>
concept TypeErasableExecutor = requires(E e) { e.type_erased(); };
} // namespace detail
} // namespace tmc
