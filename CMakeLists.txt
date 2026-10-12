cmake_minimum_required(VERSION 3.20)
project(Phylotone VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core: JUCE-free sequencing and timing. No platform dependencies.
add_library(phylo_core STATIC
    core/src/Sequencer.cpp
    core/src/Scene.cpp
    core/src/Json.cpp
    core/src/Assistant.cpp
    core/src/Generator.cpp
    core/src/Wire.cpp
    core/src/Fnv1a.cpp
    core/src/Vst3Preset.cpp
    core/src/Mixer.cpp
    core/src/Wav.cpp
    core/src/samples/SampleBlob.cpp
    core/src/samples/SampleImport.cpp
    core/src/samples/SampleLibrary.cpp
)
target_include_directories(phylo_core PUBLIC core/include)
target_compile_options(phylo_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

enable_testing()

option(PHYLO_BUILD_APP "Build the Standalone app (fetches JUCE)" OFF)
if(PHYLO_BUILD_APP)
    add_subdirectory(app)
endif()

option(PHYLO_BUILD_TESTS "Build the core unit tests" ON)
if(PHYLO_BUILD_TESTS)
    add_executable(phylo_core_tests tests/core/test_core.cpp)
    target_link_libraries(phylo_core_tests PRIVATE phylo_core)
    target_compile_options(phylo_core_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    add_test(NAME phylo_core COMMAND phylo_core_tests)

    # Sample library (docs/SAMPLE_INTERFACE.md): the blob codec, WAV import and the library
    # folder. One program each, sharing the runner in tests/core/check.h.
    foreach(suite sample_blob sample_import sample_library host_delivery)
        add_executable(phylo_${suite}_tests tests/core/test_${suite}.cpp)
        target_link_libraries(phylo_${suite}_tests PRIVATE phylo_core)
        target_compile_options(phylo_${suite}_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
        )
        add_test(NAME phylo_${suite} COMMAND phylo_${suite}_tests)
    endforeach()
endif()

option(PHYLO_BUILD_PROVIDERS "Build the assistant chat-provider adapters (fetches nlohmann/json)" OFF)
if(PHYLO_BUILD_PROVIDERS)
    add_subdirectory(app/providers)
endif()
