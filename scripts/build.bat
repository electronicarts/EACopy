@mkdir build
@pushd build

rem @call cmake .. -G "Visual Studio 15 2017 Win64" -DEACOPY_BUILD_TESTS:BOOL=ON
rem @call cmake .. -G "Visual Studio 16 2019" -A x64 -DEACOPY_BUILD_TESTS:BOOL=ON
@call cmake .. -G "Visual Studio 17 2022" -DEACOPY_BUILD_TESTS:BOOL=ON
@call cmake --build . --config Release
@call cmake --build . --config Debug

@pushd test
@call ctest -C Release -V
@call ctest -C Debug

@popd
@popd
