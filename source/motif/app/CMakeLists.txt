find_package(fmt REQUIRED)
find_package(glaze REQUIRED)
find_package(tl-expected REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg)
find_package(KDDockWidgets-qt6 REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

# Business logic library — no Qt dependency, fully testable headless
add_library(motif_app_lib STATIC
    app_config.cpp
    database_workspace.cpp
    game_navigator.cpp
    pgn_launch_queue.cpp
)

target_link_libraries(motif_app_lib PUBLIC
    motif_chess
    motif_db
    motif_import
    fmt::fmt
    glaze::glaze
    tl::expected
)

target_include_directories(motif_app_lib PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)

target_compile_features(motif_app_lib PUBLIC cxx_std_23)

# Desktop executable — Qt Quick / QML + KDDockWidgets confined here
# Target is named motif_chess_app to avoid collision with the motif_chess
# static library (source/motif/chess). OUTPUT_NAME keeps the binary name.
qt_add_executable(motif_chess_app
    main.cpp
    board_model.cpp
    game_list_model.cpp
    workspace_controller.cpp
)

qt_add_qml_module(motif_chess_app
    URI com.motif.app
    VERSION 1.0
    QML_FILES
        qml/ChessboardPanel.qml
        qml/GameListPanel.qml
        qml/Main.qml
        qml/VariationTree.qml
        qml/WorkspaceOverlay.qml
    RESOURCES
        qml/pieces/wK.svg
        qml/pieces/wQ.svg
        qml/pieces/wR.svg
        qml/pieces/wB.svg
        qml/pieces/wN.svg
        qml/pieces/wP.svg
        qml/pieces/bK.svg
        qml/pieces/bQ.svg
        qml/pieces/bR.svg
        qml/pieces/bB.svg
        qml/pieces/bN.svg
        qml/pieces/bP.svg
)

set_target_properties(motif_chess_app PROPERTIES
    OUTPUT_NAME      motif_chess
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE    TRUE
)

target_link_libraries(motif_chess_app PRIVATE
    motif_app_lib
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    KDAB::kddockwidgets
    fmt::fmt
)

target_compile_features(motif_chess_app PRIVATE cxx_std_23)

# Qt's QML-to-C++ compiler generates float equality comparisons in property-binding change
# detection. The project-wide -Werror=float-equal flag would fire on that generated code.
# Suppress the error only for this target; our own C++ never does float equality.
target_compile_options(motif_chess_app PRIVATE -Wno-error=float-equal)

# ---- Install rules ----

if(NOT CMAKE_SKIP_INSTALL_RULES)
    include(GNUInstallDirs)
    install(TARGETS motif_chess_app
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        BUNDLE  DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/motif-chess.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
        )
    endif()
endif()
