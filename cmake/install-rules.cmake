install(
    TARGETS motif_exe
    RUNTIME COMPONENT motif_Runtime
)

if(PROJECT_IS_TOP_LEVEL)
  include(CPack)
endif()
