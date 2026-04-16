# Parent project does not export its library targets, so this CMakeLists.txt
# implicitly depends on being added from it, i.e. testing is done only from
# the build tree and is not feasible from an install location

project(motifTests LANGUAGES CXX)

# ---- Dependencies ----

find_package(Catch2 REQUIRED)
include(Catch)

# ---- Tests ----

add_executable(motif_db_test source/motif_db/move_codec_test.cpp)
target_link_libraries(motif_db_test PRIVATE motif_db Catch2::Catch2WithMain)
target_compile_features(motif_db_test PRIVATE cxx_std_20)
catch_discover_tests(motif_db_test)

add_executable(motif_import_test source/motif_import/placeholder_test.cpp)
target_link_libraries(motif_import_test PRIVATE motif_import Catch2::Catch2WithMain)
target_compile_features(motif_import_test PRIVATE cxx_std_20)
catch_discover_tests(motif_import_test)

add_executable(motif_search_test source/motif_search/placeholder_test.cpp)
target_link_libraries(motif_search_test PRIVATE motif_search Catch2::Catch2WithMain)
target_compile_features(motif_search_test PRIVATE cxx_std_20)
catch_discover_tests(motif_search_test)

# ---- End-of-file commands ----

add_folders(Test)
