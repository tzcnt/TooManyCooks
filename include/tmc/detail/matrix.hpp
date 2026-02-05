// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp"

#include <vector>

namespace tmc {
namespace detail {
struct Matrix {
  size_t* data;
  size_t rows;
  size_t cols;
  // Allow multiple matrixes to point to the same underlying data
  bool weak_ptr;
  TMC_DECL Matrix();

  TMC_DECL void clear();

  // No copy constructor
  Matrix(const Matrix& Other) = delete;
  Matrix& operator=(const Matrix& Other) = delete;

  // Can be moved, transferring ownership of the data
  TMC_DECL Matrix(Matrix&& Other);
  TMC_DECL Matrix& operator=(Matrix&& Other);

  TMC_DECL ~Matrix();

  // Combines clear() and resize()
  TMC_DECL void init(size_t Value, size_t Rows, size_t Cols);

  TMC_DECL void copy_from(size_t* Other, size_t Rows, size_t Cols);

  TMC_DECL void init(std::vector<size_t>&& Other, size_t Length);
  TMC_DECL void init(std::vector<size_t>&& Other, size_t Rows, size_t Cols);

  TMC_DECL void set_weak_ref(Matrix& Other);

  // get_row is the only operation in the runtime hot path
  inline size_t* get_row(size_t Idx) { return data + Idx * cols; }

  // Like get_row, but copies data to a new vector
  TMC_DECL std::vector<size_t> get_slice(size_t Idx);

  TMC_DECL void copy_row(size_t DstIdx, size_t SrcIdx, Matrix& Src);

  TMC_DECL Matrix to_wakers();

#ifdef TMC_DEBUG_THREAD_CREATION
  TMC_DECL void print(const char* header);
#endif
};

} // namespace detail
} // namespace tmc

#if !defined(TMC_USE_IMPL_FILE) || defined(TMC_IMPL)
#include "tmc/detail/matrix.ipp"
#endif
