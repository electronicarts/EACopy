#-------------------------------------------------------------------------------------------
# Copyright (C) Electronic Arts Inc.  All rights reserved.
#-------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.1)
project(EACopy CXX)

# NOTE: Only used in multi-configuration environments
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "My multi config types" FORCE)

#-------------------------------------------------------------------------------------------
# Options
#-------------------------------------------------------------------------------------------
option(EACOPY_BUILD_TESTS "Enable generation of build files for tests" OFF)

if (WIN32)
	SET(CMAKE_CXX_FLAGS "/GR-") 
endif(WIN32)

if (UNIX)
	find_package (Threads)
endif (UNIX)


#-------------------------------------------------------------------------------------------
# Dependencies
#-------------------------------------------------------------------------------------------
add_subdirectory(external/zstd)

#-------------------------------------------------------------------------------------------
# Library definitions
#-------------------------------------------------------------------------------------------
add_executable(EACopy
	source/EACopy.cpp
	include/EACopyClient.h
	source/EACopyClient.cpp
	include/EACopyNetwork.h
	source/EACopyNetwork.cpp
	include/EACopyShared.h
	source/EACopyShared.cpp
	source/Addition.manifest)

target_include_directories(EACopy PUBLIC include)

if (WIN32)
	target_link_libraries(EACopy zstd)
endif(WIN32)

if (UNIX)
	target_link_libraries(EACopy zstd ${CMAKE_THREAD_LIBS_INIT})
endif (UNIX)

if (WIN32)
	add_executable(EACopyService
		source/EACopyService.cpp
		include/EACopyServer.h
		source/EACopyServer.cpp
		include/EACopyNetwork.h
		source/EACopyNetwork.cpp
		include/EACopyShared.h
		source/EACopyShared.cpp
		source/Addition.manifest)

	target_include_directories(EACopyService PUBLIC include)
	target_link_libraries(EACopyService zstd)
endif(WIN32)


if(EACOPY_BUILD_TESTS)
	include(CTest)
	add_subdirectory(test)
endif()
