// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>

namespace tmc {
namespace detail {

class atomic_bitmap {
  std::atomic<size_t>* words;
  size_t word_count;
  size_t bit_count;

  static constexpr size_t word_index(size_t bit_idx) noexcept {
    return bit_idx / TMC_PLATFORM_BITS;
  }

  static constexpr size_t bit_offset(size_t bit_idx) noexcept {
    return bit_idx % TMC_PLATFORM_BITS;
  }

  // Mask off bits on the highest word if it's not full
  size_t valid_mask_for_word(size_t word_idx) const noexcept {
    if (word_idx + 1 < word_count) {
      return ~size_t(0);
    }
    const size_t rem = bit_count % TMC_PLATFORM_BITS;
    return rem == 0 ? ~size_t(0) : ((TMC_ONE_BIT << rem) - 1);
  }

public:
  atomic_bitmap() noexcept : words(nullptr), word_count(0), bit_count(0) {}

  ~atomic_bitmap() { clear(); }

  void init(size_t num_bits) {
    bit_count = num_bits;
    word_count = (num_bits + TMC_PLATFORM_BITS - 1) / TMC_PLATFORM_BITS;
    words = new std::atomic<size_t>[word_count];
    for (size_t i = 0; i < word_count; ++i) {
      words[i].store(0, std::memory_order_relaxed);
    }
  }

  void clear() {
    if (words != nullptr) {
      delete[] words;
      words = nullptr;
    }
    word_count = 0;
    bit_count = 0;
  }

  size_t fetch_or_bit(size_t bit_idx, std::memory_order order) noexcept {
    assert(bit_idx < bit_count);
    size_t word_idx = word_index(bit_idx);
    size_t bit_off = bit_offset(bit_idx);
    return words[word_idx].fetch_or(TMC_ONE_BIT << bit_off, order);
  }

  size_t fetch_and_bit(size_t bit_idx, std::memory_order order) noexcept {
    assert(bit_idx < bit_count);
    size_t word_idx = word_index(bit_idx);
    size_t bit_off = bit_offset(bit_idx);
    return words[word_idx].fetch_and(~(TMC_ONE_BIT << bit_off), order);
  }

  bool test_bit(size_t bit_idx, std::memory_order order) const noexcept {
    assert(bit_idx < bit_count);
    size_t word_idx = word_index(bit_idx);
    size_t bit_off = bit_offset(bit_idx);
    return (words[word_idx].load(order) & (TMC_ONE_BIT << bit_off)) != 0;
  }

  size_t load_word(size_t word_idx, std::memory_order order) const noexcept {
    return words[word_idx].load(order);
  }

  size_t popcount(std::memory_order order) const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i].load(order);
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += std::popcount(v);
    }
    return count;
  }

  size_t popcount_or(
    const atomic_bitmap& other, std::memory_order order
  ) const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i].load(order) | other.words[i].load(order);
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += std::popcount(v);
    }
    return count;
  }

  size_t load_or(
    const atomic_bitmap& other, size_t word_idx, std::memory_order order
  ) const noexcept {
    return words[word_idx].load(order) | other.words[word_idx].load(order);
  }

  size_t load_inverted_or(
    const atomic_bitmap& other, size_t word_idx, std::memory_order order
  ) const noexcept {
    const size_t mask = valid_mask_for_word(word_idx);
    const size_t a = words[word_idx].load(order);
    const size_t b = other.words[word_idx].load(order);
    return mask & ~(a | b);
  }

  size_t get_word_count() const noexcept { return word_count; }

  bool
  find_first_set_bit(size_t& bit_out, std::memory_order order) const noexcept {
    for (size_t i = 0; i < word_count; ++i) {
      size_t word = words[i].load(order);
      if (word != 0) {
        bit_out = i * TMC_PLATFORM_BITS + std::countr_zero(word);
        return true;
      }
    }
    return false;
  }
};

} // namespace detail
} // namespace tmc
