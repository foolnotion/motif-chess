find_package(httplib REQUIRED)
find_package(glaze REQUIRED)
find_package(tl-expected REQUIRED)

add_library(motif_http STATIC
    server.cpp
)

target_include_directories(
    motif_http PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_link_libraries(motif_http
    PUBLIC
        httplib::httplib
        glaze::glaze
        tl::expected
        motif_db
    PRIVATE
        motif_search
        motif_import
        motif_engine
        motif_chess
)

target_compile_features(motif_http PUBLIC cxx_std_23)

add_executable(motif_http_server main.cpp)
target_link_libraries(motif_http_server PRIVATE motif_http)
target_compile_features(motif_http_server PRIVATE cxx_std_23)
