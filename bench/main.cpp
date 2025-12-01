// bench/main.cpp - benchmark entry point
// nanobench needs implementation defined in exactly one translation unit

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

// nothing else needed here, benchmarks register themselves or run in their own functions
// but for this setup we'll just let the linker handle the other files.
// actually, nanobench doesn't have auto-registration like google benchmark.
// we need to call the benchmark functions manually or structure this differently.

// to keep your existing structure (multiple .cpp files), we will declare a runner function
// in each file and call them here.

namespace bench
{
    void run_localhost_benchmarks();
    void run_network_benchmarks();
} // namespace bench

int main()
{
    ankerl::nanobench::Bench().minEpochIterations(1000000); // force 1M iterations minimum

    bench::run_localhost_benchmarks();
    bench::run_network_benchmarks();
    return 0;
}
