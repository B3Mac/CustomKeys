# Fetches a pinned JUCE release. Point CUSTOMKEYS_JUCE_PATH at a local JUCE
# checkout to skip the download (useful offline or when trying a new version).
set(CUSTOMKEYS_JUCE_PATH "" CACHE PATH "Optional local JUCE checkout")

if(CUSTOMKEYS_JUCE_PATH)
    add_subdirectory("${CUSTOMKEYS_JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.12
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()
