cmake_minimum_required(VERSION 3.19)

project(san_ab_bench LANGUAGES CXX)

find_package(pgnlib REQUIRED)
find_package(spdlog REQUIRED)
find_package(tl-expected REQUIRED)

set(CHESS_LIBRARY_INCLUDE_DIR "" CACHE PATH "Path to Disservin chess-library include directory")
set(CHESSLIB_SOURCE_DIR "" CACHE PATH "Optional path to local chesslib source tree")

if(NOT EXISTS "${CHESS_LIBRARY_INCLUDE_DIR}/chess.hpp")
  message(FATAL_ERROR "Set CHESS_LIBRARY_INCLUDE_DIR to the directory containing chess.hpp")
endif()

if(EXISTS "${CHESSLIB_SOURCE_DIR}/CMakeLists.txt")
  add_subdirectory("${CHESSLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/local-chesslib" EXCLUDE_FROM_ALL)
else()
  find_package(chesslib REQUIRED)
endif()

add_executable(san_ab_bench
  main.cpp
  ../../source/motif/import/pgn_reader.cpp
)

target_include_directories(san_ab_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/../..
  ${PROJECT_SOURCE_DIR}/../../source
  ${CHESS_LIBRARY_INCLUDE_DIR}
)

target_link_libraries(san_ab_bench PRIVATE
  chesslib::chesslib
  pgnlib::pgnlib
  spdlog::spdlog
  tl::expected
)

target_compile_features(san_ab_bench PRIVATE cxx_std_23)
