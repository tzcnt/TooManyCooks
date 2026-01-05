// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/matrix.hpp"
#include <cassert>
#include <vector>

#ifdef TMC_DEBUG_THREAD_CREATION
#include "tmc/detail/compat.hpp"
#include <cstdio>
#endif

namespace tmc {
namespace detail {
Matrix::Matrix() : data{nullptr}, rows{0}, cols{0}, weak_ptr{false} {}

void Matrix::clear() {
  if (!weak_ptr) {
    delete[] data;
  }
  data = nullptr;
  rows = 0;
  cols = 0;
  weak_ptr = false;
}

Matrix::Matrix(Matrix&& Other) {
  data = Other.data;
  rows = Other.rows;
  cols = Other.cols;
  weak_ptr = Other.weak_ptr;
  Other.data = nullptr;
  Other.rows = 0;
  Other.cols = 0;
  Other.weak_ptr = 0;
}
Matrix& Matrix::operator=(Matrix&& Other) {
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

Matrix::~Matrix() { clear(); }

void Matrix::init(size_t Value, size_t Rows, size_t Cols) {
  clear();
  data = new size_t[Rows * Cols];
  for (size_t i = 0; i < Rows * Cols; ++i) {
    data[i] = Value;
  }
  rows = Rows;
  cols = Cols;
  weak_ptr = false;
}

void Matrix::copy_from(size_t* Other, size_t Rows, size_t Cols) {
  clear();
  data = new size_t[Rows * Cols];
  for (size_t i = 0; i < Rows * Cols; ++i) {
    data[i] = Other[i];
  }
  rows = Rows;
  cols = Cols;
  weak_ptr = false;
}

void Matrix::init(std::vector<size_t>&& Other, size_t Length) {
  assert(Other.size() == Length * Length);
  copy_from(Other.data(), Length, Length);
  Other.clear();
}

void Matrix::init(std::vector<size_t>&& Other, size_t Rows, size_t Cols) {
  assert(Other.size() == Rows * Cols);
  copy_from(Other.data(), Rows, Cols);
  Other.clear();
}

void Matrix::set_weak_ref(Matrix& Other) {
  clear();
  data = Other.data;
  rows = Other.rows;
  cols = Other.cols;
  weak_ptr = true;
}

std::vector<size_t> Matrix::get_slice(size_t Idx) {
  std::vector<size_t> output;
  output.resize(cols);
  auto dst = output.data();
  auto src = get_row(Idx);
  for (size_t i = 0; i < cols; ++i) {
    *dst++ = *src++;
  }
  return output;
}

void Matrix::copy_row(size_t DstIdx, size_t SrcIdx, Matrix& Src) {
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
Matrix Matrix::to_wakers() {
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
void Matrix::print(const char* header) {
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

} // namespace detail
} // namespace tmc
