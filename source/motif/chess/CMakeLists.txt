find_package(chesslib REQUIRED)
find_package(tl-expected REQUIRED)

add_library(motif_chess STATIC
    chess.cpp
)

target_include_directories(
    motif_chess PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(
    motif_chess PUBLIC
        chesslib::chesslib
        tl::expected
)

target_compile_features(motif_chess PUBLIC cxx_std_23)
