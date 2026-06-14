cmake_minimum_required(VERSION 3.16)
project(binance_lob_capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wshadow)
option(ENABLE_WERROR "Treat compiler warnings as errors" OFF)
if(ENABLE_WERROR)
  add_compile_options(-Werror)
endif()

# Live capture needs Boost.Beast + OpenSSL. Set ENABLE_LIVE=AUTO to use the
# real client when those dependencies are available, ON to require them, or OFF
# for a dependency-light core/replay/sanitizer build.
set(ENABLE_LIVE "AUTO" CACHE STRING "Build live Binance WebSocket capture: AUTO, ON, or OFF")
set_property(CACHE ENABLE_LIVE PROPERTY STRINGS AUTO ON OFF)
set(BLOB_ENABLE_LIVE OFF)
if(ENABLE_LIVE STREQUAL "AUTO")
  find_package(Boost QUIET COMPONENTS system)
  find_package(OpenSSL QUIET)
  if(Boost_FOUND AND OpenSSL_FOUND)
    set(BLOB_ENABLE_LIVE ON)
    message(STATUS "Boost + OpenSSL found -> auto-enabling ENABLE_LIVE")
  else()
    message(STATUS "Boost/OpenSSL not found -> building stub client (set -DENABLE_LIVE=ON to force)")
  endif()
elseif(ENABLE_LIVE)
  set(BLOB_ENABLE_LIVE ON)
endif()
option(ENABLE_SIMDJSON "Use simdjson on the JSON parse hot path (needs libsimdjson-dev)" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be enabled together")
endif()

set(SANITIZER_FLAGS "")
if(ENABLE_ASAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(ENABLE_UBSAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
endif()
if(ENABLE_TSAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)

add_library(lob_core
  src/order_book.cpp
  src/csv_writer.cpp
  src/config.cpp
  src/json_min.cpp
  src/book_session.cpp
  src/replay.cpp
)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_core PUBLIC Threads::Threads)
if(SANITIZER_FLAGS)
  target_compile_options(lob_core PUBLIC ${SANITIZER_FLAGS})
  target_link_options(lob_core PUBLIC ${SANITIZER_FLAGS})
endif()

if(ENABLE_SIMDJSON)
  find_package(simdjson CONFIG QUIET)
  if(TARGET simdjson::simdjson)
    target_link_libraries(lob_core PUBLIC simdjson::simdjson)
  else()
    find_path(SIMDJSON_INCLUDE_DIR simdjson.h)
    find_library(SIMDJSON_LIBRARY NAMES simdjson)
    if(NOT SIMDJSON_INCLUDE_DIR OR NOT SIMDJSON_LIBRARY)
      message(FATAL_ERROR
        "ENABLE_SIMDJSON=ON requires simdjson headers and library "
        "(Ubuntu/Debian: apt-get install libsimdjson-dev)")
    endif()
    target_include_directories(lob_core PUBLIC ${SIMDJSON_INCLUDE_DIR})
    target_link_libraries(lob_core PUBLIC ${SIMDJSON_LIBRARY})
  endif()
  target_compile_definitions(lob_core PUBLIC USE_SIMDJSON=1)
endif()

add_executable(binance_capture src/main.cpp)
target_link_libraries(binance_capture PRIVATE lob_core)

if(BLOB_ENABLE_LIVE)
  find_package(Threads REQUIRED)
  find_package(Boost REQUIRED COMPONENTS system)
  find_package(OpenSSL REQUIRED)
  target_sources(binance_capture PRIVATE src/ws_client_beast.cpp src/snapshot_beast.cpp)
  target_compile_definitions(binance_capture PRIVATE ENABLE_LIVE=1)
  target_link_libraries(binance_capture PRIVATE
    Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
else()
  target_sources(binance_capture PRIVATE src/ws_client_stub.cpp src/snapshot_stub.cpp)
endif()

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE lob_core)
add_test(NAME unit_tests COMMAND unit_tests)
