# One JUCE plugin target produces every format from the same code:
# the standalone app now, AU and VST3 alongside it, and AAX once the SDK is configured.
set(customkeys_formats Standalone VST3)
if(APPLE)
    list(APPEND customkeys_formats AU)
endif()
if(CUSTOMKEYS_AAX_SDK_PATH)
    juce_set_aax_sdk_path("${CUSTOMKEYS_AAX_SDK_PATH}")
    list(APPEND customkeys_formats AAX)
endif()

juce_add_plugin(CustomKeys
    PRODUCT_NAME                "CustomKeys"
    COMPANY_NAME                "CustomKeys"
    BUNDLE_ID                   "com.b3mac.customkeys"
    VERSION                     "${PROJECT_VERSION}"
    PLUGIN_MANUFACTURER_CODE    Ckys
    PLUGIN_CODE                 Ckpn
    FORMATS                     ${customkeys_formats}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    HARDENED_RUNTIME_ENABLED    TRUE
    MICROPHONE_PERMISSION_ENABLED FALSE
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    VST3_CATEGORIES             Instrument Sampler
    AAX_CATEGORY                AAX_ePlugInCategory_SWGenerators)

target_sources(CustomKeys PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(CustomKeys PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE=0)

target_link_libraries(CustomKeys
    PRIVATE
        CustomKeys::Engine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
