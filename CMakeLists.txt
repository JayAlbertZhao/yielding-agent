cmake_minimum_required(VERSION 3.20)
project(afk_governor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no runtime deps
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /utf-8)

add_library(afk_model STATIC src/model.cpp)
target_include_directories(afk_model PUBLIC src)

set(APP_SOURCES src/main.cpp src/platform.cpp src/daemon.cpp src/gpu.cpp src/audio.cpp)
set(APP_LIBS afk_model pdh dxgi ole32 shell32 user32 wtsapi32 advapi32 psapi)

add_executable(afk-governor ${APP_SOURCES})
target_link_libraries(afk-governor PRIVATE ${APP_LIBS})

add_executable(afk-governorw WIN32 ${APP_SOURCES})
target_compile_definitions(afk-governorw PRIVATE AFK_GUI)
target_link_options(afk-governorw PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(afk-governorw PRIVATE ${APP_LIBS})

enable_testing()
add_executable(test_model tests/test_model.cpp)
target_link_libraries(test_model PRIVATE afk_model)
add_test(NAME model COMMAND test_model)
