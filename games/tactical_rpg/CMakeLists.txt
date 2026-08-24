add_library(ludus-tactical-rpg STATIC
    src/game.cpp
    src/presentation.cpp
)
add_library(ludus::tactical-rpg ALIAS ludus-tactical-rpg)
target_compile_features(ludus-tactical-rpg PUBLIC cxx_std_23)
target_include_directories(ludus-tactical-rpg
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ludus-tactical-rpg PUBLIC ludus::python ludus::render)
ludus_set_project_warnings(ludus-tactical-rpg)
ludus_enable_sanitizers(ludus-tactical-rpg)

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/python/tactical_rpg"
     DESTINATION "${CMAKE_BINARY_DIR}/python")

if(LUDUS_BUILD_TESTS)
    add_executable(ludus-tactical-rpg-tests tests/test_tactical.cpp)
    target_link_libraries(ludus-tactical-rpg-tests
        PRIVATE ludus::tactical-rpg Catch2::Catch2WithMain
    )
    target_compile_definitions(ludus-tactical-rpg-tests PRIVATE
        LUDUS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    ludus_set_project_warnings(ludus-tactical-rpg-tests)
    ludus_enable_sanitizers(ludus-tactical-rpg-tests)
    add_test(NAME ludus-tactical-rpg-tests COMMAND ludus-tactical-rpg-tests)
endif()
