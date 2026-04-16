find_package(chesslib CONFIG REQUIRED)
find_package(tl-expected REQUIRED)

add_library(motif_db STATIC motif_db.cpp)

target_link_libraries(
    motif_db PUBLIC
        chesslib::chesslib
    tl::expected
)

target_include_directories(
    motif_db PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(motif_db PUBLIC cxx_std_20)
