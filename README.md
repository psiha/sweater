# sweater
#### Humble Outer Dispatch

Cpp still does not have a standard 'parallel work dispatching' solution, AKA a thread pool. I dislike that name however, since unless the API provides access to the underlying threads, a thread pool is but one possible implementation (detail) of a 'parallel work dispatching' library.

Any name involving 'parallel' is a tongue breaker and 'worker' sounds too 'red' hence sweater - legal, portable, machine sweat shops that sweat for you.

(not an actual/official Boost library, simply started as another wannabe proposal)


#### Dependencies

[Functionoid](https://github.com/psiha/functionoid)
[ConfigEx](https://github.com/psiha/config_ex)
[moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)