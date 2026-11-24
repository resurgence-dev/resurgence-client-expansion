cmake_minimum_required(VERSION 3.20)
project(rcx CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

# The Client Expansion is a Windows x64 client module only (docs/client-expansion-design.md, Layout).
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "gmcl_rcx builds for 64-bit Windows only")
endif()

set(RCX_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/builds/win64" CACHE PATH "Folder the module and openvr_api.dll are written to")
set(RCX_OPENVR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr" CACHE PATH "OpenVR SDK 2.15.6 (headers/openvr.h, bin/win64/openvr_api.dll)")
set(RCX_GMOD_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gmod-module-base" CACHE PATH "gmod-module-base, development branch")

if(NOT EXISTS "${RCX_OPENVR_DIR}/headers/openvr.h")
	message(FATAL_ERROR "OpenVR SDK not found at ${RCX_OPENVR_DIR}: see README.md")
endif()
if(NOT EXISTS "${RCX_GMOD_BASE_DIR}/include/GarrysMod/Lua/Interface.h")
	message(FATAL_ERROR "gmod-module-base not found at ${RCX_GMOD_BASE_DIR}: see README.md")
endif()

add_library(gmcl_rcx SHARED
	src/module.cpp
	src/core/core.cpp
	src/vr/openvr_loader.cpp
	src/vr/vr_session.cpp
	src/vr/vr_input.cpp
	src/vr/d3d9_bridge.cpp
	src/vr/vr_lua.cpp
	src/ik/ik_solver.cpp
	src/ik/ik_lua.cpp
	src/gpu/gpu_timing.cpp
	src/gpu/gpu_lua.cpp
	src/engine/engine_guard.cpp
	src/engine/engine_lua.cpp
	src/mesh/mesh_merge.cpp
	src/mesh/mesh_worker.cpp
	src/mesh/mesh_lua.cpp
	src/sys/sys_memory.cpp
	src/sys/sys_lua.cpp
)
target_include_directories(gmcl_rcx PRIVATE
	src
	"${RCX_GMOD_BASE_DIR}/include"
	"${RCX_OPENVR_DIR}/headers"
)
# Nothing links openvr_api, d3d9, d3d11 or dxgi: each is loaded by hand when VR starts, so loading rcx loads no third-party DLL.
target_compile_definitions(gmcl_rcx PRIVATE GMMODULE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(gmcl_rcx PRIVATE /W4 /EHsc /permissive-)
# The static runtime: one DLL to install, no Visual C++ redistributable needed.
set_property(TARGET gmcl_rcx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(gmcl_rcx PROPERTIES
	OUTPUT_NAME "gmcl_rcx_win64"
	PREFIX ""
	SUFFIX ".dll"
	RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/$<0:>"
	LIBRARY_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/$<0:>"
)
# The IK solver's unit tests: pure C++, no GMod or OpenVR (tests/ik_solver_test.cpp).
add_executable(rcx_ik_test tests/ik_solver_test.cpp src/ik/ik_solver.cpp)
target_include_directories(rcx_ik_test PRIVATE src)
target_compile_options(rcx_ik_test PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET rcx_ik_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(rcx_ik_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/$<0:>")

# Writes the input files from a scripted set of actions, for tools/check_input_files.py to check (no SteamVR needed).
add_executable(rcx_input_files_test tests/input_files_test.cpp src/vr/vr_input.cpp src/core/core.cpp)
target_include_directories(rcx_input_files_test PRIVATE src "${RCX_OPENVR_DIR}/headers")
target_compile_definitions(rcx_input_files_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rcx_input_files_test PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET rcx_input_files_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(rcx_input_files_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/tests/$<0:>")

add_custom_command(TARGET gmcl_rcx POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RCX_OPENVR_DIR}/bin/win64/openvr_api.dll" "${RCX_OUTPUT_DIR}/openvr_api.dll"
)

# The engine safety layer's build check, policy file and crash latch outside the game (tests/engine_guard_test.cpp).
add_executable(rcx_engine_guard_test tests/engine_guard_test.cpp src/engine/engine_guard.cpp src/core/core.cpp)
target_include_directories(rcx_engine_guard_test PRIVATE src)
target_compile_definitions(rcx_engine_guard_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(rcx_engine_guard_test PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET rcx_engine_guard_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(rcx_engine_guard_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/tests/$<0:>")

# The merged-mesh builder against the Python prototype's output (tests/mesh_merge_test.cpp, fixtures from tools/mesh_prototype).
add_executable(rcx_mesh_merge_test tests/mesh_merge_test.cpp src/mesh/mesh_merge.cpp)
target_include_directories(rcx_mesh_merge_test PRIVATE src)
target_compile_definitions(rcx_mesh_merge_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rcx_mesh_merge_test PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET rcx_mesh_merge_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(rcx_mesh_merge_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/tests/$<0:>")

# rcx.sys's memory reading on this machine (tests/sys_memory_test.cpp): process, physical, commit and VRAM numbers are sane.
add_executable(rcx_sys_memory_test tests/sys_memory_test.cpp src/sys/sys_memory.cpp)
target_include_directories(rcx_sys_memory_test PRIVATE src)
target_compile_definitions(rcx_sys_memory_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rcx_sys_memory_test PRIVATE /W4 /EHsc /permissive-)
set_property(TARGET rcx_sys_memory_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(rcx_sys_memory_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${RCX_OUTPUT_DIR}/tests/$<0:>")
