add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_executable(mcpwa_tests
  test_main.cpp
  test_jsonrpc.cpp
  test_framing.cpp
  test_url.cpp
  test_cli.cpp
  test_bridge_fake_transport.cpp)

target_link_libraries(mcpwa_tests PRIVATE mcpwinauth::bridge doctest)
# cli.hpp is internal, so the tests reach into src/ for it.
target_include_directories(mcpwa_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(MSVC)
  # The legacy preprocessor mangles raw string literals passed to macros,
  # and every CHECK() here does exactly that.
  target_compile_options(mcpwa_tests PRIVATE /W4 /permissive- /utf-8 /bigobj /Zc:preprocessor)
endif()

add_test(NAME unit COMMAND mcpwa_tests)
