if(NOT TARGET Slint::slint-compiler)
    find_package(Slint CONFIG REQUIRED)
endif()
find_package(tl-expected REQUIRED)

add_library(motif_slint_spike_presenter STATIC
    game_browser_presenter.cpp
    ../app/game_navigator.cpp
)
target_link_libraries(motif_slint_spike_presenter PUBLIC
    motif_chess
    motif_db
    tl::expected
)
target_include_directories(motif_slint_spike_presenter PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)
target_compile_features(motif_slint_spike_presenter PUBLIC cxx_std_23)

add_executable(motif_slint_spike main.cpp)
slint_target_sources(motif_slint_spike ui/game_browser.slint)
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
