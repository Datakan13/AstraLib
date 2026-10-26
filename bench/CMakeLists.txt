# Comparison benchmarks: AstraLib vs established open-source equivalents.
#
# Only configured when -DBUILD_COMPARISON_BENCH=ON, so a normal build (and CI)
# never downloads anything. Competitors are header-only and fetched as
# archives pinned by SHA256 — a changed upstream archive fails the configure
# step instead of silently benchmarking different code.
#
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_COMPARISON_BENCH=ON
#     cmake --build build -j
#     ctest --test-dir build -L comparison --output-on-failure -V
#
# Each binary can also be run directly (build/bench/<Name>), which is the
# better way to read the tables.

include(FetchContent)

# SOURCE_SUBDIR points at a directory that doesn't exist, so MakeAvailable
# only downloads and unpacks — the libraries' own CMakeLists (tests,
# install rules, extra options) never run.
macro(astra_fetch name url sha)
    FetchContent_Declare(${name} URL ${url} URL_HASH SHA256=${sha}
                         SOURCE_SUBDIR __headers_only__)
    FetchContent_MakeAvailable(${name})
endmacro()

astra_fetch(rigtorp_mpmc
    https://github.com/rigtorp/MPMCQueue/archive/b9808ede08f26fa9df4df4e081d19cace8f6c6ea.tar.gz
    bdfcf2429aebe892eb7b4a2edbc2d889365fa52264bf9501d937e8484166ec6a)
astra_fetch(rigtorp_spsc
    https://github.com/rigtorp/SPSCQueue/archive/refs/tags/v1.1.tar.gz
    10f290c72e60ba0d7999c65926f96fab0b1c2f360ce5bbcfe0cf69448d067af5)
astra_fetch(moodycamel_cq
    https://github.com/cameron314/concurrentqueue/archive/refs/tags/v1.0.4.tar.gz
    87fbc9884d60d0d4bf3462c18f4c0ee0a9311d0519341cac7cbd361c885e5281)
astra_fetch(moodycamel_rwq
    https://github.com/cameron314/readerwriterqueue/archive/refs/tags/v1.0.6.tar.gz
    fc68f55bbd49a8b646462695e1777fb8f2c0b4f342d5e6574135211312ba56c1)
astra_fetch(bs_thread_pool
    https://github.com/bshoshany/thread-pool/archive/refs/tags/v5.0.0.tar.gz
    617a8fbc2c360577f498998f336777c73d581810831d4ce9c920f11ec680b07b)

find_package(Boost 1.65 REQUIRED)   # lockfree is header-only

add_library(astra_bench_competitors INTERFACE)
target_include_directories(astra_bench_competitors SYSTEM INTERFACE
    ${rigtorp_mpmc_SOURCE_DIR}/include
    ${rigtorp_spsc_SOURCE_DIR}/include
    ${moodycamel_cq_SOURCE_DIR}
    ${moodycamel_rwq_SOURCE_DIR}
    ${bs_thread_pool_SOURCE_DIR}/include)
target_link_libraries(astra_bench_competitors INTERFACE Boost::headers)

# Labelled `perf` as well as `comparison`, so the existing `ctest -LE perf`
# exclusion keeps them out of a default run even when this option is ON.
function(astra_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE AstraLib astra_bench_competitors pthread)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 1800 LABELS "perf;comparison")
endfunction()

astra_bench(QueueComparisonBench)
astra_bench(SpinlockComparisonBench)
astra_bench(ThreadPoolComparisonBench)
astra_bench(IndexPoolComparisonBench)
