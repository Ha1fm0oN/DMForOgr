add_gdal_driver(
  TARGET ogr_DM
  SOURCES ogrdmdatasource.cpp
	  ogrdmconnection.cpp
	  ogr_dm.h
          ogrdmdriver.cpp
          ogrdmlayer.cpp
	  ogrdmstatement.cpp
          ogrdmresultlayer.cpp
          ogrdmtablelayer.cpp
          ogrdmutility.cpp
	  ogrdmtransform.cpp
          PLUGIN_CAPABLE)
gdal_standard_includes(ogr_DM)

target_include_directories(ogr_DM PRIVATE ${DM_INCLUDE_DIRS} $<TARGET_PROPERTY:SOURCE_DIR>)
gdal_target_link_libraries(ogr_DM PRIVATE DM::dm)

if (OGR_ENABLE_DRIVER_DM_PLUGIN)
    if(NOT MSVC AND CMAKE_THREAD_LIBS_INIT)
        gdal_target_link_libraries(ogr_DM PRIVATE ${CMAKE_THREAD_LIBS_INIT})
    endif()
endif()