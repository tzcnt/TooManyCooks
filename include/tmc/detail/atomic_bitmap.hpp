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

class bitmap;

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
      count += static_cast<size_t>(std::popcount(v));
    }
    return count;
  }

  size_t
  popcount_and(const bitmap& mask, std::memory_order order) const noexcept;

  size_t popcount_or(
    const atomic_bitmap& other, std::memory_order order
  ) const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i].load(order) | other.words[i].load(order);
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += static_cast<size_t>(std::popcount(v));
    }
    return count;
  }

  size_t popcount_or_and(
    const atomic_bitmap& other, const bitmap& mask, std::memory_order order
  ) const noexcept;

  size_t popcount_inverted_or_and(
    const atomic_bitmap& other, const bitmap& mask, std::memory_order order
  ) const noexcept;

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
        bit_out =
          i * TMC_PLATFORM_BITS + static_cast<size_t>(std::countr_zero(word));
        return true;
      }
    }
    return false;
  }
};

class bitmap {
  size_t* words;
  size_t word_count;
  size_t bit_count;

  static constexpr size_t word_index(size_t bit_idx) noexcept {
    return bit_idx / TMC_PLATFORM_BITS;
  }

  static constexpr size_t bit_offset(size_t bit_idx) noexcept {
    return bit_idx % TMC_PLATFORM_BITS;
  }

public:
  bitmap() noexcept : words(nullptr), word_count(0), bit_count(0) {}

  ~bitmap() { clear(); }

  bitmap(const bitmap&) = delete;
  bitmap& operator=(const bitmap&) = delete;

  bitmap(bitmap&& other) noexcept
      : words(other.words), word_count(other.word_count),
        bit_count(other.bit_count) {
    other.words = nullptr;
    other.word_count = 0;
    other.bit_count = 0;
  }

  bitmap& operator=(bitmap&& other) noexcept {
    if (this != &other) {
      clear();
      words = other.words;
      word_count = other.word_count;
      bit_count = other.bit_count;
      other.words = nullptr;
      other.word_count = 0;
      other.bit_count = 0;
    }
    return *this;
  }

  void init(size_t num_bits) {
    bit_count = num_bits;
    word_count = (num_bits + TMC_PLATFORM_BITS - 1) / TMC_PLATFORM_BITS;
    words = new size_t[word_count];
    for (size_t i = 0; i < word_count; ++i) {
      words[i] = 0;
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

  void set_bit(size_t bit_idx) noexcept {
    assert(bit_idx < bit_count);
    size_t word_idx = word_index(bit_idx);
    size_t bit_off = bit_offset(bit_idx);
    words[word_idx] |= (TMC_ONE_BIT << bit_off);
  }

  bool test_bit(size_t bit_idx) const noexcept {
    assert(bit_idx < bit_count);
    size_t word_idx = word_index(bit_idx);
    size_t bit_off = bit_offset(bit_idx);
    return (words[word_idx] & (TMC_ONE_BIT << bit_off)) != 0;
  }

  size_t load_word(size_t word_idx) const noexcept { return words[word_idx]; }

  size_t get_word_count() const noexcept { return word_count; }

  // Mask off bits on the highest word if it's not full
  size_t valid_mask_for_word(size_t word_idx) const noexcept {
    if (word_idx + 1 < word_count) {
      return ~size_t(0);
    }
    const size_t rem = bit_count % TMC_PLATFORM_BITS;
    return rem == 0 ? ~size_t(0) : ((TMC_ONE_BIT << rem) - 1);
  }

  size_t popcount() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i];
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += static_cast<size_t>(std::popcount(v));
    }
    return count;
  }
};

inline size_t atomic_bitmap::popcount_and(
  const bitmap& mask, std::memory_order order
) const noexcept {
  assert(mask.get_word_count() == word_count);
  size_t count = 0;
  for (size_t i = 0; i < word_count; ++i) {
    size_t v = words[i].load(order) & mask.load_word(i);
    if (i + 1 == word_count) {
      v &= valid_mask_for_word(i);
    }
    count += static_cast<size_t>(std::popcount(v));
  }
  return count;
}

inline size_t atomic_bitmap::popcount_or_and(
  const atomic_bitmap& other, const bitmap& mask, std::memory_order order
) const noexcept {
  size_t count = 0;
  for (size_t i = 0; i < word_count; ++i) {
    size_t v =
      (words[i].load(order) | other.words[i].load(order)) & mask.load_word(i);
    if (i + 1 == word_count) {
      v &= valid_mask_for_word(i);
    }
    count += static_cast<size_t>(std::popcount(v));
  }
  return count;
}

inline size_t atomic_bitmap::popcount_inverted_or_and(
  const atomic_bitmap& other, const bitmap& mask, std::memory_order order
) const noexcept {
  size_t count = 0;
  for (size_t i = 0; i < word_count; ++i) {
    size_t v =
      ~(words[i].load(order) | other.words[i].load(order)) & mask.load_word(i);
    if (i + 1 == word_count) {
      v &= valid_mask_for_word(i);
    }
    count += static_cast<size_t>(std::popcount(v));
  }
  return count;
}

} // namespace detail
} // namespace tmc
