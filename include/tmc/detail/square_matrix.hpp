// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cassert>
#include <vector>

#ifdef TMC_DEBUG_THREAD_CREATION
#include "tmc/detail/compat.hpp"
#include <cstdio>
#endif

namespace tmc {
namespace detail {
struct Matrix {
  size_t* data;
  size_t rows;
  size_t cols;
  // Allow multiple matrixes to point to the same underlying data
  bool weak_ptr;
  inline Matrix() : data{nullptr}, rows{0}, cols{0}, weak_ptr{false} {}

  inline void clear() {
    if (!weak_ptr) {
      delete[] data;
    }
    data = nullptr;
    rows = 0;
    cols = 0;
    weak_ptr = false;
  }

  // No copy constructor
  Matrix(const Matrix& Other) = delete;
  Matrix& operator=(const Matrix& Other) = delete;

  // // Explicit copy is allowed
  // Matrix clone();

  // Can be moved, transferring ownership of the data
  Matrix(Matrix&& Other) {
    data = Other.data;
    rows = Other.rows;
    cols = Other.cols;
    weak_ptr = Other.weak_ptr;
    Other.data = nullptr;
    Other.rows = 0;
    Other.cols = 0;
    Other.weak_ptr = 0;
  }
  Matrix& operator=(Matrix&& Other) {
    clear();
    data = Other.data;
    rows = Other.rows;
    cols = Other.cols;
    weak_ptr = Other.weak_ptr;
    Other.data = nullptr;
    Other.rows = 0;
    Other.cols = 0;
    Other.weak_ptr = 0;
    return *this;
  }

  inline ~Matrix() { clear(); }

  // Combines clear() and resize()
  inline void init(size_t Value, size_t Rows, size_t Cols) {
    clear();
    data = new size_t[Rows * Cols];
    for (size_t i = 0; i < Rows * Cols; ++i) {
      data[i] = Value;
    }
    rows = Rows;
    cols = Cols;
    weak_ptr = false;
  }

  inline void copy_from(size_t* Other, size_t Rows, size_t Cols) {
    clear();
    data = new size_t[Rows * Cols];
    for (size_t i = 0; i < Rows * Cols; ++i) {
      data[i] = Other[i];
    }
    rows = Rows;
    cols = Cols;
    weak_ptr = false;
  }

  inline void init(std::vector<size_t>&& Other, size_t Length) {
    assert(Other.size() == Length * Length);
    copy_from(Other.data(), Length, Length);
    Other.clear();
  }
  inline void init(std::vector<size_t>&& Other, size_t Rows, size_t Cols) {
    assert(Other.size() == Rows * Cols);
    copy_from(Other.data(), Rows, Cols);
    Other.clear();
  }

  inline void set_weak_ref(Matrix& Other) {
    clear();
    data = Other.data;
    rows = Other.rows;
    cols = Other.cols;
    weak_ptr = true;
  }

  inline size_t* get_row(size_t Idx) { return data + Idx * cols; }

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
    output.data = new size_t[rows * cols];
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
