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
    source/motif_db/position_postings_test.cpp
    source/motif_db/opening_tree_index_test.cpp
)
target_link_libraries(motif_db_test PRIVATE motif_db Catch2::Catch2WithMain)
target_compile_features(motif_db_test PRIVATE cxx_std_23)
catch_discover_tests(motif_db_test)

add_executable(motif_prefix_postings_test
    source/motif_db/position_prefix_postings_test.cpp
)
target_link_libraries(motif_prefix_postings_test PRIVATE motif_db Catch2::Catch2WithMain)
target_compile_features(motif_prefix_postings_test PRIVATE cxx_std_23)
catch_discover_tests(motif_prefix_postings_test)

add_executable(motif_import_test
    source/motif_import/placeholder_test.cpp
    source/motif_import/logger_test.cpp
    source/motif_import/pgn_reader_test.cpp
    source/motif_import/import_worker_test.cpp
    source/motif_import/checkpoint_test.cpp
    source/motif_import/import_pipeline_test.cpp
    source/motif_import/replay_throughput_test.cpp
    source/motif_import/opening_tree_index_perf_test.cpp
)
target_link_libraries(motif_import_test PRIVATE motif_import motif_search Catch2::Catch2WithMain)
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
target_include_directories(motif_engine_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
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


if(motif_ENABLE_SLINT_SPIKE OR motif_ENABLE_SLINT_APP)
    add_executable(motif_navigator_test
        source/motif_navigator/game_navigator_test.cpp
    )
    target_link_libraries(motif_navigator_test PRIVATE motif_game_navigator Catch2::Catch2WithMain)
    target_compile_features(motif_navigator_test PRIVATE cxx_std_23)
    catch_discover_tests(motif_navigator_test)
endif()

if(motif_ENABLE_SLINT_SPIKE)
    add_executable(motif_slint_spike_presenter_test
        source/motif_slint_spike/game_browser_presenter_test.cpp
    )
    target_link_libraries(motif_slint_spike_presenter_test PRIVATE
        motif_slint_spike_presenter
        Catch2::Catch2WithMain
    )
    target_compile_features(motif_slint_spike_presenter_test PRIVATE cxx_std_23)
    catch_discover_tests(motif_slint_spike_presenter_test)
endif()

if(motif_ENABLE_SLINT_APP)
    add_executable(motif_slint_app_service_test
        source/motif_slint_app/analysis_host_test.cpp
        source/motif_slint_app/analysis_presenter_test.cpp
        source/motif_slint_app/analysis_service_test.cpp
        source/motif_slint_app/board_presenter_test.cpp
        source/motif_slint_app/import_service_test.cpp
        source/motif_slint_app/pgn_launch_queue_test.cpp
        source/motif_slint_app/board_host_test.cpp
        source/motif_slint_app/browse_buttons_test.cpp
        source/motif_slint_app/browser_host_test.cpp
        source/motif_slint_app/database_host_test.cpp
        source/motif_slint_app/replace_gate_test.cpp
        source/motif_slint_app/replace_controller_test.cpp
        source/motif_slint_app/activity_gate_test.cpp
        source/motif_slint_app/workspace_service_test.cpp
        source/motif_slint_app/game_browser_presenter_test.cpp
        source/motif_slint_app/explorer_table_test.cpp
        source/motif_slint_app/file_picker_test.cpp
        source/motif_slint_app/picker_host_test.cpp
        source/motif_slint_app/search_presenter_test.cpp
        source/motif_slint_app/search_host_test.cpp
        source/motif_slint_app/workspace_host_test.cpp
        source/motif_slint_app/layout_snapshot_test.cpp
    )
    target_link_libraries(motif_slint_app_service_test PRIVATE
        motif_slint_app_service
        Slint::Slint
        Catch2::Catch2WithMain
    )
    target_compile_features(motif_slint_app_service_test PRIVATE cxx_std_23)
    target_include_directories(motif_slint_app_service_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source
        "$<TARGET_PROPERTY:motif_slint_app,BINARY_DIR>"
    )
    target_sources(motif_slint_app_service_test PRIVATE
        "${CMAKE_BINARY_DIR}/source/motif/slint_app/slint_generated_workspace_1.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/analysis_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/board_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/browser_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/database_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/event_loop.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/picker_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/search_host.cpp"
        "${CMAKE_SOURCE_DIR}/source/motif/slint_app/workspace_host.cpp"
    )
    set_source_files_properties(
        "${CMAKE_BINARY_DIR}/source/motif/slint_app/slint_generated_workspace_1.cpp"
        PROPERTIES SKIP_LINTING ON
    )
    # The layout test includes Slint-generated workspace.h. Exclude only build-tree
    # headers from clang-tidy; handwritten test sources remain fully linted.
    if(CMAKE_CXX_CLANG_TIDY)
        set_property(TARGET motif_slint_app_service_test PROPERTY CXX_CLANG_TIDY
            "${CMAKE_CXX_CLANG_TIDY};--exclude-header-filter=^.*/build/.*"
        )
    endif()
    if(CMAKE_CXX_CPPCHECK)
        set_property(TARGET motif_slint_app_service_test PROPERTY CXX_CPPCHECK
            "${CMAKE_CXX_CPPCHECK};--suppress=unknownMacro:*workspace.h"
        )
    endif()
    add_dependencies(motif_slint_app_service_test motif_slint_app)
    catch_discover_tests(motif_slint_app_service_test PROPERTIES RUN_SERIAL TRUE)

    # The real NFD adapter's failure path. A dialog needs a desktop session, so
    # this runs on Linux only (a Mac would show a panel) and in its own
    # executable: it changes the process environment and links NFD, which the
    # service test binary never does.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(nfd CONFIG REQUIRED)
        add_executable(motif_nfd_picker_test
            source/motif_slint_app/nfd_file_picker_test.cpp
            "${CMAKE_SOURCE_DIR}/source/motif/slint_app/nfd_file_picker.cpp"
        )
        target_link_libraries(motif_nfd_picker_test PRIVATE
            motif_slint_app_service
            nfd::nfd
            Catch2::Catch2WithMain
        )
        target_compile_features(motif_nfd_picker_test PRIVATE cxx_std_23)
        target_include_directories(motif_nfd_picker_test PRIVATE "${CMAKE_SOURCE_DIR}/source")
        catch_discover_tests(motif_nfd_picker_test PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

# ---- End-of-file commands ----

add_folders(Test)
