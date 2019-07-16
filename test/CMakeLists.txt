#-------------------------------------------------------------------------------------------
# Copyright (C) Electronic Arts Inc.  All rights reserved.
#-------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.1)
project(EACopyTest CXX)


#-------------------------------------------------------------------------------------------
# Executable definition
#-------------------------------------------------------------------------------------------

add_executable(EACopyTest
	EACopyTest.cpp
	../include/EACopyClient.h
	../source/EACopyClient.cpp
	../include/EACopyServer.h
	../source/EACopyServer.cpp
	../source/EACopyNetwork.cpp
	../include/EACopyNetwork.h
	../include/EACopyShared.h
	../source/EACopyShared.cpp)

target_include_directories(EACopyTest PUBLIC ../include)

#-------------------------------------------------------------------------------------------
# Dependencies 
#-------------------------------------------------------------------------------------------
target_link_libraries(EACopyTest zstd)

#-------------------------------------------------------------------------------------------
# Run Unit tests and verify the results.
#-------------------------------------------------------------------------------------------

#include(CTest)
#add_test(EACopyTestRuns EACopyTest)
