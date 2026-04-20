cmake_minimum_required(VERSION 3.19)

include(cmake/prelude.cmake)

project(
    motif
    VERSION 0.1.0
    DESCRIPTION "Motif Chess - A chess database tool"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES CXX
)

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)

# ---- Dependencies ----

find_package(fmt REQUIRED)

# ---- Modules ----

add_subdirectory(source/motif/db)
add_subdirectory(source/motif/import)
add_subdirectory(source/motif/search)
add_subdirectory(source/motif/engine)

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
