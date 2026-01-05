// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <vector>

namespace tmc {
namespace detail {
struct Matrix {
  size_t* data;
  size_t rows;
  size_t cols;
  // Allow multiple matrixes to point to the same underlying data
  bool weak_ptr;
  Matrix();

  void clear();

  // No copy constructor
  Matrix(const Matrix& Other) = delete;
  Matrix& operator=(const Matrix& Other) = delete;

  // Can be moved, transferring ownership of the data
  Matrix(Matrix&& Other);
  Matrix& operator=(Matrix&& Other);

  ~Matrix();

  // Combines clear() and resize()
  void init(size_t Value, size_t Rows, size_t Cols);

  void copy_from(size_t* Other, size_t Rows, size_t Cols);

  void init(std::vector<size_t>&& Other, size_t Length);
  void init(std::vector<size_t>&& Other, size_t Rows, size_t Cols);

  void set_weak_ref(Matrix& Other);

  // get_row is the only operation in the runtime hot path
  inline size_t* get_row(size_t Idx) { return data + Idx * cols; }

  // Like get_row, but copies data to a new vector
  std::vector<size_t> get_slice(size_t Idx);

  void copy_row(size_t DstIdx, size_t SrcIdx, Matrix& Src);

  Matrix to_wakers();

#ifdef TMC_DEBUG_THREAD_CREATION
  void print(const char* header);
#endif
};

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/matrix.ipp"
#endif
