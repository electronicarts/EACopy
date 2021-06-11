#-------------------------------------------------------------------------------------------
# Copyright (C) Electronic Arts Inc.  All rights reserved.
#-------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.1)
project(EACopyTest CXX)

include(CTest)

#-------------------------------------------------------------------------------------------
# Executable definition
#-------------------------------------------------------------------------------------------

if (WIN32)
	set(ServerFiles ../include/EACopyServer.h ../source/EACopyServer.cpp)
endif(WIN32)

if (UNIX)
	find_package (Threads)
endif (UNIX)

add_executable(EACopyTest
	EACopyTest.cpp
	../include/EACopyClient.h
	../source/EACopyClient.cpp
	../source/EACopyNetwork.cpp
	../include/EACopyNetwork.h
	../include/EACopyShared.h
	../source/EACopyShared.cpp
	../source/Addition.manifest
	${ServerFiles})

target_include_directories(EACopyTest PUBLIC ../include)

#-------------------------------------------------------------------------------------------
# Dependencies 
#-------------------------------------------------------------------------------------------
target_link_libraries(EACopyTest libzstd_static ${CMAKE_THREAD_LIBS_INIT})

#-------------------------------------------------------------------------------------------
# Run Unit tests and verify the results.
#-------------------------------------------------------------------------------------------

# Use CTest
enable_testing()
#Disabled on farm. Enable this for local testing
#add_test(EACopyTestRun EACopyTest)
