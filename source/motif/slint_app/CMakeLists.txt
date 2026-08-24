if(NOT TARGET Slint::slint-compiler)
    find_package(Slint CONFIG REQUIRED)
endif()
find_package(tl-expected REQUIRED)

add_library(motif_slint_app_service STATIC
    import_service.cpp
    workspace_service.cpp
)
target_link_libraries(motif_slint_app_service PUBLIC
    motif_db
    motif_import
    tl::expected
)
target_include_directories(motif_slint_app_service PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)
target_compile_features(motif_slint_app_service PUBLIC cxx_std_23)

add_executable(motif_slint_app main.cpp)
slint_target_sources(motif_slint_app ui/workspace.slint)
# Slint generates framework C++ that does not follow Motif's clang-tidy rules.
# Keep linting enabled for main.cpp and skip only the generated translation unit.
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_workspace_1.cpp"
    PROPERTIES SKIP_LINTING ON
)
if(CMAKE_CXX_CLANG_TIDY)
    set_property(TARGET motif_slint_app PROPERTY CXX_CLANG_TIDY
        "${CMAKE_CXX_CLANG_TIDY};--exclude-header-filter=^.*/build/.*"
    )
endif()
if(CMAKE_CXX_CPPCHECK)
    set_property(TARGET motif_slint_app PROPERTY CXX_CPPCHECK
        "${CMAKE_CXX_CPPCHECK};--suppress=unknownMacro:*workspace.h"
    )
endif()
target_link_libraries(motif_slint_app PRIVATE
    motif_slint_app_service
    Slint::Slint
)
target_compile_features(motif_slint_app PRIVATE cxx_std_23)

if(NOT CMAKE_SKIP_INSTALL_RULES)
    include(GNUInstallDirs)
    install(TARGETS motif_slint_app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
