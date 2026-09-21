cmake_minimum_required(VERSION 3.21)
project(mcpwinauth VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "mcp-winauth-bridge is Windows-only (it uses WinHTTP integrated auth).")
endif()

option(MCPWA_BUILD_EXE      "Build the standalone bridge executable" ${PROJECT_IS_TOP_LEVEL})
option(MCPWA_BUILD_TESTS    "Build the unit tests"                   ${PROJECT_IS_TOP_LEVEL})
option(MCPWA_INSTALL        "Generate install/export rules"          ${PROJECT_IS_TOP_LEVEL})
option(MCPWA_STATIC_RUNTIME "Link the static CRT (/MT)"              OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /MT so the released exe runs without the VC++ redist. Never forced on a
# consumer: mismatched CRTs are an LNK2038 at link time.
if(MCPWA_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(mcpwinauth_bridge STATIC
  src/jsonrpc.cpp
  src/framing.cpp
  src/headers.cpp
  src/url.cpp
  src/cli.cpp
  src/sinks.cpp
  src/winhttp_transport.cpp
  src/bridge.cpp
  src/stdio_runner.cpp)
add_library(mcpwinauth::bridge ALIAS mcpwinauth_bridge)

target_include_directories(mcpwinauth_bridge
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(mcpwinauth_bridge
  PUBLIC  NOMINMAX WIN32_LEAN_AND_MEAN
  PRIVATE UNICODE _UNICODE)

target_link_libraries(mcpwinauth_bridge PUBLIC winhttp)

if(MSVC)
  target_compile_options(mcpwinauth_bridge PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
endif()

# EXPORT_NAME is what makes the installed package offer mcpwinauth::bridge,
# the same name the in-build ALIAS provides. Without it consumers would have
# to write mcpwinauth::mcpwinauth_bridge.
set_target_properties(mcpwinauth_bridge PROPERTIES
  OUTPUT_NAME mcpwinauth
  EXPORT_NAME bridge)

if(MCPWA_BUILD_EXE)
  add_executable(mcp-winauth-bridge app/main.cpp)
  target_link_libraries(mcp-winauth-bridge PRIVATE mcpwinauth::bridge)
  target_compile_definitions(mcp-winauth-bridge PRIVATE UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(mcp-winauth-bridge PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
  endif()
endif()

if(MCPWA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MCPWA_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS mcpwinauth_bridge EXPORT mcpwinauthTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(MCPWA_BUILD_EXE)
    install(TARGETS mcp-winauth-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT mcpwinauthTargets
          NAMESPACE mcpwinauth::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpwinauth)

  configure_package_config_file(cmake/mcpwinauthConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/mcpwinauthConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpwinauth)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mcpwinauthConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mcpwinauthConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/mcpwinauthConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcpwinauth)
endif()
