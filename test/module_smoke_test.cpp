// Module correctness smoke test (module.cmake, PSI_SWEATER_MODULE) - only built when the option
// is on, so it adds nothing to the default (off) CI matrix.
import psi.sweater;

#include <atomic>

int main()
{
    std::atomic<int> sum{ 0 };
    psi::sweater::shop work_shop;
    work_shop.spread_the_sweat( 1000, [ & ]( auto const start, auto const end ) noexcept
    {
        for ( auto i{ start }; i < end; ++i )
            sum.fetch_add( 1, std::memory_order_relaxed );
    } );
    return sum.load() == 1000 ? 0 : 1;
}
