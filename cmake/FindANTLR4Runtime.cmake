# Find the ANTLR4 C++ runtime
#
# This will define:
#  ANTLR4Runtime_FOUND
#  ANTLR4_RUNTIME_INCLUDE_DIR
#  ANTLR4_RUNTIME_LIBRARY
#  ANTLR4Runtime::antlr4_runtime (imported target)

if(ANTLR4_RUNTIME_INCLUDE_DIR AND
   NOT EXISTS "${ANTLR4_RUNTIME_INCLUDE_DIR}/antlr4-runtime.h")
  unset(ANTLR4_RUNTIME_INCLUDE_DIR CACHE)
endif()

find_path(ANTLR4_RUNTIME_INCLUDE_DIR
  NAMES antlr4-runtime.h
  PATH_SUFFIXES antlr4-runtime
  HINTS /usr/local/include /usr/include
)

find_library(ANTLR4_RUNTIME_LIBRARY
  NAMES antlr4-runtime
  HINTS /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ANTLR4Runtime
  REQUIRED_VARS ANTLR4_RUNTIME_INCLUDE_DIR ANTLR4_RUNTIME_LIBRARY
)

if(ANTLR4Runtime_FOUND AND NOT TARGET ANTLR4Runtime::antlr4_runtime)
  add_library(ANTLR4Runtime::antlr4_runtime UNKNOWN IMPORTED)
  set_target_properties(ANTLR4Runtime::antlr4_runtime PROPERTIES
    IMPORTED_LOCATION "${ANTLR4_RUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ANTLR4_RUNTIME_INCLUDE_DIR}"
  )
endif()
