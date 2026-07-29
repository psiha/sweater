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

#ifndef PSI_SWEATER_USE_CALLER_THREAD
#   define PSI_SWEATER_USE_CALLER_THREAD true
#endif // PSI_SWEATER_USE_CALLER_THREAD

#ifndef PSI_SWEATER_HMP // heterogeneous multi-core processing
// https://android.googlesource.com/kernel/msm/+/android-msm-bullhead-3.10-marshmallow-dr/Documentation/scheduler/sched-hmp.txt
// https://android.googlesource.com/kernel/common/+/experimental/android-4.14%5E1..experimental/android-4.14 Energy Aware Scheduling
// https://www.kernel.org/doc/html/latest/scheduler/sched-energy.html
// https://www.sisoftware.co.uk/2015/06/22/arm-big-little-the-trouble-with-heterogeneous-multi-processing-when-4-are-better-than-8-or-when-8-is-not-always-the-lucky-number
// https://lwn.net/Articles/352286
#if ( defined( __ANDROID__ ) || defined( __APPLE__ ) ) && defined( __aarch64__ )
#   define PSI_SWEATER_HMP false // implicit balancing through work stealing works better
#else
#   define PSI_SWEATER_HMP false
#endif
#endif // PSI_SWEATER_HMP

#ifndef PSI_SWEATER_USE_PARALLELIZATION_COST
#   define PSI_SWEATER_USE_PARALLELIZATION_COST true
#endif // PSI_SWEATER_USE_PARALLELIZATION_COST

// https://petewarden.com/2015/10/11/one-weird-trick-for-faster-android-multithreading
// https://stackoverflow.com/questions/26637654/low-latency-communication-between-threads-in-the-same-process
// https://source.android.com/devices/tech/debug/jank_jitter
// https://www.scylladb.com/2016/06/10/read-latency-and-scylla-jmx-process
// https://lwn.net/Articles/663879
// Now on EVERYWHERE (was Android-only): with sticky fire dispatch in place,
// a BRIEF spin (see shop::worker_spin_count -- the runtime knob; the
// Android-era 100k-nop count was the pathology, not spinning itself) lets a
// worker catch the next back-to-back item/spread without a park+wake round
// trip. Measured on Apple Silicon @ 8 workers, sweater_shop_bench: small
// counts (500-2000 nops) leave fire ops flat-to-better and improve the
// back-to-back small-spread join ~30-40% (~5 vs ~8 us); large counts
// (>=10k) regress BOTH (post-join spinners oversubscribe the cores against
// the producer/caller). Turn fully off with
// -DPSI_SWEATER_SPIN_BEFORE_SUSPENSION=false, or set worker_spin_count=0.
#ifndef PSI_SWEATER_SPIN_BEFORE_SUSPENSION
#   define PSI_SWEATER_SPIN_BEFORE_SUSPENSION true
#endif // PSI_SWEATER_SPIN_BEFORE_SUSPENSION

#ifndef PSI_SWEATER_EVENTS
#if defined( __GNUC__ )
#   define PSI_SWEATER_EVENTS true
#else
#   define PSI_SWEATER_EVENTS false
#endif
#endif // PSI_SWEATER_EVENTS

#if PSI_SWEATER_HMP
#   if defined( PSI_SWEATER_EXACT_WORKER_SELECTION ) && !PSI_SWEATER_EXACT_WORKER_SELECTION
#       error PSI_SWEATER_HMP requires PSI_SWEATER_EXACT_WORKER_SELECTION
#   elif !defined( PSI_SWEATER_EXACT_WORKER_SELECTION )
#       define PSI_SWEATER_EXACT_WORKER_SELECTION true
#   endif
#endif // PSI_SWEATER_HMP

#ifndef PSI_SWEATER_EXACT_WORKER_SELECTION
#   define PSI_SWEATER_EXACT_WORKER_SELECTION true
#endif // PSI_SWEATER_EXACT_WORKER_SELECTION
//------------------------------------------------------------------------------
