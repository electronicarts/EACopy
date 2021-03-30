# News for covid-19 times

We have added support for having EACopyService sit on another machine than the network share to enable environments where the network share machine is not running windows (like NetApp etc). This means that anyone can spin up an EACopyService next to the network share and then use "EACopy.exe ... /SERVERADDR \<machine-with-service\> /C" to speed up transfers using zstd compression from the office to your home. Very simple setup.
	
# EACopy

EACopy is an alternative to Robocopy for copying files from one location to another. The main reason EACopy was created was to be able to provide text files containing the files/directories to copy. The second reason was to improve performance when copying large amounts of data to network share where the same files are often copied over and over again but to different destinations on the same network share. (example: \\myserver\builds\myapp\version1\, \\myserver\builds\myapp\version2\, \\myserver\builds\myapp\version3\)

At EA we have a build farm that produce new builds for all supported platforms at quite a fast rate. Binaries and system files for all supported platforms adds up to over 250gb per build and we try to get a full turnaround of these builds every 5 minutes or so. Unfortunately network throughput to the network share is one of the limiting factors as well as storage to be able to go faster. Funny enough most of this data being copied over has been copied 5 minutes ago to another folder since the majority of those 250gb is the same (unless you change something _everything_ depends on) and it feels like we could save lots of work by not copying those files again. This is where EACopyService comes in.

EACopyService is a service that can be started on any windows based server. It acts as an accelerator for clients copying data to the network share and doesnt store any additional state anywhere. It needs to be allowed to create hardlinks on the file system in order to work.

Using EACopyService we've seen builds that has a very small delta from previous builds go down in copy-time from ~2 minutes to 1 second :-)

When EACopy starts copying to a network share it also tries to connect to an EACopyService. If there is no server it behaves just like Robocopy using smb. If it finds a service it switches over to do communication with the service instead of smb. Before copying a file it uses the same criterias as robocopy to compute the key of the file, the key is sent over to EACopyService together with destination path. EACopyService keeps track of all the files it has copied since it started by key and destination. If EACopyService already has the key it will check so the file still exists and has the same key, if the file is still valid it just creates a hardlink for the new file to the old file and return to the EACopy client that the file is already copied. 
If the server doesn't have the key it asks the EACopy client to write the new file. If compression is enabled EACopy will compress and send the data to the service which will write it to the share. If compression is not enabled EACopyService will ask the client to copy the file directly to the share using smb. It self-balance compression based on throughput of uncompressed data. If network is fast it will do less compression, if it for some reason is contended it will do more compression.

Note that the link-to-old-file feature is on a per-file basis so if your files are zipped up to some big archives with one changed file in it, it will still copy these files even though only 1 byte diffed from previous copy. This might be a feature added in the future.

Here's an example on how the summary of a copy could look like when using the EACopyservice (this is a real example from frostbite's farm deploy).
```
----------------------------------------------------------------------------------
                 Total    Copied    Linked   Skipped  Mismatch    FAILED    Extras
   Files:         2259         7      2252         0         0         0         0
   Bytes:         5.1g    658.0k      5.1g      0.0b         0         0         0
   Times:        0.56s      10ms     0.19s       0ms

   FindFile:        0.20s      SendFile:              0ms
   ReadFile:          1ms      SendBytes:           72.1k
   CompressFile:      1ms      CompressLevel:         1.0
   ConnectTime:      89ms

   Server found and used!
```  

## Documentation  
[EACopy usage](doc/Usage.md)  
[Technical documentation](doc/TechDoc.md)  
[Todos](doc/Todo.md)  

## Credits
EACopy was implemented by Henrik Karlsson. Thanks to Roberto Parolin for setting up cmake and github.

## Contributing
Before you can contribute, EA must have a Contributor License Agreement (CLA) on file that has been signed by each contributor.
You can sign here: [Go to CLA](https://electronicarts.na1.echosign.com/public/esignWidget?wid=CBFCIBAA3AAABLblqZhByHRvZqmltGtliuExmuV-WNzlaJGPhbSRg2ufuPsM3P0QmILZjLpkGslg24-UJtek*)

### Pull Request Policy

All code contributions to EACopy are submitted as [Github pull requests](https://help.github.com/articles/using-pull-requests/).  All pull requests will be reviewed by an EACopy maintainer according to the guidelines found in the next section.

Your pull request should:

* merge cleanly
* come with tests
	* tests should be minimal and stable
	* fail before your fix is applied
* pass the test suite
* do not deviate from style already established in the files

## Building
EACopy uses the defacto standard CMake build system.

As an example, look at the "build.bat" file in the scripts folder for how to build the library and build/run the unit tests.
NOTE: to run ctest you need to run the CLI you use as Administrator! Also you must make the changes outlined in "Test Setup" and "Running the Tests" sections below.
```
<start at the project root>

@mkdir build
@pushd build

@call cmake .. -G "Visual Studio 15 2017 Win64" -DEACOPY_BUILD_TESTS:BOOL=ON
rem @call cmake .. -G "Visual Studio 16 2019" -A x64 -DEACOPY_BUILD_TESTS:BOOL=ON
@call cmake --build . --config Release
@call cmake --build . --config Debug

@pushd test
@call ctest -C Release -V
@call ctest -C Debug

@popd
@popd

```

#CI System
Travis CI for EACopy: https://travis-ci.org/github/electronicarts/EACopy

## Test Setup
The test/CMakeLists.txt file might have its tests commented out. If so go to the file and uncomment the test line (as below):

```
# Use CTest
enable_testing()
#Disabled on farm. Enable this for local testing
add_test(EACopyTestRun EACopyTest)
```

## Running the Tests
To run the tests you need to have a source folder and a destination folder specified.

The source folder needs to be a absolute path on one of your local drives. Ex: D:\EACopyTest\source
the destination folder needs to be a unc network path.  This can be a shared location on your local machine Ex: \\localhost\EACopyTest\dest, which is mapped to D:\EACopyTest\dest.

NOTE: These folders have to actually exist on your system or EACopyTest will complain about the folder(s) not existing

You can specify these on the cmdline for EACopyTest.exe:
```
EACopyTest D:\EACopyTest\source \\localhost\EACopyTest\dest
```

Another way to set this up for ease of local development is to set these defines in the top of EACopyTest.cpp to the source and dest folders you have setup:
* Default setting for these values is L""; (Make sure you change the values or You will get an error that you didnt specify the source/dir arguments)

```
#define DEFAULT_SOURCE_DIR  L"D:\\\\EACopyTest\\source"
#define DEFAULT_DEST_DIR  L"\\\\localhost\\EACopyTest\\dest" // local share to D:\EACopyTest\dest
```

You can then set EACopyTest as your startup project in Visual Studio and debug from there, or just run EACopyTest.exe from the cmdline without parameters and it will use those folders set in the code.

## Setting up CMake with Visual Studio for Debugging
Notes: 
 - You need to run Visual Studio as Admin to properly run some tests
 - Once set up make sure to set the CMake settings so EACOPY_BUILD_TESTS is set to true so the tests are generated also.
Reference documentation for setting up CMake: https://docs.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=vs-2019
Reference documenation for running ctests in VS: https://docs.microsoft.com/en-us/visualstudio/test/how-to-use-ctest-for-cpp?view=vs-2019


##Reference links
CMake main page: https://cmake.org/
CTest documentation page: https://cmake.org/cmake/help/latest/manual/ctest.1.html
Basic CMake intro: https://a4z.bitbucket.io/blog/2018/05/17/Speed-up-your-test-cycles-with-CMake.html
CMake Tutorial 1: https://cliutils.gitlab.io/modern-cmake/chapters/testing.html
CMake Tutorial 2: https://bastian.rieck.me/blog/posts/2017/simple_unit_tests/
CMake File example: https://riptutorial.com/cmake/example/14698/basic-test-suite

## License

Modified BSD License (3-Clause BSD license) see the file LICENSE in the project root.
