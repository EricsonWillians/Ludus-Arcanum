find_package(benchmark 1.8 CONFIG QUIET)

if(NOT benchmark_FOUND)
    if(NOT LUDUS_FETCH_DEPENDENCIES)
        message(FATAL_ERROR
            "Google Benchmark is required for benchmarks. Install libbenchmark-dev or enable LUDUS_FETCH_DEPENDENCIES."
        )
    endif()

    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
        URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(googlebenchmark)
endif()

add_executable(ludus-benchmarks benchmark_kernel.cpp)
target_link_libraries(ludus-benchmarks PRIVATE
    ludus::render ludus::rule-ir ludus::studio-core benchmark::benchmark_main
)
if(TARGET ludus-python)
    target_link_libraries(ludus-benchmarks PRIVATE ludus::python ludus::tactical-rpg)
    target_compile_definitions(ludus-benchmarks PRIVATE
        LUDUS_BENCHMARK_PYTHON=1
        LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
endif()
ludus_set_project_warnings(ludus-benchmarks)
