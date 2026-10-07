# Phylotone Standalone app. JUCE is fetched at configure time, pinned to a tag.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(PhylotoneApp
    PRODUCT_NAME "Phylotone"
    COMPANY_NAME "AdairBear"
    BUNDLE_ID "com.adairbear.phylotone"
    VERSION ${PROJECT_VERSION}
)

juce_generate_juce_header(PhylotoneApp)

target_sources(PhylotoneApp PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/PlaybackEngine.cpp
)

target_compile_definitions(PhylotoneApp PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(PhylotoneApp PRIVATE
    phylo_core
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
