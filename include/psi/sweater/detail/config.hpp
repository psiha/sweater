////////////////////////////////////////////////////////////////////////////////
///
/// \file detail/config.hpp
/// -----------------------
///
/// Compiler and branch-hint utilities for psi::sweater.
/// Include chain: hardware_concurrency.hpp → this file → all sweater headers.
///
/// (c) Copyright Domagoj Saric 2016 - 2025.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#pragma once
//------------------------------------------------------------------------------

// ── Branch prediction hints ───────────────────────────────────────────────────
#if __has_builtin( __builtin_expect )
#   define PSI_LIKELY(x)   __builtin_expect( !!(x), 1 )
#   define PSI_UNLIKELY(x) __builtin_expect( !!(x), 0 )
#else
#   define PSI_LIKELY(x)   (x)
#   define PSI_UNLIKELY(x) (x)
#endif

//------------------------------------------------------------------------------
