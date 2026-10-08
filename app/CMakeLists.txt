# Phylotone Standalone app. JUCE is fetched at configure time, pinned to a tag.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# The assistant's chat-provider adapters. The root adds them only when
# PHYLO_BUILD_PROVIDERS is set; the app always needs them.
if(NOT TARGET phylo_providers)
    add_subdirectory(providers)
endif()

# JUCE's own Linux networking is plain HTTP only. The assistant's cloud providers
# need HTTPS, so Linux builds use libcurl. macOS and Windows use the system stack.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PHYLO_NEEDS_CURL TRUE)
else()
    set(PHYLO_NEEDS_CURL FALSE)
endif()

juce_add_gui_app(PhylotoneApp
    PRODUCT_NAME "Phylotone"
    COMPANY_NAME "AdairBear"
    BUNDLE_ID "com.adairbear.phylotone"
    VERSION ${PROJECT_VERSION}
    NEEDS_CURL ${PHYLO_NEEDS_CURL}
)

juce_generate_juce_header(PhylotoneApp)

target_sources(PhylotoneApp PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/PlaybackEngine.cpp
    src/AssistantSettings.cpp
    src/JuceHttpPost.cpp
    src/AssistantController.cpp
    src/AssistantPanel.cpp
)

target_compile_definitions(PhylotoneApp PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(PhylotoneApp PRIVATE
    phylo_core
    phylo_providers
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# The plugin host: one plugin per process, so a crashing plugin cannot take the app down.
juce_add_console_app(phylo-plughost
    PRODUCT_NAME "phylo-plughost"
)
juce_generate_juce_header(phylo-plughost)
target_sources(phylo-plughost PRIVATE plughost/PluginHostMain.cpp)
target_compile_definitions(phylo-plughost PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(phylo-plughost PRIVATE
    phylo_core
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# The plugin host supervisor's kill test. Plain C++, no JUCE.
add_executable(phylo_supervisor_check
    src/host/SupervisorCheck.cpp
    src/host/PluginHostSupervisor.cpp
    src/host/ProcessPipe.cpp
)
target_include_directories(phylo_supervisor_check PRIVATE src/host)
target_link_libraries(phylo_supervisor_check PRIVATE phylo_core)
add_dependencies(phylo_supervisor_check phylo-plughost)
add_test(NAME phylo_supervisor_kill COMMAND phylo_supervisor_check $<TARGET_FILE:phylo-plughost>)

# Headless check of the assistant settings file format (no JUCE).
add_executable(phylo_app_checks
    src/AssistantSettings.cpp
    src/AssistantSettingsCheck.cpp
)
target_link_libraries(phylo_app_checks PRIVATE phylo_core)
add_test(NAME phylo_app_checks COMMAND phylo_app_checks)
target_compile_options(phylo_app_checks PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
