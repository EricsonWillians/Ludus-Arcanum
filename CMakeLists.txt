cmake_minimum_required(VERSION 3.28)

project(
    LudusArcanum
    VERSION 0.1.0
    DESCRIPTION "A deterministic framework and visual studio for programmable board games"
    HOMEPAGE_URL "https://github.com/EricsonWillians/Ludus-Arcanum"
    LANGUAGES CXX
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LudusCompilerOptions)

option(LUDUS_BUILD_TESTS "Build the Ludus Arcanum test suite" ${PROJECT_IS_TOP_LEVEL})
option(LUDUS_BUILD_BENCHMARKS "Build microbenchmarks" OFF)
option(LUDUS_BUILD_PYTHON "Build embedded CPython integration and SDK extension" ON)
option(LUDUS_BUILD_GUI "Build gtkmm/OpenGL applications when dependencies exist" ON)
option(LUDUS_REQUIRE_GUI "Fail configuration unless all optional GUI targets can be built" OFF)
option(LUDUS_FETCH_DEPENDENCIES "Fetch missing third-party dependencies" ON)
option(LUDUS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LUDUS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

add_library(ludus-core STATIC
    src/core/binary.cpp
    src/core/entity_store.cpp
    src/core/version.cpp
)
add_library(ludus::core ALIAS ludus-core)
target_compile_features(ludus-core PUBLIC cxx_std_23)
target_include_directories(ludus-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(ludus-core
    PRIVATE
        LUDUS_VERSION="${PROJECT_VERSION}"
)
ludus_set_project_warnings(ludus-core)
ludus_enable_sanitizers(ludus-core)

add_library(ludus-topology STATIC
    src/topology/topology.cpp
)
add_library(ludus::topology ALIAS ludus-topology)
target_compile_features(ludus-topology PUBLIC cxx_std_23)
target_include_directories(ludus-topology
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(ludus-topology PUBLIC ludus::core)
ludus_set_project_warnings(ludus-topology)
ludus_enable_sanitizers(ludus-topology)

add_library(ludus-rules STATIC
    src/rules/effect.cpp
    src/rules/event.cpp
    src/rules/game_state.cpp
    src/rules/random.cpp
    src/rules/session.cpp
    src/rules/transaction.cpp
)
add_library(ludus::rules ALIAS ludus-rules)
target_compile_features(ludus-rules PUBLIC cxx_std_23)
target_include_directories(ludus-rules
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(ludus-rules PUBLIC ludus::core ludus::topology)
ludus_set_project_warnings(ludus-rules)
# GCC 13 reports false positives while moving std::variant alternatives that contain
# std::optional<std::variant<...>> under -O3 (libstdc++ PR 114592 family).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ludus-rules PRIVATE -Wno-maybe-uninitialized)
endif()
ludus_enable_sanitizers(ludus-rules)

add_library(ludus-rule-ir STATIC
    src/rule_ir/program.cpp
)
add_library(ludus::rule-ir ALIAS ludus-rule-ir)
target_compile_features(ludus-rule-ir PUBLIC cxx_std_23)
target_include_directories(ludus-rule-ir
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(ludus-rule-ir PUBLIC ludus::rules)
ludus_set_project_warnings(ludus-rule-ir)
ludus_enable_sanitizers(ludus-rule-ir)

find_package(PNG 1.6 REQUIRED)

add_library(ludus-render STATIC
    src/render/animation.cpp
    src/render/atlas.cpp
    src/render/batch.cpp
    src/render/camera.cpp
    src/render/snapshot.cpp
    src/render/theme.cpp
)
add_library(ludus::render ALIAS ludus-render)
target_compile_features(ludus-render PUBLIC cxx_std_23)
target_include_directories(ludus-render
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(ludus-render PUBLIC ludus::rules PRIVATE PNG::PNG)
ludus_set_project_warnings(ludus-render)
ludus_enable_sanitizers(ludus-render)

add_library(ludus-studio-core STATIC
    src/studio/inspection.cpp
    src/studio/package_document.cpp
    src/studio/toml.cpp
)
add_library(ludus::studio-core ALIAS ludus-studio-core)
target_compile_features(ludus-studio-core PUBLIC cxx_std_23)
target_include_directories(ludus-studio-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/studio"
)
target_link_libraries(ludus-studio-core PUBLIC ludus::render)
ludus_set_project_warnings(ludus-studio-core)
ludus_enable_sanitizers(ludus-studio-core)

if(LUDUS_BUILD_PYTHON)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development Development.Module)
    find_package(pybind11 3.1 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        if(NOT LUDUS_FETCH_DEPENDENCIES)
            message(FATAL_ERROR
                "pybind11 3.1 is required for Python support. Install pybind11 or enable LUDUS_FETCH_DEPENDENCIES."
            )
        endif()
        include(FetchContent)
        FetchContent_Declare(
            pybind11
            URL https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz
            URL_HASH SHA256=ef712655692a2e9bf7bb7874c022564a45f91d847ddee987e720cd9e28849665
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        )
        FetchContent_MakeAvailable(pybind11)
    endif()

    add_library(ludus-python STATIC
        src/python/bindings.cpp
        src/python/runtime.cpp
    )
    add_library(ludus::python ALIAS ludus-python)
    target_compile_features(ludus-python PUBLIC cxx_std_23)
    target_include_directories(ludus-python
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:include>"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/python"
    )
    target_link_libraries(ludus-python PUBLIC ludus::rule-ir PRIVATE pybind11::embed)
    ludus_set_project_warnings(ludus-python)
    ludus_enable_sanitizers(ludus-python)

    pybind11_add_module(ludus-python-extension MODULE NO_EXTRAS
        src/python/bindings.cpp
        src/python/extension_module.cpp
    )
    target_compile_features(ludus-python-extension PRIVATE cxx_std_23)
    target_include_directories(ludus-python-extension PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/python"
    )
    target_link_libraries(ludus-python-extension PRIVATE ludus::rule-ir)
    set_target_properties(ludus-python-extension PROPERTIES
        OUTPUT_NAME "_native"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/python/ludus_arcanum"
    )
    ludus_set_project_warnings(ludus-python-extension)
    ludus_enable_sanitizers(ludus-python-extension)

    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/python/ludus_arcanum"
         DESTINATION "${CMAKE_BINARY_DIR}/python")
endif()

add_executable(ludus-server apps/server/main.cpp)
target_link_libraries(ludus-server PRIVATE ludus::rule-ir)
ludus_set_project_warnings(ludus-server)
ludus_enable_sanitizers(ludus-server)

if(LUDUS_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LUDUS_BUILD_PYTHON)
    add_subdirectory(games/orthodox_chess)
    add_subdirectory(games/tactical_rpg)
endif()

if(LUDUS_REQUIRE_GUI AND NOT LUDUS_BUILD_GUI)
    message(FATAL_ERROR "LUDUS_REQUIRE_GUI requires LUDUS_BUILD_GUI=ON")
endif()

if(LUDUS_BUILD_GUI)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTKMM QUIET IMPORTED_TARGET gtkmm-4.0)
        pkg_check_modules(EPOXY QUIET IMPORTED_TARGET epoxy)
    endif()

    if(TARGET PkgConfig::GTKMM AND TARGET PkgConfig::EPOXY)
        add_library(ludus-gtk STATIC
            src/gtk/board_canvas.cpp
            src/gtk/board_gl_area.cpp
            src/gtk/renderer.cpp
        )
        add_library(ludus::gtk ALIAS ludus-gtk)
        target_compile_features(ludus-gtk PUBLIC cxx_std_23)
        target_include_directories(ludus-gtk
            PUBLIC
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                "$<INSTALL_INTERFACE:include>"
        )
        target_link_libraries(ludus-gtk
            PUBLIC ludus::render PkgConfig::GTKMM
            PRIVATE PkgConfig::EPOXY
        )
        ludus_set_project_warnings(ludus-gtk)
        ludus_enable_sanitizers(ludus-gtk)

        if(TARGET ludus-orthodox-chess AND TARGET ludus-tactical-rpg)
            find_package(Threads REQUIRED)
            add_executable(ludus-player
                apps/player/main.cpp
                apps/player/player_controller.cpp
                apps/player/player_window.cpp
            )
            target_include_directories(ludus-player PRIVATE apps/player)
            target_compile_definitions(ludus-player PRIVATE
                LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                LUDUS_BINARY_DIR="${CMAKE_BINARY_DIR}"
            )
            target_link_libraries(ludus-player PRIVATE
                ludus::gtk ludus::orthodox-chess ludus::tactical-rpg Threads::Threads
            )
            ludus_set_project_warnings(ludus-player)
            ludus_enable_sanitizers(ludus-player)

            add_executable(ludus-studio
                apps/studio/main.cpp
                apps/studio/studio_controller.cpp
                apps/studio/studio_window.cpp
            )
            target_include_directories(ludus-studio PRIVATE apps/studio)
            target_compile_definitions(ludus-studio PRIVATE
                LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                LUDUS_BINARY_DIR="${CMAKE_BINARY_DIR}"
            )
            target_link_libraries(ludus-studio PRIVATE
                ludus::gtk ludus::orthodox-chess ludus::studio-core Threads::Threads
            )
            ludus_set_project_warnings(ludus-studio)
            ludus_enable_sanitizers(ludus-studio)
        else()
            if(LUDUS_REQUIRE_GUI)
                message(FATAL_ERROR
                    "The GUI gate requires Python support plus the orthodox-chess "
                    "and tactical-rpg packages"
                )
            else()
                message(STATUS
                    "ludus-player and ludus-studio require Python support and the "
                    "reference game packages"
                )
            endif()
        endif()
    else()
        if(LUDUS_REQUIRE_GUI)
            message(FATAL_ERROR
                "The GUI gate requires pkg-config modules gtkmm-4.0 and epoxy"
            )
        else()
            message(STATUS
                "gtkmm-4.0 or libepoxy development files not found; "
                "ludus-gtk, ludus-player, and ludus-studio are disabled"
            )
        endif()
    endif()
endif()

if(LUDUS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

find_program(LUDUS_CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-18)
if(LUDUS_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE LUDUS_FORMAT_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/games/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/games/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )
    add_custom_target(format
        COMMAND "${LUDUS_CLANG_FORMAT_EXECUTABLE}" -i ${LUDUS_FORMAT_FILES}
        COMMENT "Formatting Ludus Arcanum C++ sources"
        VERBATIM
    )
endif()
