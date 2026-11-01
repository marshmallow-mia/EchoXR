# EchoXR's tests. They are Windows programs: CI runs them on its Windows runner; elsewhere
# they build, and run under Wine if you point ctest at it.

# Pure decisions from xr/src/echoxr_policy.h.
add_executable(runtime_units runtime_units.cpp)
target_include_directories(runtime_units PRIVATE ${CMAKE_SOURCE_DIR}/xr/src)
set_target_properties(runtime_units PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME runtime_units COMMAND runtime_units)

# The controller settings and pose math (xr/src/echoxr_settings.h, echoxr_pose.h); also builds
# natively: c++ -std=c++17 -Ixr/src tests/controller_units.cpp
add_executable(controller_units controller_units.cpp)
target_include_directories(controller_units PRIVATE ${CMAKE_SOURCE_DIR}/xr/src)
set_target_properties(controller_units PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME controller_units COMMAND controller_units)

# The built runtime, with no OpenXR runtime to talk to: it must fail cleanly, not crash.
add_executable(smoke_no_runtime smoke_no_runtime.cpp)
add_dependencies(smoke_no_runtime echoxr_runtime)
target_include_directories(smoke_no_runtime PRIVATE ${ovr_sdk_SOURCE_DIR}/LibOVR/Include)
set_target_properties(smoke_no_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME smoke_no_runtime COMMAND smoke_no_runtime $<TARGET_FILE:echoxr_runtime>)
set_tests_properties(smoke_no_runtime PROPERTIES
    ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/tests/no-such-runtime.json")

# EchoXR's Platform SDK stand-in, asked as the game's pnsovr.dll asks it.
add_executable(platform_signin platform_signin.cpp)
add_dependencies(platform_signin echoxr_platform)
target_compile_definitions(platform_signin PRIVATE UNICODE _UNICODE)
set_target_properties(platform_signin PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_test(NAME platform_signin COMMAND platform_signin $<TARGET_FILE:echoxr_platform>)

# The watches on a real D3D12 device, each run twice: with the default lines and with
# ECHOXR_VERBOSE=max.
set(WATCH_SOURCES ${CMAKE_SOURCE_DIR}/xr/src/diagnostics.cpp ${CMAKE_SOURCE_DIR}/xr/src/queue_watch.cpp
    ${CMAKE_SOURCE_DIR}/xr/src/resource_watch.cpp)
function(watch_test name)
    add_executable(${name} ${name}.cpp ${WATCH_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/xr/src)
    target_link_libraries(${name} PRIVATE detours d3d12 dxgi dxguid)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "ECHOXR_VERBOSE=default")
    add_test(NAME ${name}_max COMMAND ${name})
    set_tests_properties(${name}_max PROPERTIES ENVIRONMENT "ECHOXR_VERBOSE=max")
endfunction()

# The device-removal watch (xr/src/diagnostics.cpp) on a D3D12 device removed on purpose.
watch_test(device_watch)
# The GPU-wait watch (xr/src/queue_watch.cpp): a queue waiting for a fence nobody signals yet.
watch_test(queue_watch)
# The allocation watch (xr/src/resource_watch.cpp): allocations no device can make.
watch_test(resource_watch)
