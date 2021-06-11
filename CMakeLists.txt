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
# Zstd
#-------------------------------------------------------------------------------------------
set(ZSTD_BUILD_PROGRAMS OFF)
set(ZSTD_BUILD_SHARED OFF)
set(ZSTD_BUILD_TESTS OFF)
add_subdirectory(external/zstd/build/cmake)
if (WIN32)
	target_compile_options(libzstd_static PUBLIC "$<$<CONFIG:RELEASE>:/Oi>")
endif(WIN32)

#-------------------------------------------------------------------------------------------
# lzma
#-------------------------------------------------------------------------------------------
add_subdirectory(external/lzma)

#-------------------------------------------------------------------------------------------
# xdelta
#-------------------------------------------------------------------------------------------

add_compile_definitions(SIZEOF_SIZE_T=8 SIZEOF_UNSIGNED_LONG_LONG=8 _WIN32=1 XD3_USE_LARGEFILE64=1 SECONDARY_DJW=1 SECONDARY_LZMA=1 SECONDARY_FGK=1 XD3_WIN32=1 LZMA_API_STATIC)
add_subdirectory(external/xdelta)
target_include_directories(xdelta PRIVATE external/lzma/liblzma/api)

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
	target_link_libraries(EACopy libzstd_static xdelta)
endif(WIN32)

if (UNIX)
	target_link_libraries(EACopy libzstd_static xdelta ${CMAKE_THREAD_LIBS_INIT})
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
	target_link_libraries(EACopyService libzstd_static xdelta)
endif(WIN32)


if(EACOPY_BUILD_TESTS)
	include(CTest)
	add_subdirectory(test)
endif()
