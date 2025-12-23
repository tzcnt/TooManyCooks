// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <vector>

#ifdef TMC_DEBUG_THREAD_CREATION
#include "tmc/detail/compat.hpp"
#include <cstdio>
#endif

namespace tmc {
namespace detail {
struct Matrix {
  std::vector<size_t> data;
  size_t rows;
  size_t cols;
  inline Matrix() : data{}, rows{0}, cols{0} {}

  // Combines clear() and resize()
  inline void init(size_t Value, size_t Rows, size_t Cols) {
    data.clear();
    data.resize(Rows * Cols, Value);
    rows = Rows;
    cols = Cols;
  }
  inline void init(std::vector<size_t>&& Data, size_t Length) {
    data = std::move(Data);
    rows = Length;
    cols = Length;
  }
  inline void init(std::vector<size_t>&& Data, size_t Rows, size_t Cols) {
    data = std::move(Data);
    rows = Rows;
    cols = Cols;
  }

  inline size_t* get_row(size_t Idx) { return data.data() + Idx * cols; }

  // Like get_row, but copies data to a new vector
  inline std::vector<size_t> get_slice(size_t Idx) {
    std::vector<size_t> output;
    output.resize(cols);
    auto dst = output.data();
    auto src = get_row(Idx);
    for (size_t i = 0; i < cols; ++i) {
      *dst++ = *src++;
    }
    return output;
  }

  inline void copy_row(size_t DstIdx, size_t SrcIdx, Matrix& Src) {
    auto dst = get_row(DstIdx);
    auto src = Src.get_row(SrcIdx);
    size_t sz = cols < Src.cols ? cols : Src.cols;
    for (size_t i = 0; i < sz; ++i) {
      *dst++ = *src++;
    }
  }
  // Given a work-stealing matrix, for each thread, find the threads that will
  // prioritize stealing from it soonest. These are the threads that should be
  // woken first to steal from this thread.
  inline Matrix to_wakers() {
    Matrix output;
    output.data.resize(rows * cols);
    output.rows = rows;
    output.cols = cols;
    // Same as doing push_back to a vector<vector> but we know the fixed size in
    // advance. So just track the sub-index (col) of each nested vector (row).
    std::vector<size_t> outCols(rows, 0);
    for (size_t col = 0; col < cols; ++col) {
      for (size_t row = 0; row < rows; ++row) {
        size_t val = data[row * cols];
        size_t outRow = data[row * cols + col];
        size_t outCol = outCols[outRow];
        output.data[outRow * cols + outCol] = val;
        ++outCols[outRow];
      }
    }
    return output;
  }

  void clear() {
    data.clear();
    rows = 0;
    cols = 0;
  }

#ifdef TMC_DEBUG_THREAD_CREATION
  inline void print(const char* header) {
    if (header != nullptr) {
      printf("%s:\n", header);
    }
    size_t i = 0;
    for (size_t row = 0; row < rows; ++row) {
      for (size_t col = 0; col < cols; ++col) {
        if (data[i] == TMC_ALL_ONES) {
          std::printf("   -");
        } else {
          std::printf("%4zu", data[i]);
        }
        ++i;
      }
      std::printf("\n");
    }
    std::fflush(stdout);
  }
#endif
};

} // namespace detail
} // namespace tmc
