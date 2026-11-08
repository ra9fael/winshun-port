cmake_minimum_required(VERSION 3.21)
project(port LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The exe builds straight into the plugin folder, because plugin.json names
# it as runtime.program. Keep the ra9fael.shell convention.
add_executable(port main.cpp)
target_compile_definitions(port PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# iphlpapi: GetExtendedTcpTable / GetExtendedUdpTable. ws2_32: ntohs/htons.
# ole32 + oleaut32 + wbemuuid: WMI (Win32_Process.CommandLine for
# Shift+Enter). shell32: ShellExecuteExW "runas" for the UAC kill. advapi32:
# process/security APIs.
target_link_libraries(port PRIVATE iphlpapi ws2_32 ole32 oleaut32 wbemuuid shell32 advapi32)

if(MSVC)
    target_compile_options(port PRIVATE /utf-8 /W4 /permissive-)
    # Static CRT: no runtime DLL to load from the plugin folder.
    set_target_properties(port PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(port PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
    target_link_options(port PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(port PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(port PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()

# Pure-logic tests: logic.h needs only the SDK header and the C++17 library,
# so they build and run without touching live sockets or processes.
add_executable(port_tests tests/test_port.cpp)
target_include_directories(port_tests PRIVATE "${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_compile_options(port_tests PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(port_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(port_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(port_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tests")
endforeach()

# TestTree: manual E2E helper for killTree (three-level process tree). Lives
# in the build tree and is never shipped with the plugin.
add_executable(TestTree tools/testtree.cpp)
target_compile_definitions(TestTree PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(TestTree PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(TestTree PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(TestTree PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8 -Wall -Wextra -O2)
endif()
set_target_properties(TestTree PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(TestTree PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/tools")
endforeach()

enable_testing()
add_test(NAME port_logic COMMAND port_tests)
