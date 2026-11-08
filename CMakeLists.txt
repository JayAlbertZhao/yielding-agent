cmake_minimum_required(VERSION 3.20)
project(yielding_agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no runtime deps
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /utf-8)

add_library(yielding_model STATIC src/model.cpp)
target_include_directories(yielding_model PUBLIC src)

set(APP_SOURCES src/main.cpp src/platform.cpp src/daemon.cpp src/gpu.cpp src/audio.cpp)
set(APP_LIBS yielding_model pdh dxgi ole32 shell32 user32 wtsapi32 advapi32 psapi)

add_executable(yielding-agent ${APP_SOURCES})
target_link_libraries(yielding-agent PRIVATE ${APP_LIBS})

add_executable(yielding-agentw WIN32 ${APP_SOURCES})
target_compile_definitions(yielding-agentw PRIVATE YIELDING_GUI)
target_link_options(yielding-agentw PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(yielding-agentw PRIVATE ${APP_LIBS})

enable_testing()
add_executable(test_model tests/test_model.cpp)
target_link_libraries(test_model PRIVATE yielding_model)
add_test(NAME model COMMAND test_model)
