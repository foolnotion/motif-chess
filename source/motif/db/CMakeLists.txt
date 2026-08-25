find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(glaze REQUIRED)
find_package(tl-expected REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(xxHash REQUIRED)

add_library(motif_db STATIC
    game_store.cpp
    game_writer.cpp
    schema.cpp
    manifest.cpp
    database_manager.cpp
    position_postings.cpp
    position_prefix_postings.cpp
    opening_tree_index.cpp
)

target_link_libraries(
    motif_db PUBLIC
        motif_chess
        fmt::fmt
        glaze::glaze
        tl::expected
        SQLite::SQLite3
        spdlog::spdlog
        xxHash::xxhash
)

target_include_directories(
    motif_db PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(motif_db PUBLIC cxx_std_23)
