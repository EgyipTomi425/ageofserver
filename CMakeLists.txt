cmake_minimum_required(VERSION 3.28)
project(ageofserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(proxygen_DIR "/opt/proxygen/proxygen/_build/lib/cmake/proxygen" CACHE PATH "")
set(CMAKE_PREFIX_PATH "/opt/proxygen/proxygen/_build/deps" CACHE PATH "")
set(CMAKE_MODULE_PATH "/opt/proxygen/proxygen/_build/deps/mvfst/cmake" CACHE PATH "")

find_package(proxygen CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
find_package(Threads REQUIRED)

add_executable(ageofserver
  src/main.cpp
  src/repository.cpp
  src/api_handler.cpp
  src/server.cpp
)
target_sources(ageofserver
  PUBLIC
    FILE_SET CXX_MODULES FILES
      src/repository.cppm
      src/api_handler.cppm
      src/server.cppm
)
target_link_libraries(ageofserver PRIVATE
  proxygen::proxygen
  proxygen::proxygenhttpserver
  wangle::wangle
  Folly::folly
  PkgConfig::PQXX
  Threads::Threads
)

target_compile_options(ageofserver PRIVATE -Wall -Wextra -Wpedantic)
