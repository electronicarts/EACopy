# EACopy Technical Documentation

## EACopy

EACopy without using EACopyService is very simple and doesn't do anything magic when copying files. It just open source and destination file, read content from source and write content in to destination. It is possible to play around with different flags like "write through", "no buffering" and "async io" inside the code but in the test cases we've been doing the best performance was given with all these disabled (which is default). Note that the measurements have been made on EA's network with 10gbit transfer speeds and data being copied between machines using ssd.

If /MT:x is being used copying will be concurrent. EACopy spawns x number of worker threads that waits for entries to show up in a queue in order to copy them from source to destination. The main thread will populate that queue by using wildcards or file lists containing wildcards. Using /MT usually makes a huge difference so experiment with the number x. The main thread will also create destination directories as it traverses wildcards/file lists so when the worker threads pick up files to be copied the destination folder already exists. When main thread has found all files to copy it turns itself in to a worker thread and help process queued up files.

For some reason EACopy is slightly faster than RoboCopy in our test cases even in non EACopyService mode and I can only speculate in why but code is very straight forward and uses win32 API calls directly on most cases.

## EACopyService

EACopyService is a service which to be optimal should run on the machine owning the network share and the network share needs to be backed by a drive that EACopyService can create hard links on (https://docs.microsoft.com/en-us/windows/win32/fileio/hard-links-and-junctions). It is also possible to use EACopyService as an external accelerator where the process runs on another machine than the network share. In that case the EACopy.exe process needs to have /SERVERADDR <addr> provided in order to find the external EACopyService. EACopyService can both run as a command line application or a service.

EACopyService is listening to connections from EACopy worker threads. Let's call EACopyService "Server" and EACopy worker thread "Client" from now on.

When a client connects it provides its destination network path which is resolved to a real destination path for the server. The client then starts sending "create directory" requests and "file write" requests. A file write request contains filepath, last written time and file size. The server has a lookup table with previously written files sorted on filename without path, lastWrittenTime and filesize (This is how robocopy identifies a file). If the server finds a matching entry it takes the value of the entry which is the full path to the previously written file. The server then checks if that previously written file still exists and has the same identity. If it has, the server attempts to make a hard link to the old file. If it succeeds it tells the client that copy has already been handled, if it fails it tells the client that it needs to copy the file. The server also updates the lookup table so the new file is now representing the key.

The server has a max history count (defaults to 500000) which when hit will start dropping oldest entries. This is why we update the lookup table since a very old file could be written at server process start but reused over and over again.

## EACopy using EACopyService

When EACopy is using the EACopyService it is also possible to enable compression. Compression is using zstd and it is possible to set compression ratio or use the compression in auto-balance mode. In auto-balance mode the client constantly measure wall-time cost for transferring bytes. If it increases compression and notice that bytes/second goes down it decreases compression. This means that running EACopy on a low performant cpu with a fast network connection will end up with very low compression while a powerful cpu with slow network connection will do the opposite.

## Delta compression

EACopy does currently not support delta compression on a per-file bases. It would be fairly simple to for example integrate the rsync algorithm to reduce amount of data transferred. If anyone wants to take on the work we are more than happy to get a pull request :-)

