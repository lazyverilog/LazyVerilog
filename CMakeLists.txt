cmake_minimum_required(VERSION 3.20)

project(main)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Optional: avoid GNU extensions (e.g., g++-specific)
find_package(slang)
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE slang::slang)

project(common)
find_package(slang)
add_executable(common slang-playground/common.cpp)
target_link_libraries(common PRIVATE slang::slang)
