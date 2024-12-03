cmake_minimum_required(VERSION 3.12)
project(splinterfs)

# Specify C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find FUSE using pkg-config
pkg_check_modules(FUSE REQUIRED fuse)

# Create executable
add_executable(
    splinterfs
      src/splinterfs.cpp
      src/logger.cpp
)

# Link against FUSE and add its include directories
target_link_libraries(splinterfs PRIVATE ${FUSE_LIBRARIES})
target_include_directories(splinterfs PRIVATE ${FUSE_INCLUDE_DIRS})
target_compile_options(splinterfs PRIVATE ${FUSE_CFLAGS})

# Set specific compiler if needed
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_COMPILER "g++-13")
endif()