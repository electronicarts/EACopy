# EACopy Usage

## EACopy

Usage to copy file(s):
```
EACopy source destination [file [file]...] [options]
```

Parameter | Description
--- | ---
```source ``` | Drive or network paths
```destination``` | Drive or network paths  
```file``` | names/wildcards: default is \*.*  
<br>

Option | Description
--- | ---
```/S``` | Copy subdirectories, but not empty ones  
```/E``` | Copy subdirectories, including empty ones  
```/LEV:n``` | Only copy the top n levels of the source directory tree  
```/J``` | Enable unbuffered I/O for all files  
```/NJ``` | Disable unbuffered I/O for all files  
```/PURGE``` | Delete dest files/dirs that no longer exist in source  
```/MIR``` | Mirror a directory tree (equivalent to /E plus /PURGE)  
```/KSY``` | Keep Symlinked subdirectories at destination  
```/F``` | All files copied are Flattened in to destination folder  
```/I file [file]...``` | Use text file containing files/directories/wildcards. A line can also add dest to explicitly write dest and options to add additional params. /PURGE only supported  
```/IX file [file]...``` | Same as /I but excluding files/directories instead
```/XF file [file]...``` | Exclude Files matching given names/paths/wildcards  
```/OF file [file]...``` | Optional Files matching given names/paths/wildcards  
```/MT[:n]``` | Do multi-threaded copies with n threads (default 8), n must be at least 1 and not greater than 128  
```/NOSERVER``` | Will not try to connect to Server  
```/SERVER``` | Must connect to Server. Fails copy if not succeed
```/PORT:n``` | Port used to connect to Server (default 18099).
```/C[:n]``` | Compression Level. No value provided will auto adjust, n must be between 1=lowest, 22=highest. (zstd) 
```/DCOPY:copyflag[s]``` | What to COPY for directories (default is /DCOPY:DA) (copyflags : D=Data, A=Attributes, T=Timestamps)  
```/NODCOPY``` | COPY NO directory info (by default /DCOPY:DA is done)  
```/R:n``` | Number of Retries on failed copies: default 1 million  
```/W:n``` | Wait time between retries: default is 30 seconds  
```/LOG:file``` | Output status to LOG file (overwrite existing log)  
```/LOGMIN``` | Logs minimal amount of information  
```/VERBOSE``` | Output debug logging  
```/NJH``` | No Job Header  
```/NJS``` | No Job Summary
<br>

Usage to check service stats
```
EACopy /STATS destination
```  
<br>

## EACopyService

Usage to run service
 ```
 EACopyService [options]
 ```
 <br>
 
Option | Description
--- | ---
 ```/P:n ``` | Port that server will listen on (defaults to 18099).
```/HISTORY:n``` | Max number of files tracked in history (defaults to 500000).
```/J``` | Enable unbuffered I/O for all files.
```/NJ``` | Disable unbuffered I/O for all files.
```/LOG:file``` | Output status to LOG file (overwrite existing log).
```/VERBOSE``` | Output debug logging.
```/INSTALL``` | Install and start as auto starting windows service. Will start with parameters provided with /INSTALL call
```/REMOVE``` | Stop and remove service.
