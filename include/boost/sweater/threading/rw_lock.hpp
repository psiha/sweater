////////////////////////////////////////////////////////////////////////////////
///
/// \file rw_lock.hpp
/// -----------------
///
/// (c) Copyright Domagoj Saric 2024.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#pragma once
//------------------------------------------------------------------------------
#ifdef _WIN32
#   include "windows/rw_lock.hpp"
#else
#   include "posix/rw_lock.hpp"
#endif
//------------------------------------------------------------------------------
