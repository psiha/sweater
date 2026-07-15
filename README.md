# sweater
#### Humble Outer Dispatch

Cpp still does not have a standard 'parallel work dispatching' solution, AKA a thread pool. I dislike that name however, since unless the API provides access to the underlying threads, a thread pool is but one possible implementation (detail) of a 'parallel work dispatching' library.

Any name involving 'parallel' is a tongue breaker and 'worker' sounds too 'red' hence sweater - legal, portable, machine sweat shops that sweat for you.

(not an actual/official Boost library, simply started as another wannabe proposal)


#### Dependencies

[Functionoid](https://github.com/psiha/functionoid)
[ConfigEx](https://github.com/psiha/config_ex)
[moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)


#### Testing

CI (`.github/workflows`) builds and runs the full test suite on every push/PR across
10 configurations: MSVC (Debug/Release), Clang-CL (Debug/Release), Clang (Debug/Release),
GCC (Debug/Release), Apple-Clang (Debug/Release) — i.e. Windows, Linux, and macOS, each
in both a debug (assertions on) and optimized configuration.

Current test targets (`test/`, GoogleTest):
- `sweater_smoke_test` — `sweat_shop` basics (`spread_the_sweat`/`dispatch`/`fire_and_forget`).
- `sweater_rw_mutex_contract_test` — `TYPED_TEST_SUITE`-based consolidation of the
  rw-mutex family's shared behavioral contracts, run once per concrete type instead of
  hand-duplicated per file (`rw_mutex_contract_test.cpp`):
  - `MutexContract` (`rw_mutex`, `futex_rw_mutex`, `reader_preferring_rw_mutex`,
    `reader_preferring_futex_rw_mutex`) — preference-independent mutual-exclusion
    invariants (uncontended try-acquire, shared readers exclude a writer, a writer
    excludes everything, a writer blocks until an existing reader releases).
  - `WriterPreferringContract` (`rw_mutex`, `futex_rw_mutex`) — a new reader is blocked
    once a writer is queued.
  - `ReaderPreferringContract` (`reader_preferring_rw_mutex`,
    `reader_preferring_futex_rw_mutex`) — same-thread nested reads never deadlock
    behind a queued writer (unconditional on every platform); new-reader-admission
    despite a queued writer is trait-gated (`mutex_test_traits.hpp`'s
    `mutex_traits<Mutex>::admits_reader_with_writer_queued`) since it only holds for
    `reader_preferring_rw_mutex` under glibc's NP rwlock-kind extensions — elsewhere
    that name is `rrw_mutex.hpp`'s per-thread hold-collapsing wrapper, which is "still
    writer-preferring underneath", and the test `GTEST_SKIP`s there instead of failing.
  - `GuardContract` (`rw_mutex`, `reader_preferring_rw_mutex`) — RAII guards
    (`rw_lock`/`basic_ro_lock<Mutex>`) and `std::shared_lock`/`std::unique_lock`
    interop; scoped to just these two since they're the only types the guards bind to
    (the futex family is a separate, non-`rw_mutex`-derived hierarchy).
  - `FutexOnlyContract` (`futex_rw_mutex`, `reader_preferring_futex_rw_mutex`) —
    copyability and a mutual-exclusion/progress stress test, kept scoped to just the
    futex pair rather than folded into `MutexContract`: neither `rw_mutex` nor
    `reader_preferring_rw_mutex` had stress coverage before, and a genuinely
    reader-preferring lock under this stress shape's sustained read load can
    legitimately starve a writer, which would hang the test rather than fail it
    cleanly — a deliberate scope decision, not a consolidation byproduct.
- `sweater_rw_preference_test` — type-identity checks for the
  `writer_preferring_rw_mutex`/`reader_preferring_rw_mutex` split specific to glibc's NP
  rwlock-kind extensions (self-skips — compiles to zero tests — elsewhere; the
  behavioral coverage for `reader_preferring_rw_mutex` on those platforms now lives in
  `sweater_rw_mutex_contract_test` above).
- `sweater_futex_test` — low-level `psi::thrd_lite::futex` wait/wake, including the
  bitset-targeted `wake_all`/`wait_if_equal` overloads (real filtering on Linux
  `FUTEX_WAIT_BITSET`/`FUTEX_WAKE_BITSET`; a documented no-op elsewhere).
- `sweater_libuv_test` — optional, only built when libuv headers/library are found.

All of the above are green on every CI platform as of this writing. The rw-mutex family
in particular has been additionally stress-tested well beyond CI's single pass: repeated
`--gtest_repeat` runs (dozens of iterations) and separate process-spawn runs (tens of
runs) on both WSL/Linux and real Apple hardware, specifically to catch intermittent
concurrency bugs that a single green run can miss — two such bugs were in fact found and
fixed this way (see the `futex_rw_mutex` commit history).

Known gap (tracked, not yet addressed): test coverage for `sweat_shop`'s own dispatch/
thread-pool machinery is currently limited to `sweater_smoke_test`'s three basic-usage
cases — no dedicated stress/adversarial coverage yet (shutdown races, oversubscription,
exception propagation through `dispatch()`, `in_flight_count()`/`wait_until_idle()`
correctness). A concrete research writeup and proposed next steps is on file (ask in the
project's issue tracker or check recent history around this README's last update for the
writeup).
