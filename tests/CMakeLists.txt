add_executable(customkeys_engine_tests
    TestMain.cpp
    EngineTests.cpp)

target_link_libraries(customkeys_engine_tests PRIVATE CustomKeys::Engine)

add_test(NAME engine COMMAND customkeys_engine_tests)
