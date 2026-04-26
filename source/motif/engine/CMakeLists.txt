find_package(tl-expected REQUIRED)

add_library(motif_engine STATIC motif_engine.cpp)

target_include_directories(
    motif_engine PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_engine PUBLIC tl::expected)

target_compile_features(motif_engine PUBLIC cxx_std_23)
