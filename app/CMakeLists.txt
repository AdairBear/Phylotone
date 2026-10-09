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
    src/AudioOutput.cpp
    src/PlaybackSettings.cpp
    src/host/AudioFeed.cpp
    src/host/OfflineTake.cpp
    src/host/PipelinedRenderer.cpp
    src/host/PluginHostSupervisor.cpp
    src/host/ProcessPipe.cpp
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
target_compile_definitions(phylo-plughost PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
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

# Headless check that the pipelined renderer never blocks its caller. Plain C++, no JUCE.
find_package(Threads REQUIRED)
add_executable(phylo_renderer_check
    src/host/RendererCheck.cpp
    src/host/PipelinedRenderer.cpp
    src/host/PluginHostSupervisor.cpp
    src/host/ProcessPipe.cpp
)
target_include_directories(phylo_renderer_check PRIVATE src/host)
target_link_libraries(phylo_renderer_check PRIVATE phylo_core Threads::Threads)
add_dependencies(phylo_renderer_check phylo-plughost)
add_test(NAME phylo_renderer_pipelined COMMAND phylo_renderer_check $<TARGET_FILE:phylo-plughost>)

# Headless check of the offline take renderer (M4): note timing and block-size
# independence, with a silent renderer. Plain C++, no JUCE.
add_executable(phylo_offline_take_check
    src/host/OfflineTakeCheck.cpp
    src/host/OfflineTake.cpp
    src/host/PluginHostSupervisor.cpp
    src/host/ProcessPipe.cpp
)
target_include_directories(phylo_offline_take_check PRIVATE src/host)
target_link_libraries(phylo_offline_take_check PRIVATE phylo_core Threads::Threads)
add_dependencies(phylo_offline_take_check phylo-plughost)
add_test(NAME phylo_offline_take COMMAND phylo_offline_take_check $<TARGET_FILE:phylo-plughost>)

# Headless check of the audio feed (M4): chunking, offline match, dropped blocks, stop.
# Uses a synchronous fake sink. Plain C++, no JUCE.
add_executable(phylo_audio_feed_check
    src/host/AudioFeedCheck.cpp
    src/host/AudioFeed.cpp
    src/host/OfflineTake.cpp
)
target_include_directories(phylo_audio_feed_check PRIVATE src/host)
target_link_libraries(phylo_audio_feed_check PRIVATE phylo_core)
add_test(NAME phylo_audio_feed COMMAND phylo_audio_feed_check)

# Headless check of the playback settings file and environment fallback. Plain C++.
add_executable(phylo_playback_settings_check
    src/PlaybackSettingsCheck.cpp
    src/PlaybackSettings.cpp
)
add_test(NAME phylo_playback_settings COMMAND phylo_playback_settings_check)

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
