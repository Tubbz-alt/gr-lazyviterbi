INCLUDE(FindPkgConfig)
PKG_CHECK_MODULES(PC_LAZYVITERBI lazyviterbi)

FIND_PATH(
    LAZYVITERBI_INCLUDE_DIRS
    NAMES lazyviterbi/api.h
    HINTS $ENV{LAZYVITERBI_DIR}/include
        ${PC_LAZYVITERBI_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    LAZYVITERBI_LIBRARIES
    NAMES gnuradio-lazyviterbi
    HINTS $ENV{LAZYVITERBI_DIR}/lib
        ${PC_LAZYVITERBI_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          )

include("${CMAKE_CURRENT_LIST_DIR}/lazyviterbiTarget.cmake")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LAZYVITERBI DEFAULT_MSG LAZYVITERBI_LIBRARIES LAZYVITERBI_INCLUDE_DIRS)
MARK_AS_ADVANCED(LAZYVITERBI_LIBRARIES LAZYVITERBI_INCLUDE_DIRS)
