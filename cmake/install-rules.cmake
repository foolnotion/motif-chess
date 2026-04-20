include(GNUInstallDirs)

install(
  DIRECTORY source/motif/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/motif"
  COMPONENT motif_Development
  FILES_MATCHING
  PATTERN "*.hpp"
)

install(
  TARGETS
    motif_db
    motif_import
    motif_search
    motif_engine
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT motif_Development
)

if(PROJECT_IS_TOP_LEVEL)
  include(CPack)
endif()
