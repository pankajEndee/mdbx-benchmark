cmake_minimum_required(VERSION 3.20)
project(mdbx_bench CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

include(FetchContent)
# Pinned to a specific release tag for reproducibility. To upgrade MDBX, bump
# the GIT_TAG below. Upstream development is on gitflic.ru; the GitHub repo is
# a mirror that still serves tagged releases.
FetchContent_Declare(
  libmdbx
  GIT_REPOSITORY https://github.com/erthink/libmdbx.git
  GIT_TAG        v0.13.12
)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
set(MDBX_INSTALL_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libmdbx)

add_executable(mdbx_bench
  src/main.cpp
  src/env.cpp
  src/stats.cpp
  src/csv.cpp
  src/loader.cpp
)
target_include_directories(mdbx_bench PRIVATE src)
target_link_libraries(mdbx_bench PRIVATE mdbx-static)
find_package(Threads REQUIRED)
target_link_libraries(mdbx_bench PRIVATE Threads::Threads)
