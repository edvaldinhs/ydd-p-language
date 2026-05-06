cmake_minimum_required(VERSION 3.10)
project(MyLLVMLang)

find_package(LLVM REQUIRED CONFIG)

set(CMAKE_CXX_STANDARD 17)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
link_directories(${LLVM_LIBRARY_DIRS})

add_executable(yde
  src/main.cpp
  src/parser.cpp
  src/lexer.cpp
  src/codeGen.cpp
  src/sema.cpp
)

target_link_libraries(yde LLVM)
