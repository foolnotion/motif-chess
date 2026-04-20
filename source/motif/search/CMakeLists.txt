find_package(spdlog REQUIRED)

add_library(motif_search STATIC motif_search.cpp)

target_include_directories(
    motif_search PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_search PUBLIC spdlog::spdlog)

target_compile_features(motif_search PUBLIC cxx_std_23)
