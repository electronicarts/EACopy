// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

inline int wtoi(const wchar_t *str) { return (int)wcstol(str, 0, 10); }

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void printHelp()
{
	logInfoLinef();
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef(L"  EACopy v%hs - File Copy for Win. (c) Electronic Arts.  All Rights Reserved. ", ClientVersion);
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
	logInfoLinef(L"                /F :: all files copied are Flattened in to destination directory.");
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
	logInfoLinef(L"           /C[:n]  :: use Compression. No value provided will auto adjust level. Only works with server");
	logInfoLinef(L"                      n must be between 1=lowest, 22=highest. (uses zstd)");
	#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
	logInfoLinef(L"           /DC[:b] :: use DeltaCompression. Provide value to set min file size");
	logInfoLinef(L"                      b defaults to %ls (uses rsync algorithm)", toPretty(DefaultDeltaCompressionThreshold).c_str());
	#endif
	logInfoLinef();
	logInfoLinef(L"/LINK [dir]...     :: will try to create file links when files are the same. Provide extra dirs to link to");
	logInfoLinef();
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
			outSettings.copySubdirDepth = wtoi(arg + 5);
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
			//outSettings.copySubdirDepth = 0; // Purge in robocopy is weird.. revisit
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
				outSettings.threadCount = wtoi(arg + 4);
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
			outSettings.serverPort = wtoi(arg + 6);
		}
		else if (startsWithIgnoreCase(arg, L"/C"))
		{
			outSettings.compressionEnabled = true;
			if (arg[2] == ':')
				outSettings.compressionLevel = wtoi(arg + 3);
		}
		else if (startsWithIgnoreCase(arg, L"/DC"))
		{
			outSettings.deltaCompressionThreshold = 0;
			if (arg[3] == ':')
				outSettings.deltaCompressionThreshold = wtoi(arg + 4);
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
		else if (startsWithIgnoreCase(arg, L"/LINK"))
		{
			outSettings.useFileLinks = true;
			activeCommand = L"LINK";
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
			outSettings.retryCount = wtoi(arg + 3);
		}
		else if (startsWithIgnoreCase(arg, L"/W:"))
		{
			outSettings.retryWaitTimeMs = wtoi(arg + 3) * 1000;
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
					outSettings.sourceDirectory = getCleanedupPath(arg);
				}
				else if (paramIndex == 1) // Dest dir
				{
					outSettings.destDirectory = getCleanedupPath(arg);
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
			else if (equalsIgnoreCase(activeCommand, L"link"))
			{
				outSettings.additionalLinkDirectories.push_back(getCleanedupPath(arg));
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
				logErrorf(L"Unknown option %ls", arg);
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

const wchar_t* getPadding(const wchar_t* name)
{
	return L"              " + wcslen(name);
}

void addCount(wchar_t* buf, uint offset, uint count)
{
	if (count)
	{
		wchar_t countBuf[32];
		itow(count, countBuf, eacopy_sizeof_array(countBuf));
		uint len = wcslen(countBuf);

		swprintf(buf + offset, L" (%u)%ls", count, L"      " + len);
	}
	else
		wcscat(buf + offset, L"         ");
}

void populateStatsTime(Vector<WString>& stats, const wchar_t* name, u64 ms, uint count)
{
	if (!ms && !count)
		return;
	wchar_t buf[512];
	uint size = swprintf(buf, L"   %ls:%ls%ls", name, getPadding(name), toHourMinSec(ms, 7).c_str());
	addCount(buf, size, count);
	stats.push_back(buf);
}

void populateStatsBytes(Vector<WString>& stats, const wchar_t* name, u64 bytes)
{
	if (!bytes)
		return;
	wchar_t buf[512];
	uint size = swprintf(buf, L"  %ls:%ls%ls", name, getPadding(name), toPretty(bytes, 7).c_str());
	addCount(buf, size, 0);
	stats.push_back(buf);
}
void populateStatsValue(Vector<WString>& stats, const wchar_t* name, float value)
{
	if (!value)
		return;
	wchar_t buf[512];
	uint size = swprintf(buf, L"  %ls:%ls%7.1f", name, getPadding(name), value);
	addCount(buf, size, 0);
	stats.push_back(buf);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[])
{
	using namespace eacopy;
#else
int main(int argc, char* argv_[])
{
	using namespace eacopy;
	wchar_t* argv[64];
	WString temp[64];
	for (int i=0; i!=argc; ++i)
	{
		temp[i] = WString(argv_[i], argv_[i] + strlen(argv_[i]));
		argv[i] = const_cast<wchar_t*>(temp[i].c_str());
	}
#endif

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

	//for (int i=0; i!=argc; ++i)
	//	logDebugLinef(L"CMD: %ls", argv[i]);

	//SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	Log log;
	log.init(settings.logFileName.c_str(), settings.logDebug, false);
	LogContext logContext(log);

	if (settings.printJobHeader)
	{
		logInfoLinef();
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef(L"  EACopy v%hs - File Copy for Windows.   (c) Electronic Arts.  All Rights Reserved.", ClientVersion);
		logInfoLinef(L"-------------------------------------------------------------------------------");
		logInfoLinef();
		logInfoLinef(L"  Source : %ls", settings.sourceDirectory.c_str());
		logInfoLinef(L"    Dest : %ls", settings.destDirectory.c_str());
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
		logInfoLinef(L"   Files:      %7i   %7i   %7i   %7i   %7i   %7i   %7i", totalCount, stats.copyCount, stats.linkCount, stats.skipCount, 0, stats.failCount, stats.createDirCount);
		logInfoLinef(L"   Bytes:     %ls  %ls  %ls  %ls   %7i   %7i   %7i", toPretty(totalSize, 7).c_str(), toPretty(stats.copySize, 7).c_str(), toPretty(stats.linkSize, 7).c_str(), toPretty(stats.skipSize, 7).c_str(), 0, 0, 0);
		logInfoLinef(L"   Times:     %ls  %ls  %ls  %ls  %ls  %ls  %ls", toHourMinSec(totalTimeMs, 7).c_str(), toHourMinSec(stats.copyTimeMs, 7).c_str(), toHourMinSec(stats.linkTimeMs, 7).c_str(), toHourMinSec(stats.skipTimeMs, 7).c_str(), toHourMinSec(0, 7).c_str(), toHourMinSec(0, 7).c_str(), toHourMinSec(stats.ioStats.createDirMs, 7).c_str());

		// Include close in read and write
		stats.ioStats.readMs += stats.ioStats.closeReadMs + stats.ioStats.createReadMs;
		stats.ioStats.writeMs += stats.ioStats.closeWriteMs + stats.ioStats.createWriteMs;

		Vector<WString> statsVec;
		populateStatsTime(statsVec, L"ConnectTime", stats.connectTimeMs, 0);
		populateStatsTime(statsVec, L"FindFile", stats.ioStats.findFileMs, stats.ioStats.findFileCount);
		populateStatsTime(statsVec, L"ReadFile", stats.ioStats.readMs, stats.ioStats.createReadCount);
		populateStatsTime(statsVec, L"WriteFile", stats.ioStats.writeMs, stats.ioStats.createWriteCount);
		populateStatsTime(statsVec, L"LinkFile", stats.ioStats.createLinkMs, stats.ioStats.createLinkCount);
		populateStatsTime(statsVec, L"DeleteFile", stats.ioStats.deleteFileMs, stats.ioStats.deleteFileCount);
		populateStatsTime(statsVec, L"CreateDir", stats.ioStats.createDirMs, stats.ioStats.createDirCount);
		populateStatsTime(statsVec, L"RemoveDir", stats.ioStats.removeDirMs, stats.ioStats.removeDirCount);
		populateStatsTime(statsVec, L"FileInfo", stats.ioStats.fileInfoMs, stats.ioStats.fileInfoCount);
		populateStatsTime(statsVec, L"SetWriteTime", stats.ioStats.setLastWriteTimeMs, stats.ioStats.setLastWriteTimeCount);
		populateStatsTime(statsVec, L"SendFile", stats.sendTimeMs, 0);
		populateStatsBytes(statsVec, L"SendBytes", stats.sendSize);
		populateStatsTime(statsVec, L"RecvFile", stats.recvTimeMs, 0);
		populateStatsBytes(statsVec, L"RecvBytes", stats.recvSize);
		populateStatsTime(statsVec, L"CompressFile", stats.compressTimeMs, 0);
		populateStatsValue(statsVec, L"CompressLevel", stats.compressionAverageLevel);
		populateStatsTime(statsVec, L"DecompreFile", stats.decompressTimeMs, 0);
		populateStatsTime(statsVec, L"DeltaCompress", stats.deltaCompressionTimeMs, 0);
		populateStatsTime(statsVec, L"PurgeDir", stats.purgeTimeMs, 0);

		logInfoLinef();

		for (uint i=0; i<statsVec.size(); i += 2)
		{
			if (i + 1 < statsVec.size())
				logInfoLinef(L"%ls%ls", statsVec[i].c_str(), statsVec[i+1].c_str());
			else
				logInfoLinef(L"%ls", statsVec[i].c_str());
		}
		logInfoLinef();


		if (stats.destServerUsed || stats.sourceServerUsed)
			logInfoLinef(L"   Server found and used!");
		else if (stats.serverAttempt && !stats.destServerUsed)
			logInfoLinef(L"   Server not found (Spent ~%ls trying to connect. Use /NOSERVER to disable attempt)", toHourMinSec(stats.connectTimeMs/std::max(1, (int)settings.threadCount)).c_str());
		else
			logInfoLinef(L"   No server used!");
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
