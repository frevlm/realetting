cmake_minimum_required(VERSION 3.25)

project(realetting VERSION 0.1.0 LANGUAGES CXX)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(realetting INTERFACE)
add_library(realetting::realetting ALIAS realetting)
target_include_directories(realetting INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(realetting INTERFACE cxx_std_23)
target_link_libraries(realetting INTERFACE nlohmann_json::nlohmann_json)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(test_realetting tests/test_realetting.cpp)
  target_link_libraries(test_realetting PRIVATE realetting)
  target_compile_options(test_realetting PRIVATE -Wall -Wextra -Werror)
  add_test(NAME realetting COMMAND test_realetting)
endif()
