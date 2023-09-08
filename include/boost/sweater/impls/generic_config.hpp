////////////////////////////////////////////////////////////////////////////////
///
/// \file generic_config.hpp
/// ------------------------
///
/// (c) Copyright Domagoj Saric 2016 - 2023.
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
////////////////////////////////////////////////////////////////////////////////
// Compile time configuration
////////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_SWEATER_USE_CALLER_THREAD
#   define BOOST_SWEATER_USE_CALLER_THREAD true
#endif // BOOST_SWEATER_USE_CALLER_THREAD

#ifndef BOOST_SWEATER_HMP // heterogeneous multi-core processing
// https://android.googlesource.com/kernel/msm/+/android-msm-bullhead-3.10-marshmallow-dr/Documentation/scheduler/sched-hmp.txt
// https://android.googlesource.com/kernel/common/+/experimental/android-4.14%5E1..experimental/android-4.14 Energy Aware Scheduling
// https://www.kernel.org/doc/html/latest/scheduler/sched-energy.html
// https://www.sisoftware.co.uk/2015/06/22/arm-big-little-the-trouble-with-heterogeneous-multi-processing-when-4-are-better-than-8-or-when-8-is-not-always-the-lucky-number
// https://lwn.net/Articles/352286
#if ( defined( __ANDROID__ ) || defined( __APPLE__ ) ) && defined( __aarch64__ )
#   define BOOST_SWEATER_HMP false // implicit balancing through work stealing works better
#else
#   define BOOST_SWEATER_HMP false
#endif
#endif // BOOST_SWEATER_HMP

#ifndef BOOST_SWEATER_USE_PARALLELIZATION_COST
#   define BOOST_SWEATER_USE_PARALLELIZATION_COST false
#endif // BOOST_SWEATER_USE_PARALLELIZATION_COST

// https://petewarden.com/2015/10/11/one-weird-trick-for-faster-android-multithreading
// https://stackoverflow.com/questions/26637654/low-latency-communication-between-threads-in-the-same-process
// https://source.android.com/devices/tech/debug/jank_jitter
// https://www.scylladb.com/2016/06/10/read-latency-and-scylla-jmx-process
// https://lwn.net/Articles/663879
#ifndef BOOST_SWEATER_SPIN_BEFORE_SUSPENSION
#if defined( __ANDROID__ )
#   define BOOST_SWEATER_SPIN_BEFORE_SUSPENSION true
#else
#   define BOOST_SWEATER_SPIN_BEFORE_SUSPENSION false
#endif // Android
#endif // BOOST_SWEATER_SPIN_BEFORE_SUSPENSION

#ifndef BOOST_SWEATER_EVENTS
#if defined( __GNUC__ )
#   define BOOST_SWEATER_EVENTS true
#else
#   define BOOST_SWEATER_EVENTS false
#endif
#endif // BOOST_SWEATER_EVENTS

#if BOOST_SWEATER_HMP
#   if defined( BOOST_SWEATER_EXACT_WORKER_SELECTION ) && !BOOST_SWEATER_EXACT_WORKER_SELECTION
#       error BOOST_SWEATER_HMP requires BOOST_SWEATER_EXACT_WORKER_SELECTION
#   elif !defined( BOOST_SWEATER_EXACT_WORKER_SELECTION )
#       define BOOST_SWEATER_EXACT_WORKER_SELECTION true
#   endif
#endif // BOOST_SWEATER_HMP

#ifndef BOOST_SWEATER_EXACT_WORKER_SELECTION
#   define BOOST_SWEATER_EXACT_WORKER_SELECTION true
#endif // BOOST_SWEATER_EXACT_WORKER_SELECTION
//------------------------------------------------------------------------------
