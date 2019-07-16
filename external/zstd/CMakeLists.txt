#-------------------------------------------------------------------------------------------
# Copyright (C) Electronic Arts Inc.  All rights reserved.
#-------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.1)
project(zstd C)

#-------------------------------------------------------------------------------------------
# Library definition
#-------------------------------------------------------------------------------------------
file(GLOB ZSTD_SOURCES "**/*.c")
add_library(zstd ${ZSTD_SOURCES})

#-------------------------------------------------------------------------------------------
# Include dirs
#-------------------------------------------------------------------------------------------
target_include_directories(zstd PUBLIC ../)
target_include_directories(zstd PUBLIC ./)
target_include_directories(zstd PUBLIC common)

