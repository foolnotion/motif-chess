find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(glaze REQUIRED)
find_package(tl-expected REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(DuckDB REQUIRED)

add_library(motif_db STATIC
    game_store.cpp
    game_writer.cpp
    schema.cpp
    manifest.cpp
    database_manager.cpp
    position_store.cpp
)

target_link_libraries(
    motif_db PUBLIC
        motif_chess
        fmt::fmt
        glaze::glaze
        tl::expected
        SQLite::SQLite3
        duckdb
        spdlog::spdlog
)

target_include_directories(
    motif_db PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

# DuckDB's amalgamated header has internal forward-declaration ordering issues
# that the clang frontend (used by clang-tidy) trips over. SYSTEM suppresses
# compiler diagnostics from that header so clang-tidy can process our files.
target_include_directories(motif_db SYSTEM PUBLIC ${DuckDB_INCLUDE_DIRS})

target_compile_features(motif_db PUBLIC cxx_std_23)
