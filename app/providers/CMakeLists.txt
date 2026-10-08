# Chat-provider adapters for the assistant (M2). Kept out of core so core stays
# dependency-free. JSON comes from nlohmann/json, fetched and pinned to a tag.
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
    SYSTEM
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

set(PHYLO_PROVIDER_WARNINGS
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_library(phylo_providers STATIC
    src/Common.cpp
    src/AnthropicProvider.cpp
    src/OpenAIProvider.cpp
    src/GeminiProvider.cpp
)
target_include_directories(phylo_providers PUBLIC include)
target_link_libraries(phylo_providers
    PUBLIC phylo_core
    PRIVATE nlohmann_json::nlohmann_json
)
target_compile_options(phylo_providers PRIVATE ${PHYLO_PROVIDER_WARNINGS})

if(PHYLO_BUILD_TESTS)
    add_executable(phylo_providers_tests ${PROJECT_SOURCE_DIR}/tests/providers/test_providers.cpp)
    target_link_libraries(phylo_providers_tests PRIVATE phylo_providers nlohmann_json::nlohmann_json)
    target_compile_options(phylo_providers_tests PRIVATE ${PHYLO_PROVIDER_WARNINGS})
    add_test(NAME phylo_providers COMMAND phylo_providers_tests)
endif()
