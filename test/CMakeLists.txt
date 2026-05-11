# Parent project does not export its library targets, so this CMakeLists.txt
# implicitly depends on being added from it, i.e. testing is done only from
# the build tree and is not feasible from an install location

project(motifTests LANGUAGES CXX)

# ---- Dependencies ----

find_package(Catch2 REQUIRED)
include(Catch)

# ---- Tests ----

add_executable(motif_db_test
    source/motif_db/game_store_test.cpp
    source/motif_db/game_writer_test.cpp
    source/motif_db/schema_test.cpp
    source/motif_db/manifest_test.cpp
    source/motif_db/database_manager_test.cpp
    source/motif_db/position_store_test.cpp
)
target_link_libraries(motif_db_test PRIVATE motif_db Catch2::Catch2WithMain)
target_compile_features(motif_db_test PRIVATE cxx_std_23)
catch_discover_tests(motif_db_test)

add_executable(motif_import_test
    source/motif_import/placeholder_test.cpp
    source/motif_import/logger_test.cpp
    source/motif_import/pgn_reader_test.cpp
    source/motif_import/import_worker_test.cpp
    source/motif_import/checkpoint_test.cpp
    source/motif_import/import_pipeline_test.cpp
)
target_link_libraries(motif_import_test PRIVATE motif_import Catch2::Catch2WithMain)
target_compile_features(motif_import_test PRIVATE cxx_std_23)
target_include_directories(motif_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(
    motif_import_test PRIVATE MOTIF_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
catch_discover_tests(motif_import_test)

add_executable(motif_search_test
    source/motif_search/position_search_test.cpp
    source/motif_search/opening_stats_test.cpp
    source/motif_search/opening_tree_test.cpp
)
target_link_libraries(motif_search_test PRIVATE motif_search motif_import Catch2::Catch2WithMain)
target_compile_features(motif_search_test PRIVATE cxx_std_23)
target_include_directories(motif_search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(
    motif_search_test PRIVATE MOTIF_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
catch_discover_tests(motif_search_test)

add_executable(motif_engine_test
    source/motif_engine/engine_manager_test.cpp
)
target_link_libraries(motif_engine_test PRIVATE motif_engine Catch2::Catch2WithMain)
target_compile_features(motif_engine_test PRIVATE cxx_std_23)
catch_discover_tests(motif_engine_test)

add_executable(motif_http_test
    source/motif_http/http_server_test.cpp
)
target_link_libraries(motif_http_test PRIVATE motif_http motif_import Catch2::Catch2WithMain)
target_compile_features(motif_http_test PRIVATE cxx_std_23)
target_include_directories(motif_http_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(
    motif_http_test PRIVATE MOTIF_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
catch_discover_tests(motif_http_test)

add_executable(motif_chess_test
    source/motif_chess/chess_test.cpp
)
target_link_libraries(motif_chess_test PRIVATE motif_chess Catch2::Catch2WithMain)
target_compile_features(motif_chess_test PRIVATE cxx_std_23)
catch_discover_tests(motif_chess_test)

if(motif_ENABLE_APP)
    find_package(Qt6 REQUIRED COMPONENTS Core)

    add_executable(motif_app_test
        ../source/motif/app/game_list_model.cpp
        source/motif_app/app_config_test.cpp
        source/motif_app/database_workspace_test.cpp
        source/motif_app/game_list_model_test.cpp
        source/motif_app/game_navigator_test.cpp
        source/motif_app/pgn_launch_queue_test.cpp
    )
    target_link_libraries(motif_app_test PRIVATE motif_app_lib Qt6::Core Catch2::Catch2WithMain)
    target_compile_features(motif_app_test PRIVATE cxx_std_23)
    set_target_properties(motif_app_test PROPERTIES AUTOMOC ON)
    catch_discover_tests(motif_app_test)
endif()

# ---- End-of-file commands ----

add_folders(Test)
