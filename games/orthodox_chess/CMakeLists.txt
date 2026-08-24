add_library(ludus-orthodox-chess STATIC
    src/game.cpp
    src/match.cpp
    src/pgn.cpp
    src/presentation.cpp
    src/position.cpp
)
add_library(ludus::orthodox-chess ALIAS ludus-orthodox-chess)
target_compile_features(ludus-orthodox-chess PUBLIC cxx_std_23)
target_include_directories(ludus-orthodox-chess
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ludus-orthodox-chess PUBLIC ludus::python ludus::render)
ludus_set_project_warnings(ludus-orthodox-chess)
ludus_enable_sanitizers(ludus-orthodox-chess)

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/python/orthodox_chess"
     DESTINATION "${CMAKE_BINARY_DIR}/python")

if(LUDUS_BUILD_TESTS)
    add_executable(ludus-orthodox-chess-tests tests/test_chess.cpp)
    target_link_libraries(ludus-orthodox-chess-tests
        PRIVATE ludus::orthodox-chess ludus::studio-core Catch2::Catch2WithMain
    )
    target_compile_definitions(ludus-orthodox-chess-tests PRIVATE
        LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    ludus_set_project_warnings(ludus-orthodox-chess-tests)
    ludus_enable_sanitizers(ludus-orthodox-chess-tests)
    add_test(NAME ludus-orthodox-chess-tests COMMAND ludus-orthodox-chess-tests)
endif()
