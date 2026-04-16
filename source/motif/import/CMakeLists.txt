add_library(motif_import STATIC motif_import.cpp)

target_include_directories(
    motif_import PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(motif_import PUBLIC cxx_std_20)
