// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_IMPL_FILE
#define TMC_DEF
#define TMC_DECL
#else
#define TMC_DEF inline
#define TMC_DECL inline
#endif
