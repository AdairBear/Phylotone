cmake_minimum_required(VERSION 3.20)
project(Phylotone VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core: JUCE-free sequencing and timing. No platform dependencies.
add_library(phylo_core STATIC
    core/src/Sequencer.cpp
)
target_include_directories(phylo_core PUBLIC core/include)
target_compile_options(phylo_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

option(PHYLO_BUILD_APP "Build the Standalone app (fetches JUCE)" OFF)
if(PHYLO_BUILD_APP)
    add_subdirectory(app)
endif()

option(PHYLO_BUILD_TESTS "Build the core unit tests" ON)
if(PHYLO_BUILD_TESTS)
    enable_testing()
    add_executable(phylo_core_tests tests/core/test_sequencer.cpp)
    target_link_libraries(phylo_core_tests PRIVATE phylo_core)
    target_compile_options(phylo_core_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    add_test(NAME phylo_core COMMAND phylo_core_tests)
endif()
