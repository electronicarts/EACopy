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
set(ZSTD_STATIC_LINKING_ONLY ON)
add_subdirectory(external/zstd/build/cmake)
if (WIN32)
	target_compile_options(libzstd_static PUBLIC "$<$<CONFIG:RELEASE>:/Oi>")
endif(WIN32)

#-------------------------------------------------------------------------------------------
# lzma
#-------------------------------------------------------------------------------------------
add_subdirectory(external/lzma)
if (WIN32)
	target_compile_options(lzma PUBLIC "$<$<CONFIG:RELEASE>:/Oi>")
endif(WIN32)

#-------------------------------------------------------------------------------------------
# xdelta
#-------------------------------------------------------------------------------------------

add_compile_definitions(SIZEOF_SIZE_T=8 SIZEOF_UNSIGNED_LONG_LONG=8 _WIN32=1 XD3_USE_LARGEFILE64=1 SECONDARY_DJW=1 SECONDARY_LZMA=1 SECONDARY_FGK=1 XD3_WIN32=1 LZMA_API_STATIC)
add_subdirectory(external/xdelta)
target_include_directories(xdelta PRIVATE external/lzma/liblzma/api)
if (WIN32)
	target_compile_options(xdelta PUBLIC "$<$<CONFIG:RELEASE>:/Oi>")
endif(WIN32)

#-------------------------------------------------------------------------------------------
# Library definitions
#-------------------------------------------------------------------------------------------

set(EACOPY_EXTERNAL_LIBS libzstd_static xdelta lzma)

set(EACOPY_SHARED_FILES
	${CMAKE_CURRENT_SOURCE_DIR}/include/EACopyNetwork.h
	${CMAKE_CURRENT_SOURCE_DIR}/source/EACopyNetwork.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/include/EACopyShared.h
	${CMAKE_CURRENT_SOURCE_DIR}/source/EACopyShared.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/source/Addition.manifest)



add_definitions(-DEACOPY_ALLOW_DELTA_COPY_RECEIVE)
set(EACOPY_SHARED_FILES
	${EACOPY_SHARED_FILES}
	${CMAKE_CURRENT_SOURCE_DIR}/include/EACopyDelta.h
	${CMAKE_CURRENT_SOURCE_DIR}/source/EACopyDelta.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/source/EACopyDeltaZstd.h
	${CMAKE_CURRENT_SOURCE_DIR}/source/EACopyDeltaXDelta.h)

add_executable(EACopy
	source/EACopy.cpp
	include/EACopyClient.h
	source/EACopyClient.cpp
	${EACOPY_SHARED_FILES})

target_include_directories(EACopy PUBLIC include)

if (WIN32)
	target_link_libraries(EACopy ${EACOPY_EXTERNAL_LIBS})
endif(WIN32)

if (UNIX)
	target_link_libraries(EACopy ${EACOPY_EXTERNAL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif (UNIX)

if (WIN32)
	add_executable(EACopyService
		source/EACopyService.cpp
		include/EACopyServer.h
		source/EACopyServer.cpp
		${EACOPY_SHARED_FILES})

	target_include_directories(EACopyService PUBLIC include)
	target_link_libraries(EACopyService ${EACOPY_EXTERNAL_LIBS})
endif(WIN32)


if(EACOPY_BUILD_TESTS)
	include(CTest)
	add_subdirectory(test)
endif()
