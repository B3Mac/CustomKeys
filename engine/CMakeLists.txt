# CustomKeys engine: plain C++20, no JUCE and no UI code.
# Everything in here must stay real-time safe on the audio thread.
add_library(customkeys_engine STATIC
    src/Engine.cpp
    src/Voice.cpp)

add_library(CustomKeys::Engine ALIAS customkeys_engine)

target_include_directories(customkeys_engine PUBLIC include)
target_compile_features(customkeys_engine PUBLIC cxx_std_20)
set_target_properties(customkeys_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(customkeys_engine PRIVATE /W4)
else()
    target_compile_options(customkeys_engine PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
