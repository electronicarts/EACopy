// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void printHelp()
{
	logInfoLinef();
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef(L"  EACopy v%S - File Copy for Win. (c) Electronic Arts.  All Rights Reserved. ", ClientVersion);
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef();
	logInfoLinef(L"             Usage :: EACopy source destination [file [file]...] [options]");
	logInfoLinef();
	logInfoLinef(L"            source :: Source Directory (drive:\\path or \\\\server\\share\\path).");
	logInfoLinef(L"       destination :: Destination Dir  (drive:\\path or \\\\server\\share\\path).");
	logInfoLinef(L"              file :: File(s) to copy  (names/wildcards: default is \"*.*\").");
	logInfoLinef();
	logInfoLinef(L"::");
	logInfoLinef(L":: Copy options :");
	logInfoLinef(L"::");
	logInfoLinef(L"                /S :: copy Subdirectories, but not empty ones.");
	logInfoLinef(L"                /E :: copy subdirectories, including Empty ones.");
	logInfoLinef(L"            /LEV:n :: only copy the top n LEVels of the source directory tree.");
	logInfoLinef(L"                /J :: Enable unbuffered I/O for all files.");
	logInfoLinef(L"               /NJ :: Disable unbuffered I/O for all files.");
	logInfoLinef();
	logInfoLinef(L"            /PURGE :: delete dest files/dirs that no longer exist in source.");
    logInfoLinef(L"              /MIR :: MIRror a directory tree (equivalent to /E plus /PURGE).");
	logInfoLinef(L"              /KSY :: Keep SYmlinked subdirectories at destination.");
	logInfoLinef();
	logInfoLinef(L"                /F :: all files copied are Flattened in to destination folder.");
	logInfoLinef(L"/I file [file]...  :: use text file containing files/directories/wildcards.");
	logInfoLinef(L"                      A line can also add dest to explicitly write dest and ");
	logInfoLinef(L"                      options to add additional params. /PURGE only supported");
	logInfoLinef(L"/IX file [file]... :: same as /I but excluding files/directories instead.");
	logInfoLinef();
	logInfoLinef(L"/XD dir [dir]...   :: eXclude Directories matching given names/paths/wildcards.");
	logInfoLinef(L"/XF file [file]... :: eXclude Files matching given names/paths/wildcards.");
	logInfoLinef(L"/OF file [file]... :: Optional Files matching given names/paths/wildcards. Only used for FileLists.");
	logInfoLinef();
	logInfoLinef(L"           /MT[:n] :: do multi-threaded copies with n threads (default 8).");
	logInfoLinef(L"                      n must be at least 1 and not greater than 128.");
	logInfoLinef();
	logInfoLinef(L"         /NOSERVER :: will not try to connect to Server.");
	logInfoLinef(L"           /SERVER :: must connect to Server. Fails copy if not succeed");
	logInfoLinef(L"  /SERVERADDR addr :: Address used to connect to Server. This is only needed if using a proxy EACopyServer sitting on the side");
	logInfoLinef(L"     /SERVERPORT:n :: Port used to connect to Server (default %u).", DefaultPort);
	logInfoLinef(L"           /C[:n]  :: use Compression. No value provided will auto adjust level");
	logInfoLinef(L"                      n must be between 1=lowest, 22=highest. (uses zstd)");
	#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
	logInfoLinef(L"           /DC[:b] :: use DeltaCompression. Provide value to set min file size");
	logInfoLinef(L"                      b defaults to %s (uses rsync algorithm)", toPretty(DefaultDeltaCompressionThreshold).c_str());
	logInfoLinef();
	#endif
	logInfoLinef(L"/DCOPY:copyflag[s] :: what to COPY for directories (default is /DCOPY:DA).");
	logInfoLinef(L"                      (copyflags : D=Data, A=Attributes, T=Timestamps).");
	logInfoLinef();
	logInfoLinef(L"          /NODCOPY :: COPY NO directory info (by default /DCOPY:DA is done).");
	logInfoLinef();
	logInfoLinef(L"              /R:n :: number of Retries on failed copies: default 1 million.");
	logInfoLinef(L"              /W:n :: Wait time between retries: default is 30 seconds.");
	logInfoLinef(L"         /LOG:file :: output status to LOG file (overwrite existing log).");
	logInfoLinef(L"           /LOGMIN :: logs minimal amount of information.");
	logInfoLinef(L"          /VERBOSE :: output debug logging.");
	logInfoLinef(L"              /NJH :: No Job Header.");
	logInfoLinef(L"              /NJS :: No Job Summary.");
	logInfoLinef();
	logInfoLinef(L"  Additional Usage :: EACopy /STATS destination      - Show server stats  ");
	logInfoLinef(L"                      destination must be a full path just as when you copy.");
	logInfoLinef();
}

struct Settings : ClientSettings
{
	WString logFileName;
	bool printJobHeader = true;
	bool printJobSummary = true;
};

bool readSettings(Settings& outSettings, int argc, wchar_t* argv[])
{
	int argIndex = 1;
	int paramIndex = 0; // The non-flag parameters

	const wchar_t* activeCommand = L"";
	bool copySubdirectories = false;

	// Read options
	while (argIndex < argc)
	{
		wchar_t* arg = argv[argIndex++];

		if (*arg == '/')
			activeCommand = L"";

		if (equalsIgnoreCase(arg, L"/S"))
		{
			if (outSettings.copyEmptySubdirectories)
			{
				logErrorf(L"Can't combine /S and /E");
				return false;
			}
			copySubdirectories = true;
			if (outSettings.copySubdirDepth == 0)
				outSettings.copySubdirDepth = 10000;
		}
		else if (equalsIgnoreCase(arg, L"/E"))
		{
			if (copySubdirectories && !outSettings.copyEmptySubdirectories)
			{
				logErrorf(L"Can't combine /S and /E");
				return false;
			}
			copySubdirectories = true;
			outSettings.copyEmptySubdirectories = true;
			if (outSettings.copySubdirDepth == 0)
				outSettings.copySubdirDepth = 10000;
		}
		else if (startsWithIgnoreCase(arg, L"/LEV:"))
		{
			outSettings.copySubdirDepth = _wtoi(arg + 5);
		}
		else if (equalsIgnoreCase(arg, L"/J"))
		{
			outSettings.useBufferedIO = UseBufferedIO_Enabled;
		}
		else if (equalsIgnoreCase(arg, L"/NJ"))
		{
			outSettings.useBufferedIO = UseBufferedIO_Disabled;
		}
		else if (equalsIgnoreCase(arg, L"/PURGE"))
		{
			outSettings.purgeDestination = true;
			outSettings.copySubdirDepth = 0;
		}
		else if (equalsIgnoreCase(arg, L"/MIR"))
		{
			outSettings.purgeDestination = true;
			outSettings.copySubdirDepth = 10000;
			outSettings.copyEmptySubdirectories = true;
			copySubdirectories = true;
		}
		else if (equalsIgnoreCase(arg, L"/KSY"))
		{
			outSettings.replaceSymLinksAtDestination = false;
		}
		else if (equalsIgnoreCase(arg, L"/F"))
		{
			outSettings.flattenDestination = true;
		}
		else if (equalsIgnoreCase(arg, L"/I"))
		{
			activeCommand = L"I";
		}
		else if (startsWithIgnoreCase(arg, L"/I:"))
		{
			if (!outSettings.filesOrWildcards.empty())
			{
				logErrorf(L"Can't combine file(s) with /I");
				return false;
			}
			outSettings.filesOrWildcardsFiles.push_back(arg + 3);
		}
		else if (equalsIgnoreCase(arg, L"/IX"))
		{
			activeCommand = L"IX";
		}
		else if (startsWithIgnoreCase(arg, L"/MT"))
		{
			outSettings.threadCount = 8;
			if (arg[3] == ':')
				outSettings.threadCount = _wtoi(arg + 4);
		}
		else if (equalsIgnoreCase(arg, L"/NOSERVER"))
		{
			outSettings.useServer = UseServer_Disabled;
		}
		else if (equalsIgnoreCase(arg, L"/SERVER"))
		{
			outSettings.useServer = UseServer_Required;
		}
		else if (equalsIgnoreCase(arg, L"/SERVERADDR"))
		{
			activeCommand = L"SERVERADDR";
		}
		else if (startsWithIgnoreCase(arg, L"/SERVERPORT:"))
		{
			outSettings.serverPort = _wtoi(arg + 6);
		}
		else if (startsWithIgnoreCase(arg, L"/C"))
		{
			outSettings.compressionEnabled = true;
			if (arg[2] == ':')
				outSettings.compressionLevel = _wtoi(arg + 3);
		}
		else if (startsWithIgnoreCase(arg, L"/DC"))
		{
			outSettings.deltaCompressionThreshold = 0;
			if (arg[3] == ':')
				outSettings.deltaCompressionThreshold = _wtoi(arg + 4);
		}
		else if (startsWithIgnoreCase(arg, L"/XF"))
		{
			activeCommand = L"XF";
		}
		else if (startsWithIgnoreCase(arg, L"/XD"))
		{
			activeCommand = L"XD";
		}
		else if (startsWithIgnoreCase(arg, L"/OF"))
		{
			activeCommand = L"OF";
		}
		else if (startsWithIgnoreCase(arg, L"/DCOPY:"))
		{
			outSettings.dirCopyFlags = 0;
			arg += 7;
			while (*arg)
			{
				if (*arg == 'D')
					outSettings.dirCopyFlags |= FileFlags_Data;
				else if (*arg == 'A')
					outSettings.dirCopyFlags |= FileFlags_Attributes;
				else if (*arg == 'T')
					outSettings.dirCopyFlags |= FileFlags_Timestamps;
				++arg;
			}
			// TODO:
		}
		else if (equalsIgnoreCase(arg, L"/NODCOPY"))
		{
			outSettings.dirCopyFlags = 0;
		}
		else if (startsWithIgnoreCase(arg, L"/R:"))
		{
			outSettings.retryCount = _wtoi(arg + 3);
		}
		else if (startsWithIgnoreCase(arg, L"/W:"))
		{
			outSettings.retryWaitTimeMs = _wtoi(arg + 3) * 1000;
		}
		else if(startsWithIgnoreCase(arg, L"/LOG:"))
		{
			outSettings.logFileName = arg + 5;
		}
		else if(equalsIgnoreCase(arg, L"/LOGMIN"))
		{
			outSettings.logProgress = false;
		}
		else if (equalsIgnoreCase(arg, L"/VERBOSE"))
		{
			outSettings.logDebug = true;
		}
		else if (equalsIgnoreCase(arg, L"/NJH"))
		{
			outSettings.printJobHeader = false;
		}
		else if (equalsIgnoreCase(arg, L"/NJS"))
		{
			outSettings.printJobSummary = false;
		}
		else if (equalsIgnoreCase(arg, L"/NP"))
		{
			// Noop
		}
		else
		{
			// If no activeCommand this will be a parameter
			if (!*activeCommand && *arg != '/')
			{
				if (paramIndex == 0) // Source dir
				{
					outSettings.sourceDirectory = arg;
					WString& dir = outSettings.sourceDirectory;
					convertSlashToBackslash((wchar_t*)dir.c_str());
					if (dir[dir.size()-1] != '\\')
						dir += '\\';
				}
				else if (paramIndex == 1) // Dest dir
				{
					outSettings.destDirectory = arg;
					WString& dir = outSettings.destDirectory;
					convertSlashToBackslash((wchar_t*)dir.c_str());
					if (dir[dir.size()-1] != '\\')
						dir += '\\';
				}
				else // Wildcard(s)
				{
					outSettings.filesOrWildcards.push_back(arg);
				}

				paramIndex++;
			}
			else if (equalsIgnoreCase(activeCommand, L"SERVERADDR"))
			{
				outSettings.serverAddress = arg;
			}
			else if (equalsIgnoreCase(activeCommand, L"XF"))
			{
				outSettings.excludeWildcards.push_back(arg);
			}
			else if (equalsIgnoreCase(activeCommand, L"XD"))
			{
				outSettings.excludeWildcardDirectories.push_back(arg);
			}
			else if (equalsIgnoreCase(activeCommand, L"OF"))
			{
				outSettings.optionalWildcards.push_back(arg);
			}
			else if (equalsIgnoreCase(activeCommand, L"I"))
			{
				if (!outSettings.filesOrWildcards.empty())
				{
					logErrorf(L"Can't combine file(s) with /I");
					return false;
				}
				outSettings.filesOrWildcardsFiles.push_back(arg);
			}
			else if (equalsIgnoreCase(activeCommand, L"IX"))
			{
				if (!outSettings.filesOrWildcards.empty())
				{
					logErrorf(L"Can't combine file(s) with /IX");
					return false;
				}
				outSettings.filesExcludeFiles.push_back(arg + 3);
			}
			else
			{
				logErrorf(L"Unknown option %s", arg);
				return false;
			}
		}
	}

	{
		WString temp;
		outSettings.sourceDirectory = optimizeUncPath(outSettings.sourceDirectory.c_str(), temp);
		#ifndef _DEBUG
		outSettings.destDirectory = optimizeUncPath(outSettings.destDirectory.c_str(), temp, outSettings.useServer != UseServer_Required);
		#endif
	}

	if (outSettings.filesOrWildcardsFiles.empty() && outSettings.filesOrWildcards.empty())
		outSettings.filesOrWildcards.push_back(L"*.*");

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy

int wmain(int argc, wchar_t* argv[])
{
	using namespace eacopy;

	u64 startTimeMs = getTimeMs();

	if (argc <= 2 || equalsIgnoreCase(argv[1], L"/?"))
	{
		printHelp();
		return 0;
	}

	if (argc == 3 && equalsIgnoreCase(argv[1], L"/STATS"))
	{
		Settings settings;
		WString temp;
		settings.destDirectory = optimizeUncPath(argv[2], temp, false);
		Client client(settings);
		Log log;
		log.init(nullptr, false, false);
		LogContext logContext(log);
		logInfoLinef();
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef();
		int res = client.reportServerStatus(log);
		logInfoLinef();
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef();
		log.deinit();
		return res;
	}

	Settings settings;
	if (!readSettings(settings, argc, argv))
		return -1;

	//SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	Log log;
	log.init(settings.logFileName.c_str(), settings.logDebug, false);
	LogContext logContext(log);

	if (settings.printJobHeader)
	{
		logInfoLinef();
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef(L"  EACopy v%S - File Copy for Windows.   (c) Electronic Arts.  All Rights Reserved.", ClientVersion);
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef();
		logInfoLinef(L"  Source : %s", settings.sourceDirectory.c_str());
		logInfoLinef(L"    Dest : %s", settings.destDirectory.c_str());
		logInfoLinef();
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef();

		if (!settings.logProgress)
			logInfoLinef(L"   Running...");
	}

	Client client(settings);

	ClientStats stats;
	int res = client.process(log, stats);

	if (settings.printJobSummary)
	{
		logInfoLinef();
		logInfof(L"-------------------------------------------------------------------------------");
		logInfoLinef();
	}

	if (res == -1)
	{
		log.deinit();
		return -1;
	}

	u64 endTimeMs = getTimeMs();

	if (settings.printJobSummary)
	{
		u64 totalTimeMs = endTimeMs - startTimeMs;
		auto totalCount = stats.copyCount+stats.linkCount+stats.skipCount;
		auto totalSize = stats.copySize+stats.linkSize+stats.skipSize;

		// Report results
		logInfoLinef(L"                 Total    Copied    Linked   Skipped  Mismatch    FAILED    Extras");
		//logInfoLinef(L"    Dirs:      %7i   %7i   %7i   %7i   %7i   %7i", 1, 2, 3, 4, 5, 6);
		logInfoLinef(L"   Files:      %7i   %7i   %7i   %7i   %7i   %7i   %7i", totalCount, stats.copyCount, stats.linkCount, stats.skipCount, 0, stats.failCount, 0);
		logInfoLinef(L"   Bytes:     %s  %s  %s  %s   %7i   %7i   %7i", toPretty(totalSize, 7).c_str(), toPretty(stats.copySize, 7).c_str(), toPretty(stats.linkSize, 7).c_str(), toPretty(stats.skipSize, 7).c_str(), 0, 0, 0);
		logInfoLinef(L"   Times:     %s  %s  %s  %s", toHourMinSec(totalTimeMs, 7).c_str(), toHourMinSec(stats.copyTimeMs, 7).c_str(), toHourMinSec(stats.linkTimeMs, 7).c_str(), toHourMinSec(stats.skipTimeMs, 7).c_str());
	
		if (stats.destServerUsed)
		{
			logInfoLinef();
			logInfoLinef(L"   FindFile:     %s      SendFile:         %s", toHourMinSec(stats.findFileTimeMs, 7).c_str(), toHourMinSec(stats.sendTimeMs, 7).c_str());
			logInfoLinef(L"   ReadFile:     %s      SendBytes:        %s", toHourMinSec(stats.copyStats.readTimeMs, 7).c_str(), toPretty(stats.sendSize, 7).c_str());
			logInfoLinef(L"   CompressFile: %s      CompressLevel:     %7.1f", toHourMinSec(stats.compressTimeMs, 7).c_str(), stats.compressionAverageLevel);
			logInfoLinef(L"   ConnectTime:  %s      DeltaCompress:    %s", toHourMinSec(stats.connectTimeMs, 7).c_str(), toHourMinSec(stats.deltaCompressionTimeMs, 7).c_str());
			logInfoLinef(L"   CreateDir:    %s", toHourMinSec(stats.createDirTimeMs, 7).c_str());
			logInfoLinef();
			logInfoLinef(L"   Server found and used!");
		}
		else if (stats.sourceServerUsed)
		{
			logInfoLinef();
			logInfoLinef(L"   FindFile:     %s      RecvFile:         %s", toHourMinSec(stats.findFileTimeMs, 7).c_str(), toHourMinSec(stats.recvTimeMs, 7).c_str());
			logInfoLinef(L"   WriteFile:    %s      RecvBytes:        %s", toHourMinSec(stats.copyStats.writeTimeMs, 7).c_str(), toPretty(stats.recvSize, 7).c_str());
			logInfoLinef(L"   DecompreFile: %s                          ", toHourMinSec(stats.decompressTimeMs, 7).c_str());
			logInfoLinef(L"   ConnectTime:  %s      DeltaCompress:    %s", toHourMinSec(stats.connectTimeMs, 7).c_str(), toHourMinSec(stats.deltaCompressionTimeMs, 7).c_str());
			logInfoLinef(L"   CreateDir:    %s", toHourMinSec(stats.createDirTimeMs, 7).c_str());
			logInfoLinef();
			logInfoLinef(L"   Server found and used!");
		}
		else
		{
			logInfoLinef();
			logInfoLinef(L"   FindFile:     %s      CreateFileWrite:  %s", toHourMinSec(stats.findFileTimeMs, 7).c_str(), toHourMinSec(stats.copyStats.createWriteTimeMs, 7).c_str());
			logInfoLinef(L"   ReadFile:     %s      WriteFile:        %s", toHourMinSec(stats.copyStats.readTimeMs, 7).c_str(), toHourMinSec(stats.copyStats.writeTimeMs, 7).c_str());
			logInfoLinef(L"   ConnectTime:  %s      SetLastWriteTime: %s", toHourMinSec(stats.connectTimeMs, 7).c_str(), toHourMinSec(stats.copyStats.setLastWriteTimeTimeMs, 7).c_str());
			logInfoLinef(L"   CreateDir:    %s", toHourMinSec(stats.createDirTimeMs, 7).c_str());
			logInfoLinef();

			if (stats.serverAttempt && !stats.destServerUsed)
				logInfoLinef(L"   Server not found (Spent ~%s trying to connect. Use /NOSERVER to disable attempt)", toHourMinSec(stats.connectTimeMs/max(1, settings.threadCount)).c_str());
			else
				logInfoLinef(L"   No server used!");
		}
	}

	log.deinit([&]()
	{
		if (settings.printJobSummary)
		{
			u64 endLogTimeMs = getTimeMs();
			if (endLogTimeMs - endTimeMs > 100)
			{
				logInfoLinef();
				logInfoLinef(L"   Spent %.1f seconds waiting for log output to finish (Consider using /LOG:file or /LOGMIN)", float(endLogTimeMs - endTimeMs)*0.001f);
			}
		}
	});

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
