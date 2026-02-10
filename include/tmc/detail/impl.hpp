// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// When TMC_STANDALONE_COMPILATION is defined, the user must #define TMC_IMPL
// and #include the necessary headers (easiest to #include
// "tmc/all_headers.hpp") in exactly one translation unit. This provides
// definitions for non-template functions in that translation unit.

// When TMC_WINDOWS_DLL is defined, this also implies
// TMC_STANDALONE_COMPILATION. The user must provide the impl translation unit,
// which also dllexports the necessary symbols. Other DLLs / files which do not
// define TMC_IMPL will dllimport those symbols.

// When neither are defined, the library is header-only, and all symbols are
// declared "inline".

#ifdef TMC_WINDOWS_DLL
#ifndef TMC_STANDALONE_COMPILATION
#define TMC_STANDALONE_COMPILATION
#endif
#endif

#ifdef TMC_STANDALONE_COMPILATION
#ifndef TMC_STANDALONE_COMPILATION_HWLOC
#define TMC_STANDALONE_COMPILATION_HWLOC
#endif

#ifdef TMC_WINDOWS_DLL
#ifdef TMC_IMPL
#define TMC_DECL __declspec(dllexport)
#else // !TMC_IMPL
#define TMC_DECL __declspec(dllimport)
#endif
#else // !TMC_WINDOWS_DLL
#define TMC_DECL
#endif

#define TMC_DECL_HWLOC TMC_DECL
#else // !TMC_STANDALONE_COMPILATION
#define TMC_DECL inline

#if defined(TMC_STANDALONE_COMPILATION_HWLOC) && !defined(TMC_USE_HWLOC)
#undef TMC_STANDALONE_COMPILATION_HWLOC
#endif

#ifdef TMC_STANDALONE_COMPILATION_HWLOC
#define TMC_DECL_HWLOC
#else // !TMC_STANDALONE_COMPILATION_HWLOC
#define TMC_DECL_HWLOC inline
#endif
#endif
