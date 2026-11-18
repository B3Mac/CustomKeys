cmake_minimum_required(VERSION 3.22)

project(CustomKeys VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    # Universal binary (Apple Silicon + Intel), macOS 11 and later.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")
    endif()
endif()

# The engine is plain C++ with no dependencies, so its tests build anywhere.
# The app (Standalone, AU, VST3 and later AAX) needs JUCE and is on by default on macOS.
option(CUSTOMKEYS_BUILD_APP   "Build the JUCE app and plugin targets" ${APPLE})
option(CUSTOMKEYS_BUILD_TESTS "Build the engine unit tests"           ON)
set(CUSTOMKEYS_AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK. When set, an AAX target is added.")

add_subdirectory(engine)

if(CUSTOMKEYS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CUSTOMKEYS_BUILD_APP)
    include(cmake/JUCE.cmake)
    add_subdirectory(app)
endif()
