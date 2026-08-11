find_package(PkgConfig)

PKG_CHECK_MODULES(PC_GR_NUT gnuradio-nut)

FIND_PATH(
    GR_NUT_INCLUDE_DIRS
    NAMES gnuradio/nut/api.h
    HINTS $ENV{NUT_DIR}/include
        ${PC_NUT_INCLUDEDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/include
          /usr/local/include
          /usr/include
)

FIND_LIBRARY(
    GR_NUT_LIBRARIES
    NAMES gnuradio-nut
    HINTS $ENV{NUT_DIR}/lib
        ${PC_NUT_LIBDIR}
    PATHS ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
          /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          )

include("${CMAKE_CURRENT_LIST_DIR}/gnuradio-nutTarget.cmake")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(GR_NUT DEFAULT_MSG GR_NUT_LIBRARIES GR_NUT_INCLUDE_DIRS)
MARK_AS_ADVANCED(GR_NUT_LIBRARIES GR_NUT_INCLUDE_DIRS)
