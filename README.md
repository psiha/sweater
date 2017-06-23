# sweater
#### Humble Outer Dispatch

In 2017 cpp still does not have a standard 'parallel work dispatching' solution, AKA a thread pool. I dislike that name however, since unless the API provides access to the underlying threads, a thread pool is but one possible implementation (detail) of a 'parallel work dispatching' library.

Since any name involving 'parallel' is a tongue breaker and 'worker' sounds too 'red', I went with sweater as it sweats for you. A legal, portable, machine sweat shop (and it might make you sweat too if you have too many cores and not enough ventilation).

It was stuffed in boost:: as is usual with any component that should IMO one day be standard in some shape or form (i.e. in case it isn't obvious: this is not an official Boost library, just another T(ransparent)N(on-imperialist)U(nopressive)N(imble) software component made public.


#### Prerequisites

[Functionoid](https://github.com/psiha/functionoid)
[ConfigEx](https://github.com/psiha/config_ex)