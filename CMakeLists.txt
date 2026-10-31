cmake_minimum_required(VERSION 3.16)
project(alamo_profiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: no extra runtime DLL to install alongside the proxy.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
find_package(Threads REQUIRED)
foreach(unit frame_stats memory_sample deferred_log)
  add_executable(test_${unit} tests/test_${unit}.cpp)
  target_include_directories(test_${unit} PRIVATE src/proxy)
  target_link_libraries(test_${unit} PRIVATE Threads::Threads)
  if(NOT unit STREQUAL "deferred_log")
    target_sources(test_${unit} PRIVATE src/proxy/${unit}.cpp)
  endif()
  add_test(NAME ${unit} COMMAND test_${unit})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME analyze COMMAND "${Python3_EXECUTABLE}" -I -B -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -p test_analyze.py)
endif()

if(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "Steam EaW/FoC requires x64: use -A x64 or an x86_64 MinGW toolchain.")
  endif()
  add_library(d3d9_proxy SHARED
    src/proxy/exports.cpp
    src/proxy/device_hooks.cpp
    src/proxy/exit_hook.cpp
    src/proxy/gpu_timer.cpp
    src/proxy/profiler.cpp
    src/proxy/frame_stats.cpp
    src/proxy/memory_sample.cpp
    src/proxy/memory_sampler.cpp
    src/proxy/d3d9.def)
  set_target_properties(d3d9_proxy PROPERTIES OUTPUT_NAME d3d9 PREFIX "")
  target_compile_definitions(d3d9_proxy PRIVATE CINTERFACE COBJMACROS
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(d3d9_proxy PRIVATE user32)
  if(MSVC)
    target_compile_options(d3d9_proxy PRIVATE /W4)
  else()
    target_compile_options(d3d9_proxy PRIVATE -Wall -Wextra)
    target_link_options(d3d9_proxy PRIVATE -static -Wl,--enable-stdcall-fixup)
  endif()

  add_executable(smoke_d3d9 tests/smoke_d3d9.cpp)
  target_compile_definitions(smoke_d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(smoke_d3d9 PRIVATE user32 gdi32)
  add_dependencies(smoke_d3d9 d3d9_proxy)
  add_test(NAME smoke_d3d9 COMMAND smoke_d3d9)
  set_tests_properties(smoke_d3d9 PROPERTIES TIMEOUT 120
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:smoke_d3d9>")

  get_property(all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target ${all_targets})
    get_target_property(kind ${target} TYPE)
    if(kind STREQUAL "EXECUTABLE")
      target_sources(${target} PRIVATE tests/no_crash_dialogs.cpp)
      if(NOT MSVC)
        target_link_options(${target} PRIVATE -static)
      endif()
    endif()
  endforeach()
endif()
