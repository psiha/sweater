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
- `sweater_rw_mutex_test` — `rw_mutex` (pthread_rwlock/SRWLOCK-backed).
- `sweater_rrw_mutex_test` — `rrw_mutex` (reentrant-read TLS wrapper).
- `sweater_rw_preference_test` — `reader_preferring_rw_mutex` vs `writer_preferring_rw_mutex`
  (Linux-only; self-skips elsewhere via `GTEST_SKIP`, no glibc NP kind-selection API there).
- `sweater_futex_test` — low-level `psi::thrd_lite::futex` wait/wake, including the
  bitset-targeted `wake_all`/`wait_if_equal` overloads (real filtering on Linux
  `FUTEX_WAIT_BITSET`/`FUTEX_WAKE_BITSET`; a documented no-op elsewhere).
- `sweater_futex_rw_mutex_test` — `futex_rw_mutex`/`reader_preferring_futex_rw_mutex`
  (bleeding-edge, `__ulock`-based on Apple — see that header's own design-doc comment on
  why this isn't a production path there).
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
correctness). The mutex-family tests are also still one hand-duplicated file per type
rather than a single parameterized suite run across all of them. Both gaps have a
concrete research writeup and proposed next steps on file (ask in the project's issue
tracker or check recent history around this README's last update for the writeup).