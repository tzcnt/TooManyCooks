// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/bit_manip.hpp"
#include "tmc/detail/compat.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>

namespace tmc {
namespace detail {

struct bitmap;

#ifdef TMC_MORE_THREADS

struct atomic_bitmap {
  std::atomic<size_t>* words;
  size_t word_count;
  size_t bit_count;

  static constexpr size_t word_index(size_t BitIdx) noexcept {
    return BitIdx / TMC_PLATFORM_BITS;
  }

  static constexpr size_t bitOffset(size_t BitIdx) noexcept {
    return BitIdx % TMC_PLATFORM_BITS;
  }

  inline size_t valid_mask_for_word(size_t WordIdx) const noexcept {
    if (WordIdx + 1 < word_count) {
      return TMC_ALL_ONES;
    }
    const size_t rem = bit_count % TMC_PLATFORM_BITS;
    return rem == 0 ? TMC_ALL_ONES : ((TMC_ONE_BIT << rem) - 1);
  }

public:
  inline atomic_bitmap() noexcept
      : words(nullptr), word_count(1), bit_count(0) {}

  inline ~atomic_bitmap() { clear(); }

  inline void init(size_t BitCount) {
    bit_count = BitCount;
    word_count = (BitCount + TMC_PLATFORM_BITS - 1) / TMC_PLATFORM_BITS;
    words = new std::atomic<size_t>[word_count];
    for (size_t i = 0; i < word_count; ++i) {
      words[i].store(0, std::memory_order_relaxed);
    }
  }

  inline void clear() {
    if (words != nullptr) {
      delete[] words;
      words = nullptr;
    }
    word_count = 1;
    bit_count = 0;
  }

  inline bool test_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) const noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    return (words[wordIdx].load(MO) & (TMC_ONE_BIT << bitOff)) != 0;
  }

  inline void set_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    words[wordIdx].fetch_or(TMC_ONE_BIT << bitOff, MO);
  }

  inline void clr_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    words[wordIdx].fetch_and(~(TMC_ONE_BIT << bitOff), MO);
  }

  // Sets bit at BitIdx and returns the popcnt after the bit was set.
  // WARNING: This uses fetch_add as an optimization, so it's not safe to call
  // if the bit might already be set.
  inline size_t set_bit_popcnt(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t old;
      if (i == wordIdx) {
        old = words[wordIdx].fetch_add(TMC_ONE_BIT << bitOff, MO);
        assert((old & (TMC_ONE_BIT << bitOff)) == 0);
      } else {
        old = words[i].load(MO);
      }
      if (i + 1 == word_count) {
        old &= valid_mask_for_word(i);
      }
      count += tmc::detail::popcnt(old);
    }
    return count + 1;
  }

  // Clears bit at BitIdx and returns the popcnt after the bit was cleared.
  // WARNING: This uses fetch_sub as an optimization, so it's not safe to call
  // if the bit might already be cleared.
  inline size_t clr_bit_popcnt(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t old;
      if (i == wordIdx) {
        old = words[wordIdx].fetch_sub(TMC_ONE_BIT << bitOff, MO);
        assert((old & (TMC_ONE_BIT << bitOff)) != 0);
      } else {
        old = words[i].load(MO);
      }
      if (i + 1 == word_count) {
        old &= valid_mask_for_word(i);
      }
      count += tmc::detail::popcnt(old);
    }
    return count - 1;
  }

  inline size_t load_word(
    size_t WordIdx, std::memory_order MO = std::memory_order_relaxed
  ) const noexcept {
    return words[WordIdx].load(MO);
  }

  inline size_t popcnt() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i].load(std::memory_order_relaxed);
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += tmc::detail::popcnt(v);
    }
    return count;
  }
  // Returns ~(this | other)
  inline size_t load_inverted_or(
    const atomic_bitmap& other, size_t WordIdx, std::memory_order MO
  ) const noexcept {
    const size_t mask = valid_mask_for_word(WordIdx);
    const size_t a = words[WordIdx].load(MO);
    const size_t b = other.words[WordIdx].load(MO);
    return mask & ~(a | b);
  }

  inline size_t get_word_count() const noexcept { return word_count; }
};

struct bitmap {
  size_t* words;
  size_t word_count;
  size_t bit_count;

  static constexpr size_t word_index(size_t BitIdx) noexcept {
    return BitIdx / TMC_PLATFORM_BITS;
  }

  static constexpr size_t bitOffset(size_t BitIdx) noexcept {
    return BitIdx % TMC_PLATFORM_BITS;
  }

public:
  inline bitmap() noexcept : words(nullptr), word_count(0), bit_count(0) {}

  inline ~bitmap() { clear(); }

  bitmap(const bitmap&) = delete;
  bitmap& operator=(const bitmap&) = delete;

  inline bitmap(bitmap&& other) noexcept
      : words(other.words), word_count(other.word_count),
        bit_count(other.bit_count) {
    other.words = nullptr;
    other.word_count = 0;
    other.bit_count = 0;
  }

  inline bitmap& operator=(bitmap&& other) noexcept {
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

  inline void init(size_t num_bits) {
    bit_count = num_bits;
    word_count = (num_bits + TMC_PLATFORM_BITS - 1) / TMC_PLATFORM_BITS;
    words = new size_t[word_count];
    for (size_t i = 0; i < word_count; ++i) {
      words[i] = 0;
    }
  }

  inline void clear() {
    if (words != nullptr) {
      delete[] words;
      words = nullptr;
    }
    word_count = 0;
    bit_count = 0;
  }

  inline void set_bit(size_t BitIdx) noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    words[wordIdx] |= (TMC_ONE_BIT << bitOff);
  }

  inline bool test_bit(size_t BitIdx) const noexcept {
    assert(BitIdx < bit_count);
    size_t wordIdx = word_index(BitIdx);
    size_t bitOff = bitOffset(BitIdx);
    return (words[wordIdx] & (TMC_ONE_BIT << bitOff)) != 0;
  }

  inline size_t load_word(size_t WordIdx) const noexcept {
    return words[WordIdx];
  }

  inline size_t get_word_count() const noexcept { return word_count; }

  inline size_t valid_mask_for_word(size_t WordIdx) const noexcept {
    if (WordIdx + 1 < word_count) {
      return TMC_ALL_ONES;
    }
    const size_t rem = bit_count % TMC_PLATFORM_BITS;
    return rem == 0 ? TMC_ALL_ONES : ((TMC_ONE_BIT << rem) - 1);
  }

  inline size_t popcnt() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < word_count; ++i) {
      size_t v = words[i];
      if (i + 1 == word_count) {
        v &= valid_mask_for_word(i);
      }
      count += tmc::detail::popcnt(v);
    }
    return count;
  }
};

#else // !TMC_MORE_THREADS - Fixed size single-word implementation

struct atomic_bitmap {
  std::atomic<size_t> word;

  inline atomic_bitmap() noexcept : word(0) {}

  inline void init([[maybe_unused]] size_t num_bits) {
    word.store(0, std::memory_order_relaxed);
  }

  inline void clear() { word.store(0, std::memory_order_relaxed); }

  inline constexpr size_t get_word_count() const noexcept { return 1; }

  inline size_t popcnt() {
    return tmc::detail::popcnt(word.load(std::memory_order_relaxed));
  }

  // Sets bit at BitIdx and returns the popcnt after the bit was set.
  // WARNING: This uses fetch_add as an optimization, so it's not safe to call
  // if the bit might already be set.
  inline size_t set_bit_popcnt(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    size_t old = word.fetch_add(TMC_ONE_BIT << BitIdx, MO);
    assert((old & (TMC_ONE_BIT << BitIdx)) == 0);
    return tmc::detail::popcnt(old) + 1;
  }

  // Clears bit at BitIdx and returns the popcnt after the bit was cleared.
  // WARNING: This uses fetch_sub as an optimization, so it's not safe to call
  // if the bit might already be cleared.
  inline size_t clr_bit_popcnt(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    size_t old = word.fetch_sub(TMC_ONE_BIT << BitIdx, MO);
    assert((old & (TMC_ONE_BIT << BitIdx)) != 0);
    return tmc::detail::popcnt(old) - 1;
  }

  inline void set_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    word.fetch_or(TMC_ONE_BIT << BitIdx, MO);
  }

  inline void clr_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) noexcept {
    word.fetch_and(~(TMC_ONE_BIT << BitIdx), MO);
  }

  /*** Currently only used by tests, for compabilitity with TMC_MORE_THREADS */

  inline bool test_bit(
    size_t BitIdx, std::memory_order MO = std::memory_order_relaxed
  ) const noexcept {
    assert(BitIdx < 64);
    return (word.load(MO) & (TMC_ONE_BIT << BitIdx)) != 0;
  }

  inline size_t load_word(
    [[maybe_unused]] size_t WordIdx,
    std::memory_order MO = std::memory_order_relaxed
  ) const noexcept {
    assert(WordIdx == 0);
    return word.load(MO);
  }
};

struct bitmap {
  size_t word;

  inline bitmap() noexcept : word(0) {}

  inline void init([[maybe_unused]] size_t num_bits) { word = 0; }

  inline void set_bit(size_t BitIdx) noexcept {
    word |= (TMC_ONE_BIT << BitIdx);
  }

  inline bool test_bit(size_t BitIdx) const noexcept {
    return (word & (TMC_ONE_BIT << BitIdx)) != 0;
  }
  inline size_t popcnt() const noexcept { return tmc::detail::popcnt(word); }

  /*** Currently only used by tests, for compabilitity with TMC_MORE_THREADS */
  inline void clear() { word = 0; }

  inline size_t load_word([[maybe_unused]] size_t WordIdx) const noexcept {
    return word;
  }
  inline constexpr size_t get_word_count() const noexcept { return 1; }

  inline size_t
  valid_mask_for_word([[maybe_unused]] size_t WordIdx) const noexcept {
    return TMC_ALL_ONES;
  }
};

#endif // TMC_MORE_THREADS
} // namespace detail
} // namespace tmc
