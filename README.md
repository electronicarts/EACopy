# EACopy

EACopy is an alternative to Robocopy for copying files from one location to another. The main reason EACopy was created was to be able to provide text files containing the files/directories to copy. The second reason was to improve performance when copying large amounts of data to network share where the same files are often copied over and over again but to different destinations on the network share. (example: \\myserver\builds\myapp\version1\, \\myserver\builds\myapp\version2\, \\myserver\builds\myapp\version3\)

On frostbite we have a build farm that produce new builds for all supported platforms at quite a fast rate. Binaries and system files for all supported platforms adds up to over 250gb per build and we try to get a full turnaround of these builds every 5 minutes or so. Unfortunately network throughput to the network share is one of the limiting factors as well as storage to be able to go faster. Funny enough most of this data being copied over has been copied over 5 minutes ago to another folder since the majority of those 250gb is the same (unless you change something _everything_ pulls in) and it feels like we could save lots of work by not copying those files again. This is where EACopyService comes in.

EACopyService is a service that can be started on any windows based server that hosts a network share. It acts as an accellerator for clients to copy data to the network share and doesnt store any additional state anywhere. It needs to be allowed to create hardlinks on the file system in order to work.

Using EACopyService we've seen builds that has a very small delta from previous builds go down in copy-time from ~2 minutes to 1 second :-)

When EACopy starts copying to a network share it also tries to connect to an EACopyService. If there is no server it behaves just like Robocopy using smb. If it finds a service it switches over to do communication with the service instead of smb. Before copying a file it uses the same criterias as robocopy to compute the key of the file, the key is sent over to EACopyService together with destination path. EACopyService keeps track of all the files it has copied since it started by key and destination. If EACopyService already has the key it will check so the file still exists and has the same key, if the file is still valid it just creates a hardlink for the new file to the old file and return to the EACopy client that the file is already copied. 
If the server doesn't have the key it asks the EACopy client to send the new file. EACopy uses zstd to compress the file while sending. It self-balance compression based on throughput of uncompressed data. If network is fast it will do less compression, if it for some reason is contended it will do more compression.

Note that the copy-reduction feature is on a per-file basis so if your files are zipped up to some big archives with one changed file in it, it will still copy these files even though only 1 byte diffed from previous copy. This might be a feature added in the future.


## Documentation
TODO: Add link to library documentation.
Please see [EACopy Introduction](doc/introduction.md).

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

```
<start at the project root>

@mkdir build
@pushd build

@call cmake .. -G "Visual Studio 15 2017 Win64" -DEACOPY_BUILD_TESTS:BOOL=ON 
@call cmake --build . --config Release

@pushd test
@call ctest -C Release -V

@popd
@popd
```

## License

Modified BSD License (3-Clause BSD license) see the file LICENSE in the project root.
