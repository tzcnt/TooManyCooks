// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

/// The major version of TooManyCooks. Major versions are likely to contain
/// substantial breaking API changes.
#define TMC_VERSION_MAJOR 1

/// The minor version of TooManyCooks. Minor versions will contain new features
/// and may contain breaking API changes that affect a small subset of use
/// cases.
#define TMC_VERSION_MINOR 5

/// The patch version of TooManyCooks. Patch versions > 0 are used exclusively
/// for bugfixes that are backward-compatible (no breaking API changes).
#define TMC_VERSION_PATCH 0

/// This number is updated to (MAJOR<<16) + (MINOR<<8) + PATCH for each new
/// release tagged vMAJOR.MINOR.PATCH
#define TMC_VERSION 0x00010500
