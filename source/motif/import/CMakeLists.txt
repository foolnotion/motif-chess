find_package(spdlog REQUIRED)
find_package(tl-expected REQUIRED)
find_package(pgnlib REQUIRED)
find_package(Taskflow REQUIRED)

add_library(motif_import STATIC
    motif_import.cpp logger.cpp pgn_reader.cpp import_worker.cpp
    checkpoint.cpp import_pipeline.cpp)

target_include_directories(
    motif_import PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_import PUBLIC
    spdlog::spdlog pgnlib::pgnlib tl::expected motif_db
    Taskflow::Taskflow)

target_compile_features(motif_import PUBLIC cxx_std_23)
