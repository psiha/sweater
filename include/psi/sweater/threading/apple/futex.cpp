////////////////////////////////////////////////////////////////////////////////
///
/// \file futex.cpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2026.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#include "../futex.hpp"

#include <cstdint>
//------------------------------------------------------------------------------
namespace psi::thrd_lite
{
//------------------------------------------------------------------------------

// __ulock_wait / __ulock_wake -- PRIVATE Darwin syscalls (libsystem_kernel),
// declared in no public header. This is the same pair libc++ itself falls
// back to for std::atomic<T>::wait/notify on Apple OS versions predating the
// PUBLIC os_sync_wait_on_address API (macOS 14.4 / iOS 17.4, <os/os_sync_wait_
// on_address.h>) -- see llvm-project's libcxx/include/__atomic/atomic_sync.h.
// Deliberately targeting the older private pair here rather than the new
// public one so this backend also covers pre-14.4 deployment targets; this is
// explicitly a bleeding-edge/testing-only backend (see futex_rw_mutex.hpp's
// design-doc comment), not a production path, so the extra private-API risk
// (undocumented, unversioned, could disappear or be blocked under a hardened
// runtime / App Store review without notice) is accepted knowingly. The
// prototypes and UL_*/ULF_* constants below are reverse-engineered from XNU's
// bsd/sys/ulock.h (kernel-side source; userspace ships no equivalent header)
// and match what libc++'s fallback path and WebKit's ParkingLot declare.
extern "C"
{
    int __ulock_wait( std::uint32_t operation, void * addr, std::uint64_t value, std::uint32_t timeout_us ) noexcept;
    int __ulock_wake( std::uint32_t operation, void * addr, std::uint64_t wake_value ) noexcept;
}

namespace
{
    // Non-shared (single-process, plain-memory-address) 32-bit compare-and-wait.
    // The _SHARED variants (cross-process, backed by identically-mapped shared
    // memory) and the 64-bit variants (UL_COMPARE_AND_WAIT64[_SHARED]) are not
    // needed here -- futex::value_type is 32 bits on every non-Windows backend
    // (see futex.hpp) and this primitive is always intra-process.
    constexpr std::uint32_t UL_COMPARE_AND_WAIT = 1;
    constexpr std::uint32_t ULF_WAKE_ALL        = 0x00000100;
    // Skips the (thread-local) errno round-trip; __ulock_wait/__ulock_wake
    // instead return -error_code directly on failure. We don't need the
    // specific error (ENOENT "nothing was waiting", ETIMEDOUT, EINTR are all
    // equally "return to the caller's re-check loop" here -- see
    // futex_rw_mutex.hpp's acquire_ro/acquire_rw, which always re-load and
    // re-verify after wait_if_equal returns for any reason), so the plain
    // return value is all this needs.
    constexpr std::uint32_t ULF_NO_ERRNO        = 0x01000000;

    static_assert( sizeof( futex ) == 4 );
} // anonymous namespace

void futex::wake_one(                                              ) const noexcept
{
    // No ULF_WAKE_ALL: __ulock_wake's default already wakes (at most) a single
    // waiter.
    __ulock_wake( UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, const_cast< futex * >( this ), 0 );
}
void futex::wake    ( hardware_concurrency_t                       ) const noexcept { wake_all(); } // no exact-count API, mirrors windows/futex.cpp
void futex::wake_all(                                              ) const noexcept
{
    __ulock_wake( UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, const_cast< futex * >( this ), 0 );
}

void futex::wait_if_equal( value_type const desired_value ) const noexcept
{
    // timeout_us == 0 means "wait indefinitely" for __ulock_wait. Return value
    // deliberately ignored (see ULF_NO_ERRNO comment above) -- every caller
    // treats "this returned" (for whatever reason: woken, value already
    // didn't match, spurious, signal-interrupted) as "go re-check", exactly
    // like the Linux FUTEX_WAIT backend (linux/futex.cpp) already does.
    __ulock_wait( UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, const_cast< futex * >( this ), std::uint64_t{ desired_value }, 0 );
}

//------------------------------------------------------------------------------
} // namespace psi::thrd_lite
//------------------------------------------------------------------------------
