cmake_minimum_required(VERSION 3.25)

include(cmake/prelude.cmake)

project(
    motif
    VERSION 0.1.0
    DESCRIPTION "Motif Chess - A chess database tool"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES CXX
)

option(
    motif_ENABLE_X86_64_V3
    "Enable -march=x86-64-v3 for x86_64 builds"
    OFF
)

option(
    motif_ENABLE_APP
    "Build the motif_app Qt module"
    OFF
)

if(motif_ENABLE_X86_64_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-march=x86-64-v3>)
endif()

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# ---- Dependencies ----

find_package(fmt REQUIRED)

# ---- Modules ----

add_subdirectory(source/motif/db)
add_subdirectory(source/motif/chess)
add_subdirectory(source/motif/import)
add_subdirectory(source/motif/search)
add_subdirectory(source/motif/engine)
add_subdirectory(source/motif/http)
if(motif_ENABLE_APP)
  add_subdirectory(source/motif/app)
endif()

# ---- Install rules ----

if(NOT CMAKE_SKIP_INSTALL_RULES)
  include(cmake/install-rules.cmake)
endif()

# ---- Developer mode ----

if(NOT motif_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(
      AUTHOR_WARNING
      "Developer mode is intended for developers of motif"
  )
endif()

include(cmake/dev-mode.cmake)
