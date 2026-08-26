add_library(motif_game_navigator STATIC
    game_navigator.cpp
)
target_link_libraries(motif_game_navigator PUBLIC
    motif_chess
    motif_db
)
target_include_directories(motif_game_navigator PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/source>"
)
target_compile_features(motif_game_navigator PUBLIC cxx_std_23)
