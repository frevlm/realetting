cmake_minimum_required(VERSION 3.25)

project(realetting VERSION 0.1.0 LANGUAGES CXX)

add_library(realetting INTERFACE)
add_library(realetting::realetting ALIAS realetting)
target_include_directories(realetting INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(realetting INTERFACE cxx_std_17)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(test_realetting tests/test_realetting.cpp)
  target_link_libraries(test_realetting PRIVATE realetting)
  target_compile_options(test_realetting PRIVATE -Wall -Wextra -Werror)
  add_test(NAME realetting COMMAND test_realetting)
endif()
