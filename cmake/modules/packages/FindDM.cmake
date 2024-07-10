# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindDAMENG
--------------

Find the DM installation.

IMPORTED Targets
^^^^^^^^^^^^^^^^

Result Variables
^^^^^^^^^^^^^^^^

This module will set the following variables in your project:

``DM_FOUND``
  True if DM is found.
``DM_LIBRARIES``
  the DM libraries needed for linking
``DM_INCLUDE_DIRS``
  the directories of the DM headers
``DM_LIBRARY_DIRS``
  the link directories for DM libraries
``DM_TYPE_INCLUDE_DIR``
  the directories of the DM server headers

#]=======================================================================]
# ----------------------------------------------------------------------------
# You may need to manually set:
#  DM_INCLUDE_DIR  - the path to where the DM include files are.
#  DM_LIBRARY_DIR  - The path to where the DM library files are.
# If FindDM.cmake cannot find the include files or the library files.
#
# ----------------------------------------------------------------------------
# The following variables are set if DM is found:
#  DM_FOUND         - Set to true when DM is found.
#  DM_INCLUDE_DIRS  - Include directories for DM
#  DM_LIBRARY_DIRS  - Link directories for DM libraries
#  DM_LIBRARIES     - The DM libraries.
#
# The ``DM::DM`` imported target is also created.
#
# ----------------------------------------------------------------------------
# If you have installed DM in a non-standard location.
# (Please note that in the following comments, it is assumed that <Your Path>
# points to the root directory of the include directory of DM.)
# Then you have three options.
# 1) After CMake runs, set DM_INCLUDE_DIR to <Your Path>/include and
#    DM_LIBRARY_DIR to wherever the library dm is
# 2) Use CMAKE_INCLUDE_PATH to set a path to <Your Path>/DM. This will allow find_path()
#    to locate DM_INCLUDE_DIR by utilizing the PATH_SUFFIXES option. e.g. In your CMakeLists.txt file
#    set(CMAKE_INCLUDE_PATH ${CMAKE_INCLUDE_PATH} "<Your Path>/include")
# 3) Set an environment variable called ${DM_HOME} that points to the root of where you have
#    installed DM, e.g. <Your Path>.
#
# ----------------------------------------------------------------------------

set(DM_INCLUDE_PATH_DESCRIPTION "top-level directory containing the DM include directories. E.g /usr/local/include/DM or C:/Program Files/DM/include")
set(DM_INCLUDE_DIR_MESSAGE "Set the DM_INCLUDE_DIR cmake cache entry to the ${DM_INCLUDE_PATH_DESCRIPTION}")
set(DM_LIBRARY_PATH_DESCRIPTION "top-level directory containing the DM libraries.")
set(DM_LIBRARY_DIR_MESSAGE "Set the DM_LIBRARY_DIR cmake cache entry to the ${DM_LIBRARY_PATH_DESCRIPTION}")
set(DM_HOME_DIR_MESSAGE "Set the DM_HOME system variable to where DM is found on the machine E.g C:/Program Files/DM")

# Define additional search paths for root directories.
set( DM_HOME_DIRECTORIES
   ENV DM_HOME
   ${DM_HOME}
)

if(WIN32)
  list(APPEND DM_LIBRARY_ADDITIONAL_SEARCH_SUFFIXES
      "drivers/dpi")
  list(APPEND DM_INCLUDE_ADDITIONAL_SEARCH_SUFFIXES
      "DM/include")
endif()
if(UNIX)
  list(APPEND DM_LIBRARY_ADDITIONAL_SEARCH_SUFFIXES
      "DM"
      "drivers/dpi")
  list(APPEND DM_INCLUDE_ADDITIONAL_SEARCH_SUFFIXES
      "DM"
      "DM/"
      "drivers/dpi/include")
endif()

#
# Look for an installation.
#
find_path(DM_INCLUDE_DIR
  NAMES DPI.h
  PATHS
   # Look in other places.
   ${DM_HOME_DIRECTORIES}
  PATH_SUFFIXES
    DM
    include
    ${DM_INCLUDE_ADDITIONAL_SEARCH_SUFFIXES}
  # Help the user find it if we cannot.
  DOC "The ${DM_INCLUDE_DIR_MESSAGE}"
)

# The DM library.
if ( WIN32 )
  set (DM_LIBRARY_TO_FIND dmdpi)
endif()
if (UNIX)
  set (DM_LIBRARY_TO_FIND libdmdpi)
endif()
# Setting some more prefixes for the library
set (DM_LIB_PREFIX "")
if ( WIN32 )
  set (DM_LIB_PREFIX ${DM_LIB_PREFIX} "lib")
  set (DM_LIBRARY_TO_FIND ${DM_LIB_PREFIX}${DM_LIBRARY_TO_FIND})
endif()

function(__DM_find_library _name)
  find_library(${_name}
   NAMES ${ARGN}
   PATHS
     ${DM_HOME_DIRECTORIES}
   PATH_SUFFIXES
     lib
     ${DM_LIBRARY_ADDITIONAL_SEARCH_SUFFIXES}
   # Help the user find it if we cannot.
   DOC "The ${DM_LIBRARY_DIR_MESSAGE}"
  )
endfunction()

# For compatibility with versions prior to this multi-config search, honor
# any DM_LIBRARY that is already specified and skip the search.
if(DM_LIBRARY)
  set(DM_LIBRARIES "${DM_LIBRARY}")
  get_filename_component(DM_LIBRARY_DIR "${DM_LIBRARY}" PATH)
else()
  __DM_find_library(DM_LIBRARY_RELEASE ${DM_LIBRARY_TO_FIND})
  __DM_find_library(DM_LIBRARY_DEBUG ${DM_LIBRARY_TO_FIND})
  include(SelectLibraryConfigurations)
  select_library_configurations(DM)
  mark_as_advanced(DM_LIBRARY_RELEASE DM_LIBRARY_DEBUG)
  if(DM_LIBRARY_RELEASE)
    get_filename_component(DM_LIBRARY_DIR "${DM_LIBRARY_RELEASE}" PATH)
  elseif(DM_LIBRARY_DEBUG)
    get_filename_component(DM_LIBRARY_DIR "${DM_LIBRARY_DEBUG}" PATH)
  else()
    set(DM_LIBRARY_DIR "")
  endif()
endif()

if (DM_INCLUDE_DIR)
  # Some platforms include multiple DPItypes.hs for multi-lib configurations
  # This is a temporary workaround.  A better solution would be to compile
  # a dummy c file and extract the value of the symbol.
  file(GLOB _DM_CONFIG_HEADERS "${DM_INCLUDE_DIR}/DPItypes.h")
  foreach(_DM_CONFIG_HEADER ${_DM_CONFIG_HEADERS})
    if(EXISTS "${_DM_CONFIG_HEADER}")
      break()
    endif()
  endforeach()
endif()

set(DM_VERSION 8)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DM
                                  REQUIRED_VARS DM_LIBRARY DM_INCLUDE_DIR
                                  VERSION_VAR DM_VERSION)
set(DM_FOUND  ${DM_FOUND})

function(__dm_import_library _target _var _config)
  if(_config)
    set(_config_suffix "_${_config}")
  else()
    set(_config_suffix "")
  endif()

  set(_lib "${${_var}${_config_suffix}}")
  if(EXISTS "${_lib}")
    if(_config)
      set_property(TARGET ${_target} APPEND PROPERTY
        IMPORTED_CONFIGURATIONS ${_config})
    endif()
    set_target_properties(${_target} PROPERTIES
      IMPORTED_LOCATION${_config_suffix} "${_lib}")
  endif()
endfunction()

mark_as_advanced(DM_INCLUDE_DIR DM_LIBRARY)
if(DM_FOUND)
  set(DM_INCLUDE_DIRS "${DM_INCLUDE_DIR}")
  set(DM_LIBRARIES_DIRS "${DM_LIBRARY_DIR}")
  if(NOT TARGET DM::dm)
    add_library(DM::dm UNKNOWN IMPORTED)
    set_target_properties(DM::dm PROPERTIES
                          INTERFACE_INCLUDE_DIRECTORIES "${DM_INCLUDE_DIR}"
                          IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    __dm_import_library(DM::dm DM_LIBRARY "")
    __dm_import_library(DM::dm DM_LIBRARY "RELEASE")
    __dm_import_library(DM::dm DM_LIBRARY "DEBUG")
  endif()
endif()
