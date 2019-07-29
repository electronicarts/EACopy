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

SET(CMAKE_CXX_FLAGS "/GR-") 

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
	source/EACopyShared.cpp)

add_executable(EACopyService
	source/EACopyService.cpp
	include/EACopyServer.h
	source/EACopyServer.cpp
	include/EACopyNetwork.h
	source/EACopyNetwork.cpp
	include/EACopyShared.h
	source/EACopyShared.cpp)

target_include_directories(EACopy PUBLIC include)
target_include_directories(EACopyService PUBLIC include)


#-------------------------------------------------------------------------------------------
# Dependencies
#-------------------------------------------------------------------------------------------
add_subdirectory(external/zstd)

target_link_libraries(EACopy zstd)
target_link_libraries(EACopyService zstd)


if(EACOPY_BUILD_TESTS)
	add_subdirectory(test)
endif()
