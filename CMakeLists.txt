cmake_minimum_required(VERSION 3.13)
project(p05-gpgpu-parallelizer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Clang REQUIRED CONFIG)
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Found Clang: ${CLANG_INSTALL_PREFIX}")

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(p05tool
  src/driver/main.cpp
  src/analysis/LoopAnalysis.cpp
  src/analysis/CallResolver.cpp
)

# Project headers are included as "analysis/LoopInfo.h" etc.
target_include_directories(p05tool PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(p05tool
  PRIVATE
  clangTooling
  clangBasic
  clangASTMatchers
  clangRewrite
  clangFrontend
  clangAnalysis
)
