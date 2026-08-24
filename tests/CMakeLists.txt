find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    if(NOT LUDUS_FETCH_DEPENDENCIES)
        message(FATAL_ERROR
            "Catch2 3 is required for tests. Install catch2 or enable LUDUS_FETCH_DEPENDENCIES."
        )
    endif()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
        URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

add_executable(ludus-core-tests
    test_ids_and_symbols.cpp
    test_random.cpp
    test_render.cpp
    test_rule_ir.cpp
    test_session.cpp
    test_studio.cpp
    test_topology.cpp
    test_version.cpp
)
target_compile_definitions(ludus-core-tests PRIVATE
    LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(TARGET ludus-python)
    target_sources(ludus-core-tests PRIVATE test_python.cpp)
    target_link_libraries(ludus-core-tests PRIVATE ludus::python)
    target_compile_definitions(ludus-core-tests PRIVATE
        LUDUS_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )
endif()
target_link_libraries(ludus-core-tests PRIVATE
    ludus::render ludus::rule-ir ludus::studio-core Catch2::Catch2WithMain
)
ludus_set_project_warnings(ludus-core-tests)
ludus_enable_sanitizers(ludus-core-tests)

include(Catch)
catch_discover_tests(ludus-core-tests DISCOVERY_MODE PRE_TEST)

add_test(NAME ludus-server-self-check COMMAND ludus-server --self-check)
set_tests_properties(ludus-server-self-check PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: ludus-core"
)

add_test(NAME ludus-server-deterministic-demo COMMAND ludus-server --demo)
set_tests_properties(ludus-server-deterministic-demo PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: deterministic-demo entities=1 events=2 hash=5c9e02f6234e7030"
)

if(TARGET ludus-python)
    find_program(LUDUS_PYTEST_EXECUTABLE NAMES pytest
        HINTS "${CMAKE_BINARY_DIR}/pytest-venv/bin"
    )
    if(LUDUS_PYTEST_EXECUTABLE)
        add_test(NAME ludus-python-sdk-tests
            COMMAND "${LUDUS_PYTEST_EXECUTABLE}" -q
                "${CMAKE_SOURCE_DIR}/tests/python"
                "${CMAKE_SOURCE_DIR}/games/orthodox_chess/tests/test_package.py"
                "${CMAKE_SOURCE_DIR}/games/tactical_rpg/tests/test_tactical_package.py"
        )
        set_tests_properties(ludus-python-sdk-tests PROPERTIES
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_SOURCE_DIR}/games/orthodox_chess/python:${CMAKE_SOURCE_DIR}/games/tactical_rpg/python:${CMAKE_BINARY_DIR}/python"
        )
    else()
        message(STATUS
            "pytest not found; install requirements-dev.txt to register Python SDK tests with CTest"
        )
    endif()
endif()
