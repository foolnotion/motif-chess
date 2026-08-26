find_package(tl-expected REQUIRED)

add_library(motif_slint_spike_presenter STATIC
    game_browser_presenter.cpp
)
target_link_libraries(motif_slint_spike_presenter PUBLIC
    motif_chess
    motif_db
    motif_game_navigator
    tl::expected
)
target_include_directories(motif_slint_spike_presenter PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)
target_compile_features(motif_slint_spike_presenter PUBLIC cxx_std_23)

add_executable(motif_slint_spike main.cpp)
slint_target_sources(motif_slint_spike ui/game_browser.slint)
# Slint generates framework C++ that does not follow Motif's clang-tidy rules.
# Keep linting enabled for main.cpp and skip only the generated translation
# unit. NOTE: this hardcodes Slint's current codegen filename convention
# (<slint-file-stem>_<N>.cpp); if Slint's codegen naming changes, this
# silently stops matching and clang-tidy will start linting generated code.
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/slint_generated_game_browser_1.cpp"
    PROPERTIES SKIP_LINTING ON
)
if(CMAKE_CXX_CLANG_TIDY)
    set_property(TARGET motif_slint_spike PROPERTY CXX_CLANG_TIDY
        "${CMAKE_CXX_CLANG_TIDY};--exclude-header-filter=^.*/build/.*"
    )
endif()
if(CMAKE_CXX_CPPCHECK)
    set_property(TARGET motif_slint_spike PROPERTY CXX_CPPCHECK
        "${CMAKE_CXX_CPPCHECK};--suppress=unknownMacro:*game_browser.h"
    )
endif()
target_link_libraries(motif_slint_spike PRIVATE
    motif_slint_spike_presenter
    Slint::Slint
    fmt::fmt
)
target_compile_features(motif_slint_spike PRIVATE cxx_std_23)

if(NOT CMAKE_SKIP_INSTALL_RULES)
    include(GNUInstallDirs)
    install(TARGETS motif_slint_spike RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
