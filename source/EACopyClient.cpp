// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"
#include <assert.h>
#include <utility>
#if defined(_WIN32)
#define NOMINMAX
#include <ws2tcpip.h>
#include <assert.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <windows.h>
#include <mbctype.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif
#if defined(EACOPY_ALLOW_RSYNC)
#include <EACopyRsync.h>
#endif

#if defined(EACOPY_ALLOW_DELTA_COPY)
#include "EACopyDelta.h"
#endif

#if defined(_WIN32)
#pragma comment (lib, "Shlwapi.lib") // PathMatchSpecW
#endif

#if defined(_WIN32)
#else
#define TIMEVAL timeval
namespace eacopy {
bool PathMatchSpecW(const wchar_t* file, const wchar_t* spec)
{
	if (wcscmp(spec, "*.*") == 0)
		return true;
	EACOPY_NOT_IMPLEMENTED
	return false;
}
enum : int { FindExSearchNameMatch, FindExInfoStandard };
bool RemoveDirectoryW(const wchar_t* dir);
#define WSAHOST_NOT_FOUND                11001L
#define WSA_FLAG_OVERLAPPED           0x01
SOCKET WSASocketW(int af, int type, int protocol, void* lpProtocolInfo, int g, int dwFlags) { EACOPY_NOT_IMPLEMENTED return 0; }
#define WSAEWOULDBLOCK                   10035L
#define GUID Guid
void StringFromGUID2(Guid rguid, wchar_t* lpsz, int cchMax) { EACOPY_NOT_IMPLEMENTED }
#define SD_SEND         0x01
}
#endif

namespace stdargv_c
{
	static void parse_cmdline(const char *cmdstart, char **argv, char *args, int *numargs, int *numchars);
}

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Client::Client(const ClientSettings& settings)
:	m_settings(settings)
{
}

int
Client::process(Log& log)
{
	ClientStats stats;
	return process(log, stats);
}

int
Client::process(Log& log, ClientStats& outStats)
{
	resetWorkState(log);

	m_networkWsaInitDone = false;

	#if defined(_WIN32)
	ScopeGuard wsaCleanup([this]() { if (m_networkWsaInitDone) WSACleanup(); });
	#endif

	m_serverAddrInfo = nullptr;
	ScopeGuard addrCleanup([this]() { if (m_serverAddrInfo) freeAddrInfo(m_serverAddrInfo); });

	LogContext logContext(log);

	const WString& sourceDir = m_settings.sourceDirectory;
	if (sourceDir.size() < 5 || sourceDir[0] != '\\' || sourceDir[1] != '\\')
		m_useSourceServerFailed = true;

	const WString& destDir = m_settings.destDirectory;
	if (destDir.size() < 5 || destDir[0] != '\\' || destDir[1] != '\\')
		m_useDestServerFailed = true;

	// Try to connect to server (can fail to connect and still return true if settings allow it to fail)
	if (!connectToServer(destDir.c_str(), true, m_destConnection, m_useDestServerFailed, outStats))
		return -1;
	ScopeGuard destConnectionCleanup([this]() { delete m_destConnection; m_destConnection = nullptr; });

	#if defined(_WIN32)
	if (m_settings.threadCount > 0)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	#endif

	// Add all link directories to primeQueue
	for (auto& primeDir : m_settings.additionalLinkDirectories)
		m_fileDatabase.primeDirectory(primeDir, outStats.ioStats, m_settings.useLinksRelativePath, false);

	// Spawn worker threads that will copy the files
	struct WorkerThreadData { ClientStats stats; Client* client = nullptr; uint connectionIndex = 0; };
	Vector<WorkerThreadData> workerThreadDataList(m_settings.threadCount);

	Vector<Thread> workerThreadList(m_settings.threadCount);
	for (int i=0; i!=m_settings.threadCount; ++i)
	{
		auto& threadData = workerThreadDataList[i];
		threadData.client = this;
		threadData.connectionIndex = i + 1;
		workerThreadList[i].start([&]() -> int
			{
				return threadData.client->workerThread(threadData.connectionIndex, threadData.stats);
			});
	}

	// Setup guard that will make sure all threads are waited for before leaving method
	ScopeGuard waitThreadsGuard([&]()
	{
		// Wait for all threads to finish
		m_workDone.set();
		for (auto& thread : workerThreadList)
			thread.wait();
	});

	// Connect to source if no destination is set
	if (!m_destConnection)
		if (!connectToServer(m_settings.sourceDirectory.c_str(), true, m_sourceConnection, m_useSourceServerFailed, outStats))
			return -1;
	ScopeGuard sourceConnectionCleanup([&] { delete m_sourceConnection; m_sourceConnection = nullptr; });

	// Collect exclusions provided through file
	for (auto& file : m_settings.filesExcludeFiles)
		if (!excludeFilesFromFile(logContext, outStats, sourceDir, file, destDir))
			return -1;

	// Help out flush out all primed directories
	m_fileDatabase.primeWait(outStats.ioStats);

	if (!m_settings.linkDatabaseFile.empty())
	{
		TimerScope _(outStats.readLinkDbTime);
		m_fileDatabase.readFile(m_settings.linkDatabaseFile.c_str(), outStats.ioStats);
		outStats.readLinkDbEntries = m_fileDatabase.getHistorySize();
	}

	{
		// Traverse through and collect all files that needs copying (worker threads will handle copying). This code will also generate destination directories needed.
		if (!m_settings.filesOrWildcardsFiles.empty())
		{
			CachedFindFileEntries findFileCache;
			for (auto& file : m_settings.filesOrWildcardsFiles)
				if (!gatherFilesOrWildcardsFromFile(logContext, outStats, findFileCache, sourceDir, file, destDir))
					return -1;
			processQueuedWildcardFileEntries(logContext, outStats, findFileCache, sourceDir, destDir);
		}
		else
		{
			for (auto& fileOrWildcard : m_settings.filesOrWildcards)
				if (!traverseFilesInDirectory(logContext, m_sourceConnection, m_destConnection, m_copyContext, sourceDir, destDir, fileOrWildcard, m_settings.copySubdirDepth, outStats))
					return -1;
		}
	}


	// Process dirs and files (worker threads are doing the same right now)
	if (!processQueues(logContext, m_sourceConnection, m_destConnection, m_copyContext, outStats, true))
		return -1;
	

	// Wait for all worker threads to finish
	waitThreadsGuard.execute();

	// If main thread had an error code, return that
	if (int exitCode = logContext.getLastError())
		return exitCode;

	// Go through all threads and see if any of them had an error code.
	for (Thread& wt : workerThreadList)
	{
		uint threadExitCode;
		if (!wt.getExitCode(threadExitCode))
			return -1;
		if (threadExitCode != 0)
			return threadExitCode;
	}

	// If purge feature is enabled.. traverse destination and remove unwanted files/directories
	if (m_settings.purgeDestination)
	{
		TimerScope ps(outStats.purgeTime);
		if (m_createdDirs.find(destDir) == m_createdDirs.end()) // We don't need to purge directories we know we created
			if (!purgeFilesInDirectory(destDir, 0, m_settings.copySubdirDepth, outStats)) // use 0 for directory attribute because we always want to purge root dir even if it is a symlink (which it probably never is)
				return -1;
	}

	// Purge individual directories (can be provided in filelist file)
	for (auto& purgeDir : m_purgeDirs)
	{
		TimerScope ps(outStats.purgeTime);
		if (m_createdDirs.find(purgeDir) == m_createdDirs.end()) // We don't need to purge directories we know we created
		{
			FileInfo dirInfo;
			uint dirAttributes;
			if (isValid(m_destConnection))
			{
				uint error = 0;
				if (!m_destConnection->sendGetFileAttributes(purgeDir.c_str(), dirInfo, dirAttributes, error))
					return -1;
				if (error)
					return -1;
			}
			else
				dirAttributes = getFileInfo(dirInfo, purgeDir.c_str(), outStats.ioStats);

			if (!purgeFilesInDirectory(purgeDir.c_str(), dirAttributes, m_settings.copySubdirDepth, outStats))
				return -1;
		}
	}

	sourceConnectionCleanup.execute();
	destConnectionCleanup.execute();

	if (!m_settings.linkDatabaseFile.empty())
	{
		TimerScope _(outStats.writeLinkDbTime);
		m_fileDatabase.writeFile(m_settings.linkDatabaseFile.c_str(), outStats.ioStats);
		outStats.writeLinkDbEntries = m_fileDatabase.getHistorySize();
	}

	// Merge stats from all threads
	for (auto& threadData : workerThreadDataList)
	{
		auto& threadStats = threadData.stats;
		outStats.copyCount += threadStats.copyCount;
		outStats.copySize += threadStats.copySize;
		outStats.skipCount += threadStats.skipCount;
		outStats.skipSize += threadStats.skipSize;
		outStats.linkCount += threadStats.linkCount;
		outStats.linkSize += threadStats.linkSize;
		outStats.serverAttempt |= threadStats.serverAttempt;
		outStats.processedByServerCount += threadStats.processedByServerCount;

		outStats.copyTime = max(outStats.copyTime, threadStats.copyTime);
		outStats.skipTime = max(outStats.skipTime, threadStats.skipTime);
		outStats.linkTime = max(outStats.linkTime, threadStats.linkTime);
		outStats.purgeTime = max(outStats.purgeTime, threadStats.purgeTime);

		outStats.createDirCount += threadStats.createDirCount;
		outStats.compressTime += threadStats.compressTime;
		outStats.deltaCompressionTime += threadStats.deltaCompressionTime;
		outStats.sendTime += threadStats.sendTime;
		outStats.sendSize += threadStats.sendSize;
		outStats.recvTime += threadStats.recvTime;
		outStats.recvSize += threadStats.recvSize;
		outStats.compressionLevelSum += threadStats.compressionLevelSum;
		outStats.failCount += threadStats.failCount;
		outStats.retryCount += threadStats.retryCount;
		outStats.retryTime += threadStats.retryTime;
		outStats.connectTime += threadStats.connectTime;
		outStats.hashCount += threadStats.hashCount;
		outStats.hashTime += threadStats.hashTime;
		outStats.netSecretGuid += threadStats.netSecretGuid;
		for (uint i=0;i!=WriteResponseCount; ++i)
		{
			outStats.netWriteResponseTime[i] += threadStats.netWriteResponseTime[i];
			outStats.netWriteResponseCount[i] += threadStats.netWriteResponseCount[i];
		}
		outStats.netFindFilesTime += threadStats.netFindFilesTime;
		outStats.netFindFilesCount += threadStats.netFindFilesCount;
		outStats.netCreateDirTime += threadStats.netCreateDirTime;
		outStats.netCreateDirCount += threadStats.netCreateDirCount;
		outStats.netFileInfoTime += threadStats.netFileInfoTime;
		outStats.netFileInfoCount += threadStats.netFileInfoCount;
		outStats.ioStats.createReadTime += threadStats.ioStats.createReadTime;
		outStats.ioStats.closeReadTime += threadStats.ioStats.closeReadTime;
		outStats.ioStats.closeReadCount += threadStats.ioStats.closeReadCount;
		outStats.ioStats.readTime += threadStats.ioStats.readTime;
		outStats.ioStats.readCount += threadStats.ioStats.readCount;
		outStats.ioStats.createReadCount += threadStats.ioStats.createReadCount;
		outStats.ioStats.createWriteTime += threadStats.ioStats.createWriteTime;
		outStats.ioStats.createWriteCount += threadStats.ioStats.createWriteCount;
		outStats.ioStats.closeWriteTime += threadStats.ioStats.closeWriteTime;
		outStats.ioStats.closeWriteCount += threadStats.ioStats.closeWriteCount;
		outStats.ioStats.writeTime += threadStats.ioStats.writeTime;
		outStats.ioStats.writeCount += threadStats.ioStats.writeCount;
		outStats.ioStats.removeDirTime += threadStats.ioStats.removeDirTime;
		outStats.ioStats.removeDirCount += threadStats.ioStats.removeDirCount;
		outStats.ioStats.deleteFileTime += threadStats.ioStats.deleteFileTime;
		outStats.ioStats.deleteFileCount += threadStats.ioStats.deleteFileCount;
		outStats.ioStats.moveFileTime += threadStats.ioStats.moveFileTime;
		outStats.ioStats.moveFileCount += threadStats.ioStats.moveFileCount;
		outStats.ioStats.createLinkTime += threadStats.ioStats.createLinkTime;
		outStats.ioStats.createLinkCount += threadStats.ioStats.createLinkCount;
		outStats.ioStats.setLastWriteTime += threadStats.ioStats.setLastWriteTime;
		outStats.ioStats.setLastWriteTimeCount += threadStats.ioStats.setLastWriteTimeCount;
		outStats.ioStats.findFileTime += threadStats.ioStats.findFileTime;
		outStats.ioStats.findFileCount += threadStats.ioStats.findFileCount;
		outStats.ioStats.fileInfoTime += threadStats.ioStats.fileInfoTime;
		outStats.ioStats.fileInfoCount += threadStats.ioStats.fileInfoCount;
		outStats.ioStats.createDirCount += threadStats.ioStats.createDirCount;
		outStats.ioStats.createDirTime += threadStats.ioStats.createDirTime;
		outStats.ioStats.copyFileCount += threadStats.ioStats.copyFileCount;
		outStats.ioStats.copyFileTime += threadStats.ioStats.copyFileTime;
	}

	outStats.compressionAverageLevel = outStats.copySize ? (float)((double)outStats.compressionLevelSum / outStats.copySize) : 0;

	outStats.destServerUsed =  m_settings.useServer != UseServer_Disabled && !m_useDestServerFailed;
	outStats.sourceServerUsed =  m_settings.useServer != UseServer_Disabled && !m_useSourceServerFailed;

	// Success!
	return 0;
}

int
Client::reportServerStatus(Log& log)
{
	resetWorkState(log);

	LogContext logContext(log);

	bool failedToConnect = false;
	ClientStats stats;

	// Create connection
	Connection* connection = createConnection(m_settings.destDirectory.c_str(), 0, stats, failedToConnect, false);
	if (!connection && !failedToConnect)
	{
		logErrorf(L"Failed to connect to server. Is path '%ls' a proper smb path?", m_settings.destDirectory.c_str());
		return -1;
	}
	ScopeGuard connectionGuard([&]() { delete connection; });

	// Send request report command
	RequestReportCommand cmd;
	cmd.commandType = CommandType_RequestReport;
	cmd.commandSize = sizeof(cmd);
	if (!connection->sendCommand(cmd))
		return -1;

	// Read report from server and log it out
	uint dataSize;
	if (!receiveData(connection->m_socket, &dataSize, sizeof(dataSize)))
		return -1;

	Vector<wchar_t> buffer;
	buffer.resize(dataSize + 1);
	if (!receiveData(connection->m_socket, buffer.data(), dataSize*2))
		return -1;

	buffer[dataSize] = 0;
	
	// Print report
	logInfo(buffer.data());
	return 0;
}

void
Client::resetWorkState(Log& log)
{
	m_log = &log;
	m_useSourceServerFailed = false;
	m_useDestServerFailed = false;
	m_workDone.reset();
	m_tryCopyFirst = true;
	m_networkInitDone = false;
	m_networkServerName.clear();
	m_copyEntries.clear();
	m_handledFiles.clear();
	m_createdDirs.clear();
	m_sourceConnection = nullptr;
	m_destConnection = nullptr;
	m_secretGuid = {0};

	m_processDirActive = 0;

	// These are used for when sending files to server with compression enabled
	m_compressionStats.fixedLevel = m_settings.compressionLevel != 255;
	m_compressionStats.currentLevel = std::min<u8>(std::max<u8>(m_settings.compressionLevel, 1), 22);
}

bool
Client::processDir(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats)
{
	// Pop first entry off the queue (and increase active count to point out that something is being processed)
	DirEntry entry;
	m_dirEntriesCs.scoped([&]()
		{
			if (!m_dirEntries.empty())
			{
				entry = std::move(m_dirEntries.front());
				m_dirEntries.pop_front();
				++m_processDirActive;
			}
		});

	// If no new entry queued
	if (entry.destDir.empty())
		return false;


	traverseFilesInDirectory(logContext, sourceConnection, destConnection, copyContext, entry.sourceDir, entry.destDir, entry.wildcard, entry.depthLeft, stats);

	m_dirEntriesCs.scoped([&]() { --m_processDirActive; } );

	return true;
}

bool
Client::processFile(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats)
{
	// Pop first entry off the queue
	CopyEntry entry;
	m_copyEntriesCs.scoped([&]()
		{
			if (!m_copyEntries.empty())
			{
				entry = std::move(m_copyEntries.front());
				m_copyEntries.pop_front();
			}
		});

	// If no new entry queued, sleep a bit and return in order to try again
	if (entry.src.empty())
	{
		Sleep(1);
		return false;
	}

	bool useLinks = entry.srcInfo.fileSize >= m_settings.useLinksThreshold;

	// Get full destination path
	WString fullDst = m_settings.destDirectory + entry.dst;
	 
	// Try to copy file
	int retryCountLeft = m_settings.retryCount;
	while (true)
	{

		u64 startTime = getTime();

		auto reportSkip = [&]()
		{
			if (m_settings.logProgress)
				logInfoLinef(L"Skip File   %ls", getRelativeSourceFile(entry.src));
			stats.skipTime += getTime() - startTime;
			++stats.skipCount;
			stats.skipSize += entry.srcInfo.fileSize;
		};

		if (useLinks)
		{
			FileKey key{ getFileKeyPath(entry.dst), entry.srcInfo.lastWriteTime, entry.srcInfo.fileSize }; // Robocopy style key for uniqueness of file
			FileDatabase::FileRec dbFile = m_fileDatabase.getRecord(key);
			if (!dbFile.name.empty())
			{
				FileInfo dbFileInfo;
				uint attributes = getFileInfo(dbFileInfo, dbFile.name.c_str(), stats.ioStats);
				bool isValidSource = attributes && equals(entry.srcInfo, dbFileInfo);

				if (isValidSource)
				{
					// Link happens to be the same as destination and up-to-date, skip it
					if (dbFile.name == fullDst)
					{
						reportSkip();
						m_fileDatabase.addToFilesHistory(key, dbFile.hash, fullDst); // Touch db
						return true;
					}

					bool skip = false;
					bool deleteAndRetry = false; // I don't like this default but there is currently a race condition on our build farm where two machines copy same file to same destination and deleting the file cause some problems
					if (createFileLink(fullDst.c_str(), entry.srcInfo, dbFile.name.c_str(), skip, stats.ioStats, deleteAndRetry))
					{
						if (skip)
						{
							reportSkip();
						}
						else
						{
							if (m_settings.logProgress)
								logInfoLinef(L"Link File   %ls", getRelativeSourceFile(entry.src));
							stats.linkTime += getTime() - startTime;
							++stats.linkCount;
							stats.linkSize += entry.srcInfo.fileSize;
						}

						m_fileDatabase.addToFilesHistory(key, dbFile.hash, fullDst);
						return true;
					}
					else
					{
						logContext.resetLastError(); // We want to handle failing links as non-error and fallback to normal copying
						useLinks = false; // and do not try linking again if copy also fails.. just let copy retry
					}
				}
				else
				{
					// Remove from fileDatabase.. since file at location has either changed or does not exist anymore and we don't want to look here again
					m_fileDatabase.removeFileHistory(key);
				}
			}
		}

		if (m_settings.useOdx) // Try to use ODX (use system copy call using previous destination as source expecting the system to optimize the copy)
		{
			FileKey key{ getFileKeyPath(entry.dst), entry.srcInfo.lastWriteTime, entry.srcInfo.fileSize };
			FileDatabase::FileRec dbFile = m_fileDatabase.getRecord(key);
			if (!dbFile.name.empty())
			{
				FileInfo dbFileInfo;
				uint attributes = getFileInfo(dbFileInfo, dbFile.name.c_str(), stats.ioStats);
				bool isValidSource = attributes && equals(entry.srcInfo, dbFileInfo); // Check so file is still valid
				if (isValidSource)
				{
					bool useSystemCopy = true; // Must use system copy for odx to potentially work
					bool existed = false;
					u64 written;
					bool failIfExists = m_settings.excludeChangedFiles;
					if (copyFile(dbFile.name.c_str(), entry.srcInfo, attributes, fullDst.c_str(), useSystemCopy, failIfExists, existed, written, copyContext, stats.ioStats, m_settings.useBufferedIO))
					{
						stats.copyTime += getTime() - startTime;
						++stats.copyCount;
						stats.copySize += written;

						m_fileDatabase.addToFilesHistory(key, dbFile.hash, fullDst);
						return true;
					}
					else
						logContext.resetLastError(); // We want to handle failing odx copy as non-error and fallback to normal copying
				}
			}
		}

		// Use connection to server if available
		if (isValid(destConnection))
		{
			u64 size;
			u64 written;
			bool linked;
			bool processedByServer;

			// Send file to server (might be skipped if server already has it).. returns false if it fails
			if (destConnection->sendWriteFileCommand(entry.src.c_str(), entry.dst.c_str(), entry.srcInfo, entry.attributes, size, written, linked, copyContext, processedByServer))
			{
				if (written)
				{
					if (m_settings.logProgress)
						logInfoLinef(L"%ls   %ls", linked ? L"Link File" : L"New File ", getRelativeSourceFile(entry.src));
					(linked ? stats.linkTime : stats.copyTime) += getTime() - startTime;
					++(linked ? stats.linkCount : stats.copyCount);
					(linked ? stats.linkSize : stats.copySize) += written;
				}
				else
				{
					reportSkip();
				}
				if (processedByServer)
				{
					++stats.processedByServerCount;
				}
				return true;
			}
		}
		else if (isValid(sourceConnection))
		{
			u64 size;
			u64 read;
			bool processedByServer;
			switch (sourceConnection->sendReadFileCommand(entry.src.c_str(), entry.dst.c_str(), entry.srcInfo, entry.attributes, size, read, copyContext, processedByServer))
			{
			case Connection::ReadFileResult_Success:
				if (read)
				{
					if (m_settings.logProgress)
						logInfoLinef(L"%ls   %ls", L"New File ", getRelativeSourceFile(entry.src));
					stats.copyTime += getTime() - startTime;
					++stats.copyCount;
					stats.copySize += size;
				}
				else
				{
					if (m_settings.logProgress)
						logInfoLinef(L"Skip File   %ls", getRelativeSourceFile(entry.src));
					stats.skipTime += getTime() - startTime;
					++stats.skipCount;
					stats.skipSize += size;
				}
				if (processedByServer)
				{
					++stats.processedByServerCount;
				}
				return true;

			case Connection::ReadFileResult_Error:
				return false;

			case Connection::ReadFileResult_ServerBusy:	// Server was busy, return entry in to queue and take a long break (this should never happen on mainthread)
				m_copyEntriesCs.scoped([&]() { m_copyEntries.push_front(entry); });
				m_workDone.isSet(5*1000);
				return true;
			}
		}
		else
		{
			auto addToDatabase = [&]()
			{
				if (useLinks)
				{
					FileKey key{ getFileKeyPath(entry.dst), entry.srcInfo.lastWriteTime, entry.srcInfo.fileSize }; // Robocopy style key for uniqueness of file
					m_fileDatabase.addToFilesHistory(key, Hash(), fullDst);
				}
			};

			bool useSystemCopy = m_settings.useSystemCopy || (m_settings.useOdx && !isLocalPath(m_settings.destDirectory.c_str()) && !isLocalPath(m_settings.sourceDirectory.c_str()));
			bool tryCopyFirst = m_tryCopyFirst;

			bool existed = false;
			u64 written;

			// Try to copy file first without checking if it is there (we optimize for copying new files)
			if (tryCopyFirst)
			{
				bool failIfExists = true;
				if (copyFile(entry.src.c_str(), entry.srcInfo, entry.attributes, fullDst.c_str(), useSystemCopy, failIfExists, existed, written, copyContext, stats.ioStats, m_settings.useBufferedIO))
				{
					if (m_settings.logProgress)
						logInfoLinef(L"New File    %ls", getRelativeSourceFile(entry.src));
					stats.copyTime += getTime() - startTime;
					++stats.copyCount;
					stats.copySize += written;

					addToDatabase();
					return true;
				}

				if (m_settings.excludeChangedFiles)
				{
					if (existed) // If failure was because file existed, then we're good and can skip
					{
						addToDatabase();
						reportSkip();
						return true;
					}
				}
				else
				{
					// Stop trying to copy first since we found something in target directory and there most likely are more (it is ok this is not thread safe.. it is just an optimization that can take effect later)
					// A failed copy is way more expensive that a failed "getFileInfo" so this change is to stop trying to copy first
					m_tryCopyFirst = false;
				}
			}

			// Handle scenario of failing to copy because target existed or we never tried copy it
			if (existed || !tryCopyFirst)
			{
				FileInfo destInfo;
				uint fileAttributes = getFileInfo(destInfo, fullDst.c_str(), stats.ioStats);

				// If no file attributes it might be that the file doesnt exist
				if (!fileAttributes)
				{
					if (m_settings.logProgress)
						logDebugLinef(L"Failed to get attributes from file %ls", fullDst.c_str());
				}
				else if (!m_settings.forceCopy && equals(entry.srcInfo, destInfo)) // Skip file if the same
				{
					if (m_settings.logProgress)
						logInfoLinef(L"Skip File   %ls", getRelativeSourceFile(entry.src));
					stats.skipTime += getTime() - startTime;
					++stats.skipCount;
					stats.skipSize += destInfo.fileSize;

					addToDatabase();
					return true;
				}

				// if destination file is read-only then we will clear that flag so the copy can succeed
				if (fileAttributes & FILE_ATTRIBUTE_READONLY)
				{
					if (!setFileWritable(fullDst.c_str(), true))
						logErrorf(L"Could not copy over read-only destination file (%ls).  EACopy could not forcefully unset the destination file's read-only attribute.", fullDst.c_str());
				}
				
				if (copyFile(entry.src.c_str(), entry.srcInfo, entry.attributes, fullDst.c_str(), useSystemCopy, false, existed, written, copyContext, stats.ioStats, m_settings.useBufferedIO))
				{
					if (m_settings.logProgress)
						logInfoLinef(L"New File    %ls", getRelativeSourceFile(entry.src));

					stats.copyTime += getTime() - startTime;
					++stats.copyCount;
					stats.copySize += written;

					addToDatabase();
					return true;
				}
			}
		}

		if (retryCountLeft-- == 0)
		{
			++stats.failCount;
			logErrorf(L"failed to copy file (%ls)", entry.src.c_str());
			return true;
		}

		// Reset last error and try again!
		logContext.resetLastError();
		logInfoLinef(L"Warning - failed to copy file %ls to %ls, retrying in %i seconds", entry.src.c_str(), fullDst.c_str(), m_settings.retryWaitTimeMs/1000);
		Sleep(m_settings.retryWaitTimeMs);

		++stats.retryCount;
		stats.retryTime += getTime() - startTime;
	}

	return true;
}

bool
Client::connectToServer(const wchar_t* networkPath, uint connectionIndex, Connection*& outConnection, bool& failedToConnect, ClientStats& stats)
{
	outConnection = nullptr;
	if (m_settings.useServer == UseServer_Disabled || failedToConnect)
		return true;

	// Set temporary log context to swallow potential errors when trying to connect
	LogContext logContext(*m_log);

	u64 startConnect = getTime();
	stats.serverAttempt = true;
	outConnection = createConnection(networkPath, connectionIndex, stats, failedToConnect, true);
	stats.connectTime += getTime() - startConnect;

	if (failedToConnect && m_settings.useServer == UseServer_Required)
	{
		logErrorf(L"Failed to connect to server hosting %ls at port %u", networkPath, m_settings.serverPort);
		return false;
	}
	return true;
}

bool
Client::processQueues(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats, bool isMainThread)
{
	logDebugLinef(L"Worker started");

	IOStats ioStats;
	uint filesProcessedCount = 0;

	// Process file queue
	while (!m_workDone.isSet(0))
	{
		if (m_fileDatabase.primeUpdate(stats.ioStats))
			continue;
		if (processDir(logContext, sourceConnection, destConnection, copyContext, stats))
			continue;
		if (processFile(logContext, sourceConnection, destConnection, copyContext, stats))
		{
			++filesProcessedCount;
			continue;
		}

		// If this is the main thread we check if we can leave processing
		if (!isMainThread)
			continue;

		{
			// If there are no active dir processing in any thread plus dir entries are empty there is no way to add more file entries. (all calls are protected by locks)
			ScopedCriticalSection cs(m_dirEntriesCs);
			if (m_processDirActive || !m_dirEntries.empty())
				continue;
		}

		// If there are still copy entries left, keep helping out. 
		// We can only end up here if dir processing is _fully_ done...
		// but another thread might just have added the last file entries and m_dirEntries were empty and m_processDirActive was 0 in the check above
		ScopedCriticalSection cs2(m_copyEntriesCs);
		if (!m_copyEntries.empty())
			continue;

		break;
	}

	logDebugLinef(L"Worker done - %u file(s) processed", filesProcessedCount);

	return true;
}

int
Client::workerThread(uint connectionIndex, ClientStats& stats)
{
	// Try to connect to server (can fail to connect and still return true if settings allow it to fail)
	Connection* destConnection;
	if (!connectToServer(m_settings.destDirectory.c_str(), connectionIndex, destConnection, m_useDestServerFailed, stats))
		return false;
	ScopeGuard destConnectionGuard([&] { delete destConnection; });

	Connection* sourceConnection = nullptr;
	if (!destConnection)
		if (!connectToServer(m_settings.sourceDirectory.c_str(), connectionIndex, sourceConnection, m_useSourceServerFailed, stats))
			return false;
	ScopeGuard sourcesConnectionGuard([&] { delete sourceConnection; });


	// Help process the files
	LogContext logContext(*m_log);
	NetworkCopyContext copyContext;
	processQueues(logContext, sourceConnection, destConnection, copyContext, stats, false);

	return logContext.getLastError();
}

bool
Client::addDirectoryToHandledFiles(LogContext& logContext, Connection* destConnection, const WString& destFullPath, uint attributes, ClientStats& stats)
{
	WString destFile = destFullPath.c_str() + m_settings.destDirectory.size();

	uint lastSlashIndex = destFile.find_last_of(L'\\');
	if (lastSlashIndex != WString::npos)
	{
		bool first = true;
		WString destPath(destFile, 0, lastSlashIndex+1);
		while (true)
		{
			ScopedCriticalSection cs(m_handledFilesCs); // Need to cover entire thing to make sure directory is always created

			if (!m_handledFiles.insert(destPath).second)
				break;
			if (first)
			{
				WString destFullPath2(destFullPath);
				destFullPath2.resize(destFullPath2.find_last_of(L'\\') + 1);
				int retryCount = m_settings.retryCount;
				while (true)
				{
					if (ensureDirectory(destConnection, destFullPath2.c_str(), attributes, stats.ioStats))
						break;

					if (retryCount-- == 0)
						return false;

					// Reset last error and try again!
					logContext.resetLastError();
					TimerScope _(stats.retryTime);
					logInfoLinef(L"Warning - Failed to create directory %ls, retrying in %i seconds", destFullPath2.c_str(), m_settings.retryWaitTimeMs/1000);
					Sleep(m_settings.retryWaitTimeMs);
					++stats.retryCount;
				}
				++stats.createDirCount;
			}
			first = false;
			if (destPath.empty())
				break;
			destPath.resize(destPath.size()-1);
			lastSlashIndex = destPath.find_last_of(L'\\');
			if (lastSlashIndex == WString::npos)
				break;
			destPath.resize(lastSlashIndex + 1);
		}

	}
	return true;
}

bool
Client::handleFile(LogContext& logContext, Connection* destConnection, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const FileInfo& fileInfo, uint attributes, ClientStats& stats)
{
	const wchar_t* destFileName = fileName;

	const wchar_t* lastSlash = wcsrchr(fileName, L'\\');

	// If destination is flatten we remove relative path
	if (m_settings.flattenDestination)
		if (lastSlash)
			destFileName = lastSlash + 1;

	WString destFullPath = destPath + destFileName;

	// Check if file should be excluded because of wild cards
	for (auto& excludeWildcard : m_settings.excludeWildcards)
		if (PathMatchSpecW(destFullPath.c_str(), excludeWildcard.c_str()))
			return true;

	// This is the path of the dest file relative root directory
	WString destFile = destFullPath.c_str() + m_settings.destDirectory.size();

	// Keep track of handled files so we don't do duplicated work
	{
		ScopedCriticalSection cs(m_handledFilesCs);
		if (!m_handledFiles.insert(destFile).second)
			return true;
	}

	FileInfo srcDirInfo;
	uint srcDirAttributes = getFileInfo(srcDirInfo, sourcePath.c_str(), stats.ioStats);

	if (!addDirectoryToHandledFiles(logContext, destConnection, destFullPath, srcDirAttributes, stats))
		return false;
	WString srcFile = sourcePath + fileName;

	// Add entry (workers will pick this up as soon as possible )
	ScopedCriticalSection cs(m_copyEntriesCs);
	m_copyEntries.push_back(CopyEntry());
	auto& entry = m_copyEntries.back();
	entry.src = srcFile;
	entry.dst = destFile;
	entry.srcInfo = fileInfo;
	entry.attributes = attributes;
	return true;
}

bool
Client::handleDirectory(LogContext& logContext, Connection* destConnection, const WString& sourcePath, const WString& destPath, const wchar_t* directory, const wchar_t* wildcard, int depthLeft, ClientStats& stats)
{
	if (isIgnoredDirectory(directory))
 		return true;

	WString newSourceDirectory = sourcePath + directory + L'\\';
	WString newDestDirectory = destPath;
	if (!m_settings.flattenDestination && *directory)
		newDestDirectory = newDestDirectory + directory + L'\\';
	
	if (m_settings.copyEmptySubdirectories)
	{
		FileInfo srcDirInfo;
		uint srcDirAttributes = getFileInfo(srcDirInfo, newSourceDirectory.c_str(), stats.ioStats);

		if (!addDirectoryToHandledFiles(logContext, destConnection, newDestDirectory, srcDirAttributes, stats))
			return false;
	}

	ScopedCriticalSection cs(m_dirEntriesCs);
	m_dirEntries.push_back(DirEntry());
	DirEntry& dirEntry = m_dirEntries.back();
	dirEntry.sourceDir = newSourceDirectory;
	dirEntry.destDir = newDestDirectory;
	dirEntry.wildcard = wildcard;
	dirEntry.depthLeft = depthLeft;
	return true;
}

bool
Client::handleMissingFile(const wchar_t* fileName)
	{
	for (auto& optionalWildcard : m_settings.optionalWildcards)
		if (PathMatchSpecW(fileName, optionalWildcard.c_str()))
			return true;
	for (auto& excludeWildcard : m_settings.excludeWildcards)
		if (PathMatchSpecW(fileName, excludeWildcard.c_str()))
			return true;
	ScopedCriticalSection cs(m_handledFilesCs);
	if (m_handledFiles.find(fileName) != m_handledFiles.end())
		return true;
	return false;
}

bool
Client::handlePath(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName)
{
	WString fullFileName = sourcePath;
	if (fileName[0])
		fullFileName += fileName;

	uint attributes = 0;
	FileInfo fileInfo;

	{
		int retryCount = m_settings.retryCount;
		while (true)
		{
			uint error = 0;

			// Get file attributes (and allow retry if fails)
			if (isValid(sourceConnection))
			{
				if (!sourceConnection->sendGetFileAttributes(fileName, fileInfo, attributes, error))
					return false;
				if (!error)
					break;
			}
			else
			{
				FileInfo temp;
				if (uint attr = getFileInfo(temp, fullFileName.c_str(), stats.ioStats))
				{
					fileInfo = temp;
					attributes = attr;
					break;
				}
				error = GetLastError();
			}

			wchar_t errorDesc[1024];

			if (ERROR_FILE_NOT_FOUND == error || ERROR_PATH_NOT_FOUND == error)
			{
				if (handleMissingFile(fileName))
					return true;
				StringCbPrintfW(errorDesc, eacopy_sizeof_array(errorDesc), L"Can't find file/directory %ls", fullFileName.c_str());
			}
			else
				StringCbPrintfW(errorDesc, eacopy_sizeof_array(errorDesc), L"%ls getting attributes from file/directory %ls", getErrorText(error).c_str(), fullFileName.c_str());

			if (retryCount-- == 0)
			{
				++stats.failCount;
				logErrorf(errorDesc);
				return true;
			}

			// Reset last error and try again!
			logContext.resetLastError();
			TimerScope _(stats.retryTime);
			logInfoLinef(L"Warning - %ls, retrying in %i seconds", errorDesc, m_settings.retryWaitTimeMs/1000);
			Sleep(m_settings.retryWaitTimeMs);

			++stats.retryCount;
		}
	}

	return handlePath(logContext, sourceConnection, destConnection, stats, sourcePath, destPath, fileName, attributes, fileInfo);
}

bool
Client::handlePath(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, uint attributes, const FileInfo& fileInfo)
{
	// Handle directory
	if (attributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		if (!handleDirectory(logContext, destConnection, sourcePath, destPath, fileName, L"*.*", m_settings.copySubdirDepth, stats))
			return false;
	}
	else //if (attributes & FILE_ATTRIBUTE_NORMAL) // Handle file
	{
		// This can happen in certain scenarios when wildcard files are used. The file is actually part of the source path
		if (!*fileName)
		{
			const wchar_t* lastSlash = wcsrchr(sourcePath.c_str(), L'\\');
			if (!lastSlash)
			{
				logErrorf(L"Something went wrong with the file paths. Source: %ls Dest: %ls", sourcePath.c_str(), destPath.c_str());
				return false;
			}

			WString newSourcePath(sourcePath.c_str(), lastSlash + 1);
			fileName = lastSlash + 1;
			if (!handleFile(logContext, destConnection, newSourcePath, destPath, fileName, fileInfo, attributes, stats))
				return false;
		}
		else
		{
			if (!handleFile(logContext, destConnection, sourcePath, destPath, fileName, fileInfo, attributes, stats))
				return false;
		}
	}

	return true;
}

bool
Client::traverseFilesInDirectory(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, const WString& sourcePath, const WString& destPath, const WString& wildcard, int depthLeft, ClientStats& stats)
{
	if (isValid(sourceConnection))
	{
		WString relPath;
		if (sourcePath.size() > m_settings.sourceDirectory.size())
			relPath.append(sourcePath.c_str() + m_settings.sourceDirectory.size());

		WString searchStr = relPath + wildcard;

		Vector<NameAndFileInfo> files;
		if (!sourceConnection->sendFindFiles(searchStr.c_str(), files, copyContext))
			return false;
		for (auto& file : files)
		{
			if(!(file.attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				if (!handleFile(logContext, destConnection, sourcePath, destPath, file.name.c_str(), file.info, file.attributes, stats))
					return false;
			}
		}

		// Handle directories separately
		Vector<NameAndFileInfo> directories;
		WString dirSearchStr = relPath + L"*.*";
		if (!sourceConnection->sendFindFiles(dirSearchStr.c_str(), directories, copyContext))
			return false;
		for (auto& directory : directories)
		{
			if ((directory.attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				if (depthLeft)
				{
					if (!handleDirectory(logContext, destConnection, sourcePath, destPath, directory.name.c_str(), wildcard.c_str(), depthLeft - 1, stats))
						return false;
				}
			}
		}
	}
	else
	{
		WString searchStr = sourcePath;
		if (wildcard.find('*') == std::string::npos)
			searchStr += wildcard;
		else
			searchStr += L"*.*";

		FindFileData fd; 
		FindFileHandle findFileHandle; 

		int retryCount = m_settings.retryCount;
		while (true)
		{
			findFileHandle = findFirstFile(searchStr.c_str(), fd, stats.ioStats); 
			if (findFileHandle != InvalidFindFileHandle)
				break;

			uint findFileError = GetLastError();

			// If path is wildcard and file is not found it is still fine.. just skip find next
			if (findFileError == ERROR_FILE_NOT_FOUND)
			{
				if (wildcard.find('*') != std::string::npos)
					break;
				logErrorf(L"Can't find file %ls", searchStr.c_str());
			}
			else
				logErrorf(L"FindFirstFile %ls failed: %ls", searchStr.c_str(), getErrorText(findFileError).c_str());

			if (retryCount-- == 0)
				return false;

			// Reset last error and try again!
			logContext.resetLastError();
			TimerScope _(stats.retryTime);
			logInfoLinef(L"Warning - FindFirstFile %ls failed, retrying in %i seconds", searchStr.c_str(), m_settings.retryWaitTimeMs/1000);
			Sleep(m_settings.retryWaitTimeMs);
			++stats.retryCount;
		}

		//Handle all the files first
		if (findFileHandle != InvalidFindFileHandle)
		{
			ScopeGuard _([&]() { findClose(findFileHandle, stats.ioStats); });

			do
			{
				FileInfo fileInfo;
				uint fileAttr = getFileInfo(fileInfo, fd);

				if (!(fileAttr & FILE_ATTRIBUTE_DIRECTORY))
				{
					if (!isFileWithAttributeAllowed(fileAttr))
						continue;

					wchar_t* fileName = getFileName(fd);
					if (PathMatchSpecW(fileName, wildcard.c_str()))
						if (!handleFile(logContext, destConnection, sourcePath, destPath, getFileName(fd), fileInfo, fileAttr, stats))
							return false;
				}
				else //if (wildcardIncludesAll)
				{
					if (depthLeft)
					{
						const wchar_t* dirName = getFileName(fd);
						if (!isDotOrDotDot(dirName))
							if (!handleDirectory(logContext, destConnection, sourcePath, destPath, dirName, wildcard.c_str(), depthLeft - 1, stats))
								return false;
					}
				}
			}
			while (findNextFile(findFileHandle, fd, stats.ioStats));

			uint findNextError = GetLastError();
			if (findNextError != ERROR_NO_MORE_FILES)
			{
				logErrorf(L"FindNextFileW failed for %ls: %ls", searchStr, getErrorText(findNextError).c_str());
				return false;
			}
		}
	}
	return true;
}

bool
Client::findFilesInDirectory(Vector<NameAndFileInfo>& outEntries, LogContext& logContext, Connection* connection, NetworkCopyContext& copyContext, const WString& path, ClientStats& stats)
{
	if (isValid(connection))
	{
		WString relPath;
		if (path.size() > m_settings.sourceDirectory.size())
			relPath.append(path.c_str() + m_settings.sourceDirectory.size());
		WString searchStr = relPath + L"*.*";
		return connection->sendFindFiles(searchStr.c_str(), outEntries, copyContext);
	}
	else
	{
		WString searchStr = path;
		searchStr += L"*.*";

		FindFileData fd; 
		FindFileHandle findFileHandle; 

		int retryCount = m_settings.retryCount;
		while (true)
		{
			findFileHandle = findFirstFile(searchStr.c_str(), fd, stats.ioStats);
			if (findFileHandle != InvalidFindFileHandle)
				break;

			uint findFileError = GetLastError();

			// If path is wildcard and file is not found it is still fine.. just skip find next
			if (findFileError != ERROR_FILE_NOT_FOUND)
				logErrorf(L"FindFirstFile %ls failed: %ls", searchStr.c_str(), getErrorText(findFileError).c_str());

			if (retryCount-- == 0)
				return false;

			// Reset last error and try again!
			logContext.resetLastError();
			logInfoLinef(L"Warning - FindFirstFile %ls failed, retrying in %i seconds", searchStr.c_str(), m_settings.retryWaitTimeMs/1000);
			Sleep(m_settings.retryWaitTimeMs);
			++stats.retryCount;
		}

		if (findFileHandle == InvalidFindFileHandle)
			return true;

		ScopeGuard _([&]() { findClose(findFileHandle, stats.ioStats); });

		do
		{
			FileInfo fileInfo;
			uint attr = getFileInfo(fileInfo, fd);
			bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY);
			if (!isDir && !isFileWithAttributeAllowed(attr))
				continue;
			const wchar_t* fileName = getFileName(fd);
			if (isDir && isDotOrDotDot(fileName))
				continue;
			outEntries.push_back({fileName, fileInfo, attr});
		}
		while (findNextFile(findFileHandle, fd, stats.ioStats));

		uint findNextError = GetLastError();
		if (findNextError != ERROR_NO_MORE_FILES)
		{
			logErrorf(L"FindNextFileW failed for %ls: %ls", searchStr.c_str(), getErrorText(findNextError).c_str());
			return false;
		}
	}
	return true;
}

bool
Client::handleFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileOrWildcardFunc& func)
{
	int retryCount = m_settings.retryCount;
	uint handledLineCount = 0;
	bool isFirstRun = true;
	bool tryUseSourceConnection = false;
	WString originalFullPath;
	if (isAbsolutePath(fileName.c_str()))
	{
		originalFullPath = fileName;
	}
	else
	{
		originalFullPath = sourcePath + fileName;
		tryUseSourceConnection = true;
	}


	while (true)
	{
		if (!isFirstRun)
		{
			if (retryCount-- == 0)
				return false;

			logContext.resetLastError();
			logInfoLinef(L"Warning - Failed reading input file %ls, retrying in %i seconds", originalFullPath.c_str(), m_settings.retryWaitTimeMs/1000);
			Sleep(m_settings.retryWaitTimeMs);

			++stats.retryCount;
		}
		isFirstRun = false;

		WString fullPath = originalFullPath;
		// If there is a source connection we can read the file to local drive first and then read that file using normal commands
		if (tryUseSourceConnection && isValid(m_sourceConnection))
		{
			FileInfo srcDirInfo;
			uint srcDirAttributes = getFileInfo(srcDirInfo, sourcePath.c_str(), stats.ioStats);

			// TODO: Should this use a temporary file? This file will now be leaked at destination!
			ensureDirectory(m_destConnection, destPath, srcDirAttributes, stats.ioStats);
			u64 fileSize;
			u64 read;

			FileInfo srcInfo;
			uint srcAttributes;
			uint error;
			bool processedByServer;
			if (!m_sourceConnection->sendGetFileAttributes(fileName.c_str(), srcInfo, srcAttributes, error))
				continue;
			if (!m_sourceConnection->sendReadFileCommand(originalFullPath.c_str(), fileName.c_str(), srcInfo, srcAttributes, fileSize, read, m_copyContext, processedByServer))
				continue;
			fullPath = destPath + fileName;
		}


		FileHandle hFile;
		{
			if (!openFileRead(fullPath.c_str(), hFile, stats.ioStats, true, nullptr, true))
				continue;
		}
		ScopeGuard fileGuard([&]() { closeFile(fullPath.c_str(), hFile, AccessType_Read, stats.ioStats); });

		char* buffer = (char*)m_copyContext.buffers[1]; // Important that we use '1'.. '0' is used by SendFindFiles command
		uint left = 0;
		uint totalRead = 0;
		uint lineIndex = 0;
		while (true)
		{
			u64 read = 0;
			u64 toRead = CopyContextBufferSize - left - 1;
			{
				TimerScope _(stats.ioStats.readTime);
				++stats.ioStats.readCount;
				if (!readFile(fullPath.c_str(), hFile, buffer + left, toRead, read, stats.ioStats))
				{
					logErrorf(L"Failed reading input file %ls: %ls (Tried to read %u bytes after reading a total of %u bytes)", fullPath.c_str(), getLastErrorText().c_str(), toRead, totalRead);
					continue;
				}
			}
			totalRead += read;

			left += read;

			if (left == 0)
				break;

			buffer[left] = 0;

			uint consumePos = 0;

			while (true)
			{
				char* startPos = buffer + consumePos;
				char* newLinePos = strchr(startPos, '\n');
				if (newLinePos)
				{
					if (newLinePos > startPos && newLinePos[-1] == '\r')
						newLinePos[-1] = 0;

					*newLinePos = 0;
					if (*startPos)
					{
						if (lineIndex >= handledLineCount)
						{
							++handledLineCount;
							if (!func(startPos))
								return false; // These functions have built-in retry so we don't want to retry if these fails
						}
						++lineIndex;
					}
					consumePos = uint(newLinePos - buffer) + 1;
				}
				else
				{
					// Last line
					if (read == 0 && left != 0)
					{
						if (lineIndex >= handledLineCount)
						{
							++handledLineCount;
							if (!func(startPos))
								return false; // These functions have built-in retry so we don't want to retry if these fails
						}
						++lineIndex;
					}

					// Copy the left overs to beginning of buffer and continue
					left -= consumePos;
					memmove(buffer, buffer + consumePos, left);
					break;
				}
			}

			if (read == 0)
				break;
		}

		return true;
	}
}

bool
Client::excludeFilesFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath)
{
	auto executeFunc = [&](char* str) -> bool
	{
		convertSlashToBackslash(str);
		if (strchr(str, '*') != nullptr)
		{
			logErrorf(L"Wildcards not supported in exclude list file %ls", fileName.c_str());
			return false;
		}
		m_handledFiles.insert(WString(str, str + strlen(str))); // Does not need to be protected, happens before parallel
		return true;
	};

	return handleFilesOrWildcardsFromFile(logContext, stats, sourcePath, fileName, destPath, executeFunc);
}

bool
Client::gatherFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, CachedFindFileEntries& findFileCache, const WString& rootSourcePath, const WString& fileName, const WString& rootDestPath)
{
	// Do a find files in source paths to reduce number of kernel calls.. this can end up be a waste but in most cases most files are directly in the source root
	// Essentially we avoid doing lots of getFileInfo by doing a find files and get the file info from there.
	bool useFindFilesOptimization = m_settings.useOptimizedWildCardFileSearch;

	// Function to handle each entry
	auto executeFunc = [&](char* str) -> bool
	{
		bool handled = false;
		// Handle initial path that might exist inside input file

		// Parse line to figure out the parts. source [dest [file [file]...]] [options]

		int argc = 0;
		char* argv_[64];
		char buffer[1024];
		int numchars;
		stdargv_c::parse_cmdline(str, argv_, buffer, &argc, &numchars);
		--argc;
		wchar_t* argv[64];
		WString temp[64];
		for (int i=0; i!=argc; ++i)
		{
			temp[i] = WString(argv_[i], argv_[i] + strlen(argv_[i]));
			argv[i] = const_cast<wchar_t*>(temp[i].c_str());
		}

		if (argc == 0)
			return true;

		bool modifiedRootPaths = false;
		WString sourcePath = rootSourcePath;
		WString destPath = rootDestPath;
		convertSlashToBackslash(argv[0]);
		WString wpath = argv[0];

		int optionsStartIndex = 2;

		if (argc > 1)
		{
			if (*argv[1] == L'/')
			{
				optionsStartIndex = 1;
			}
			else
			{
				// Is dest path
				convertSlashToBackslash(argv[0]);
				convertSlashToBackslash(argv[1]);

				modifiedRootPaths = true;
				if (isAbsolutePath(argv[0]))
					sourcePath = argv[0];
				else
					sourcePath += argv[0];
				wpath.clear();

				destPath += argv[1];
				destPath +=  L"\\";
			}
		}

		// Parse options (right now only purge is handled)
		for (int i=optionsStartIndex; i<argc; ++i)
		{
			if (_wcsnicmp(argv[i], L"/PURGE", 6) != 0)
			{
				logErrorf(L"Only '/PURGE' allowed after second separator in file list %ls.. feel free to add more support :)", fileName.c_str());
				return false;
			}
			m_purgeDirs.insert(destPath + wpath + L'\\');
		}

		// Check if absolute path
		if (isAbsolutePath(wpath.c_str()))
		{
			if (_wcsnicmp(wpath.c_str(), sourcePath.c_str(), sourcePath.size()) == 0)
			{
				wpath = wpath.c_str() + sourcePath.size();
			}
			else if (!m_settings.flattenDestination)
			{
				logErrorf(L"Entry in file list %ls is using absolute path %ls that is not in source path %ls", fileName.c_str(), wpath.c_str(), sourcePath.c_str());
				return false;
			}
		}

		if (modifiedRootPaths || !useFindFilesOptimization) // Optimized path can't handle modified root paths.. handle path inline without using findfiles optimization
			return handlePath(logContext, m_sourceConnection, m_destConnection, stats, sourcePath, destPath, wpath.c_str());

		if (const wchar_t* lastSlash = wcsrchr(wpath.c_str(), L'\\'))
			findFileCache[WString(wpath.c_str(), lastSlash + 1)].insert(lastSlash + 1);
		else
			findFileCache[WString()].insert(std::move(wpath));
		return true;

	};

	return handleFilesOrWildcardsFromFile(logContext, stats, rootSourcePath, fileName, rootDestPath, executeFunc);
}

bool
Client::processQueuedWildcardFileEntries(LogContext& logContext, ClientStats& stats, CachedFindFileEntries& findFileCache, const WString& rootSourcePath, const WString& rootDestPath)
{
	for (auto& pe : findFileCache)
	{
		Vector<NameAndFileInfo> nafvec;
		std::map<const wchar_t*, const NameAndFileInfo*, NoCaseWStringLess> lookup;
		WString pathPath(rootSourcePath + pe.first);
		if (!findFilesInDirectory(nafvec, logContext, m_sourceConnection, m_copyContext, pathPath, stats))
			return false;
		for (auto& entry : nafvec)
			lookup.insert({entry.name.c_str(), &entry});
		for (auto& e : pe.second)
		{
			WString relativePath(pe.first + e);
			auto findIt = lookup.find(e.c_str());
			if (findIt != lookup.end())
			{
				if (!handlePath(logContext, m_sourceConnection, m_destConnection, stats, rootSourcePath, rootDestPath, relativePath.c_str(), findIt->second->attributes, findIt->second->info))
					return false;
			}
			else
			{
				wchar_t errorDesc[1024];
				bool success = handleMissingFile(relativePath.c_str());
				if (!success)
				{
					++stats.failCount;
					logErrorf(L"Can't find file/directory %ls", (rootSourcePath + relativePath).c_str());
					return false;
				}
			}
		}
	}
	return true;
}

bool
Client::purgeFilesInDirectory(const WString& path, uint destPathAttributes, int depthLeft, ClientStats& stats)
{
	// We don't enter symlinks for purging. Maybe this should be an command line option to treat symlinks just like normal directories
	// but in the use cases we have at ea we don't want to enter symlinks for purging
	if ((destPathAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		return true;


	WString relPath;
	if (path.size() > m_settings.destDirectory.size())
		relPath.append(path.c_str() + m_settings.destDirectory.size());

	// If there is a connection and no files were handled inside the directory we can just do a full delete on the server side
	if (isValid(m_destConnection))
		if ((relPath.empty() && m_handledFiles.empty()) || (!relPath.empty() && (m_handledFiles.find(relPath) == m_handledFiles.end())))
			return m_destConnection->sendDeleteAllFiles(relPath.c_str());


    FindFileData fd; 
    WString searchStr = path + L"*.*";
    FindFileHandle findHandle = findFirstFile(searchStr.c_str(), fd, stats.ioStats); 
    if(findHandle == InvalidFindFileHandle)
	{
		uint lastError = GetLastError();
		if (lastError == ERROR_FILE_NOT_FOUND)
			return true;
		logErrorf(L"FindFirstFile failed while purging with search string %ls: %ls", searchStr.c_str(), getErrorText(lastError).c_str());
		return false;
	}
	ScopeGuard _([&]() { findClose(findHandle, stats.ioStats); });

	bool errorOnMissingFile = false;
	bool res = true;
    do
	{
		FileInfo fileInfo;
		uint fileAttr = getFileInfo(fileInfo, fd);
		bool isDir = (fileAttr & FILE_ATTRIBUTE_DIRECTORY) != 0;

		if (!isDir && !isFileWithAttributeAllowed(fileAttr))
			continue;

		const wchar_t* fileName = getFileName(fd);
		// Check if file was copied here
		WString filePath = relPath + fileName;

		if (isDir)
		{
			if (isDotOrDotDot(fileName))
				continue;
			filePath += L'\\';
		}

		// File/directory was not part of source, delete
		if (m_handledFiles.find(filePath) == m_handledFiles.end())
		{
			if (isIgnoredDirectory(fileName))
				continue;
			WString fullPath = (path + fileName);
	        if(isDir)
			{
				bool isSymlink = (fileAttr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
				if (isSymlink)
				{
					if (!RemoveDirectoryW(fullPath.c_str()))
					{
						logErrorf(L"Trying to remove reparse point while purging destination %ls: %ls", fullPath.c_str(), getLastErrorText().c_str());
						res = false;
					}
				}
				else if (!deleteDirectory(fullPath.c_str(), stats.ioStats, errorOnMissingFile))
					res = false;
			}
			else
			{
				if (fileAttr & FILE_ATTRIBUTE_READONLY)
				{
					if (!setFileWritable(fullPath.c_str(), true))
						logErrorf(L"Could not purge read-only file in destination (%ls).  EACopy could not forcefully unset the file's read-only attribute in destination.", fullPath.c_str());
				}
				if (!deleteFile(fullPath.c_str(), stats.ioStats, errorOnMissingFile))
					res = false;
			}
		}
        else if(isDir)
		{
			if (!purgeFilesInDirectory(path + fileName + L'\\', fileAttr, depthLeft - 1, stats))
				res = false;
		}

	} while (findNextFile(findHandle, fd, stats.ioStats));

	uint error = GetLastError();
	if (error != ERROR_NO_MORE_FILES)
	{
		logErrorf(L"FindNextFile failed while purging for %ls: %ls", searchStr.c_str(), getErrorText(error).c_str());
		res = false;
	}

	return res;
}

bool
Client::ensureDirectory(Connection* destConnection, const WString& directory, uint attributes, IOStats& ioStats)
{
	if (directory[directory.size() - 1] != L'\\')
	{
		logErrorf(L"ensureDirectory must get path ending with '\\'");
		return false;
	}

	FilesSet createdDirs;

	if (isValid(destConnection))
	{
		// Ask connection to create new directory.
		if (!destConnection->sendCreateDirectoryCommand(directory.c_str(), createdDirs))
			return false;
	}
	else
	{
		// Create directory through windows api
		if (!eacopy::ensureDirectory(directory.c_str(), attributes, ioStats, true, m_settings.replaceSymLinksAtDestination, &createdDirs))
			return false;
	}

	ScopedCriticalSection cs(m_createdDirsCs);
	m_createdDirs.insert(createdDirs.begin(), createdDirs.end());

	return true;
}

const wchar_t*
Client::getRelativeSourceFile(const WString& sourcePath) const
{
	const wchar_t* logStr = sourcePath.c_str();
	const WString& baseDir = m_settings.sourceDirectory;
	if (_wcsnicmp(logStr, baseDir.c_str(), baseDir.size()) == 0)
		logStr += baseDir.size();
	return logStr;
}

const wchar_t*
Client::getFileKeyPath(const WString& relativePath) const
{
	if (m_settings.useLinksRelativePath)
		return relativePath.c_str();
	auto lastSlash = relativePath.find_last_of(L'\\');
	if (lastSlash == WString::npos)
		return relativePath.c_str();
	return relativePath.c_str() + lastSlash + 1;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Client::Connection*
Client::createConnection(const wchar_t* networkPath, uint connectionIndex, ClientStats& stats, bool& failedToConnect, bool doProtocolCheck)
{
	u64 startTime = getTime();

	ScopedCriticalSection networkInitScope(m_networkInitCs);

	if (!m_networkInitDone)
	{
		m_networkInitDone = true;

		// Initialize Winsock
		#if defined(_WIN32)
		WSADATA wsaData;
		int wsaRes = WSAStartup(MAKEWORD(2,2), &wsaData);
		if (wsaRes != 0)
		{
			logErrorf(L"WSAStartup failed with error: %d", wsaRes);
			return nullptr;
		}
		#endif

		m_networkWsaInitDone = true;

		WString networkServerName;

		if (m_settings.serverAddress.empty())
		{
			const wchar_t* serverNameStart = networkPath + 2;
			const wchar_t* serverNameEnd = wcschr(serverNameStart, L'\\');
			if (!serverNameEnd)
				return nullptr;

			const wchar_t* netDirectoryStart = serverNameEnd;
			while (*netDirectoryStart == '\\' && *netDirectoryStart != 0)
				++netDirectoryStart;
			if (!*netDirectoryStart)
			{
				logErrorf(L"Need to provide a net directory after the network server name (minimum \\\\<server>\\<netdir>): %d", networkPath);
				return nullptr;
			}

			networkServerName = WString(serverNameStart, serverNameEnd);

			m_networkServerNetDirectory = netDirectoryStart;
		}
		else
		{
			networkServerName = m_settings.serverAddress;

			m_networkServerNetDirectory = networkPath;
		}
   
		AddrInfo  hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET; //AF_UNSPEC; (Skip AF_INET6)
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		wchar_t defaultPortStr[32];
		itow(m_settings.serverPort, defaultPortStr, eacopy_sizeof_array(defaultPortStr));

		// Resolve the server address and port
		int res = getAddrInfoW(networkServerName.c_str(), defaultPortStr, &hints, &m_serverAddrInfo);
		if (res != 0)
		{
			if (res == WSAHOST_NOT_FOUND)
			{
				if (!failedToConnect) // Just to reduce chance of getting multiple log entries in multithreading scenarios (which doesnt matter)
				{
					logInfoLinef(L"   !!Invalid server address '%ls'", networkServerName.c_str());
					logInfoLinef();
					failedToConnect = true;
				}
				return nullptr;
			}
			logErrorf(L"GetAddrInfoW failed with error: %ls", getErrorText(res).c_str());

			return nullptr;
		}

		// Set server name and net directory (this will enable all connections to try to connect)
		m_networkServerName = networkServerName;

	}
	else if (m_networkServerName.empty())
		return nullptr;

	networkInitScope.leave();

	Socket sock = {INVALID_SOCKET, 0};

	// Loop through and attempt to connect to an address until one succeeds
	for(auto addrInfoIt=m_serverAddrInfo; addrInfoIt!=NULL; addrInfoIt=addrInfoIt->ai_next)
	{
		// Create a socket for connecting to server
		sock.socket = WSASocketW(addrInfoIt->ai_family, addrInfoIt->ai_socktype, addrInfoIt->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (sock.socket == INVALID_SOCKET)
		{
			logErrorf(L"socket failed with error: %ld", getLastNetworkError());
			return nullptr;
		}

		// Create guard in case we fail to connect (will be cancelled further down if we succeed)
		ScopeGuard socketClose([&]() { closeSocket(sock); });

		// Set to non-blocking just for the connect call (we want to control the connect timeout after connect using select instead)
		if (!setBlocking(sock, false))
			break;

		// Connect to server.
		int res = connect(sock.socket, addrInfoIt->ai_addr, (int)addrInfoIt->ai_addrlen);
		if (res == SOCKET_ERROR)
		{
			if (getLastNetworkError() != WSAEWOULDBLOCK)
				break;
		}

		// Return to blocking since we want select to block
		if (!setBlocking(sock, true))
			break;

		TIMEVAL timeval;
		timeval.tv_sec = 0;
		timeval.tv_usec = m_settings.serverConnectTimeoutMs*1000;
		fd_set write, err;
		FD_ZERO(&write);
		FD_ZERO(&err);
		FD_SET(sock.socket, &write);
		FD_SET(sock.socket, &err);

		// check if the socket is ready
		select(0,NULL,&write,&err,&timeval);			
		if(!FD_ISSET(sock.socket, &write)) 
			continue;

		// Socket is good, cancel the socket close scope and break out of the loop.
		socketClose.cancel();
		break;
	}

	u64 endTime = getTime();
	logDebugLinef(L"Connect to server %ls. (%.1f seconds)", sock.socket != INVALID_SOCKET ? L"SUCCESS" : L"FAILED", float(endTime - startTime)/1000.0f);

	if (sock.socket == INVALID_SOCKET)
	{
		failedToConnect = true;
		return nullptr;
	}

	ScopeGuard socketCleanup([&]() { closeSocket(sock); });

	// Set send buffer size (makes some difference in the tests we've done)
	if (!setSendBufferSize(sock, 4*1024*1024))
		return nullptr;

	// Disable Nagle's algorithm (it is 2019 after all)
	if (!disableNagle(sock))
		return nullptr;

	bool useSecurityFile;

	{
		// Read version command
		enum { MaxStringLength = 1024 };
		char cmdBuf[sizeof(VersionCommand) + MaxStringLength*2];
		auto& cmd = *(const VersionCommand*)cmdBuf;
		if (!receiveData(sock, &cmdBuf, 4))
			return nullptr;
		if (!receiveData(sock, cmdBuf+4, cmd.commandSize-4))
			return nullptr;
		stats.info = cmd.info;

		if (doProtocolCheck && cmd.protocolVersion != ProtocolVersion)
		{
			static bool logOnce = true;
			if (logOnce)
			{
				logOnce = false;
				logInfoLinef(L"   !!Protocol mismatch, will not use server. (Local: v%u, Server: v%u)", ProtocolVersion, cmd.protocolVersion);
				logInfoLinef();
			}
			failedToConnect = true;
			return nullptr;
		}

		useSecurityFile = (cmd.protocolFlags & UseSecurityFile) != 0;
	}

	// Connection is ready, cancel socket cleanup and create connection object
	socketCleanup.cancel();
	auto connection = new Connection(m_settings, stats, sock, m_compressionStats);
	ScopeGuard connectionGuard([&] { delete connection; });

	{
		// If we need to use security file we only want to do that once (and for the rest of the connections provide it in the environment command)
		ScopedCriticalSection cs(m_secretGuidCs);
		Guid zeroGuid = {0};
		bool hasSecretGuid = m_secretGuid != zeroGuid;
		if (hasSecretGuid)
		{
			cs.leave();
		}
		else if (!useSecurityFile)
		{
			// If no security is set, let the client create a guid that can be used on server side to identify which connections belonging to which client
			static_assert(sizeof(GUID) == sizeof(Guid), "GUID and Guid are not matching");
			if (CoCreateGuid((GUID*)&m_secretGuid) != S_OK)
			{
				logErrorf(L"CoCreateGuid - Failed to create filename guid");
				return nullptr;
			}
			hasSecretGuid = true;
			cs.leave();
		}


		// Send environment command
		char cmdBuf[MaxPath*2 + sizeof(EnvironmentCommand)+1];
		auto& cmd = *(EnvironmentCommand*)cmdBuf;
		cmd.commandType = CommandType_Environment;
		cmd.majorVersion = ClientMajorVersion;
		cmd.minorVersion = ClientMinorVersion;
		cmd.connectionIndex = connectionIndex;
		cmd.secretGuid = m_secretGuid;
		cmd.commandSize = sizeof(cmd) + uint(m_networkServerNetDirectory.size()*2);
		
		if (!stringCopy(cmd.netDirectory, MaxPath, m_networkServerNetDirectory.data()))
		{
			logErrorf(L"Failed send environment %ls: wcscpy_s. Server will not be used", m_networkServerNetDirectory.c_str());
			return nullptr;
		}
	
		if (!connection->sendCommand(cmd))
		{
			logErrorf(L"Failed sending environment command. Server will not be used");
			return nullptr;
		}

		if (!hasSecretGuid)
		{
			TimerScope _(stats.netSecretGuid);
			Guid securityFileGuid;
			if (!receiveData(sock, &securityFileGuid, sizeof(securityFileGuid)))
			{
				logErrorf(L"Failed receiving security file guid. Server will not be used", m_networkServerNetDirectory.c_str());
				return nullptr;
			}

			wchar_t securityFile[128];
			securityFile[0] = L'.';
			StringFromGUID2(*(GUID*)&securityFileGuid, securityFile + 1, 40);
			securityFile[1] = L'f';
			securityFile[38] = 0;
			FileHandle hFile;
			WString networkFilePath = WString(networkPath) + securityFile;
			bool secretGuidRead = false;
			{
				LogContext temp(*m_log);
				temp.mute(); // Mute log, we want to give different errors here
				if (openFileRead(networkFilePath.c_str(), hFile, stats.ioStats, true, nullptr, true))
				{
					ScopeGuard fileGuard([&]() { closeFile(networkFilePath.c_str(), hFile, AccessType_Read, stats.ioStats); });
					u64 read = 0;
					secretGuidRead = readFile(networkFilePath.c_str(), hFile, &m_secretGuid, sizeof(m_secretGuid),read, stats.ioStats) != 0;
				}
			}

			if (!sendData(sock, &m_secretGuid, sizeof(m_secretGuid)))
			{
				logErrorf(L"Failed sending secret Guid. Server will not be used");
				return nullptr;
			}
			if (!secretGuidRead)
			{
				logErrorf(L"Failed reading secret guid from file %ls. Server will not be used", networkFilePath.c_str());
				failedToConnect = true;
				return nullptr;
			}
		}
	}

	connectionGuard.cancel();

	return connection;
}

bool
Client::isIgnoredDirectory(const wchar_t *directory)
{
	// Check if dir should be excluded because of wild cards
	for (auto& excludeWildcard : m_settings.excludeWildcardDirectories)
		if (PathMatchSpecW(directory, excludeWildcard.c_str()))
			return true;
	return false;
}

bool
Client::isValid(Connection* connection)
{
	return connection && isValidSocket(connection->m_socket);
}

bool
Client::isFileWithAttributeAllowed(uint fileAttr)
{
	return (m_settings.excludeAttributes == 0 || !(fileAttr & m_settings.excludeAttributes)) &&
		(m_settings.includeAttributes == 0 || fileAttr & m_settings.includeAttributes);
}

Client::Connection::Connection(const ClientSettings& settings, ClientStats& stats, Socket s, CompressionStats& compressionStats)
:	m_settings(settings)
,	m_stats(stats)
,	m_hashContext(stats.hashTime, stats.hashCount)
,	m_socket(s)
,	m_compressionStats(compressionStats)
{
}

Client::Connection::~Connection()
{
	if (!isValidSocket(m_socket))
		return;

	DoneCommand cmd;
	cmd.commandType = CommandType_Done;
	cmd.commandSize = sizeof(cmd);
	sendCommand(cmd);

	u64 compressionLevelSum = 0;
	receiveData(m_socket, &compressionLevelSum, sizeof(compressionLevelSum), false);
	m_stats.compressionLevelSum += compressionLevelSum;

	destroy();
}

bool
Client::Connection::sendCommand(const Command& cmd)
{
	// Send an initial buffer
	if (!sendData(m_socket, (const char*)&cmd, cmd.commandSize))
		return false;
	//logDebugLinef("Bytes Sent: %ld", res);
	return true;
}

bool
Client::Connection::sendTextCommand(const wchar_t* text)
{
	char buffer[1024];
	auto& cmd = *(TextCommand*)buffer;
	cmd.commandType = CommandType_Text;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(text)*2);
	if (!stringCopy(cmd.string, eacopy_sizeof_array(buffer) - sizeof(cmd), text))
	{
		logErrorf(L"wcscpy_s in sendTextCommand failed");
		return false;
	}
	return sendCommand(cmd);
}

bool
Client::Connection::sendWriteFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, uint srcAttributes, u64& outSize, u64& outWritten, bool& outLinked, NetworkCopyContext& copyContext, bool &processedByServer)
{
	outSize = 0;
	outWritten = 0;
	outLinked = false;
	processedByServer = false;

	WriteFileType writeType = m_settings.compressionLevel != 0 ? WriteFileType_Compressed : WriteFileType_Send;

	char buffer[MaxPath*2 + sizeof(WriteFileCommand)];
	auto& cmd = *(WriteFileCommand*)buffer;
	cmd.commandType = CommandType_WriteFile;
	cmd.writeType = writeType;
	//cmd.excludeRules = ; // TODO: IMPLEMENT THIS m_settings.excludeChangedFiles
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dst)*2);
	if (!stringCopy(cmd.path, MaxPath, dst))
	{
		logErrorf(L"Failed to write file %ls: wcscpy_s in sendWriteFileCommand failed", dst);
		return false;
	}
	
	if (srcInfo.lastWriteTime.dwLowDateTime || srcInfo.lastWriteTime.dwHighDateTime)
		cmd.info = srcInfo;
	else
		if (getFileInfo(cmd.info, src, m_stats.ioStats) == 0)
			return false;

	outSize = cmd.info.fileSize;

	if (!sendCommand(cmd))
		return false;

	WriteResponse writeResponse;
	u64 netWriteResponseTime = 0;
	{
		TimerScope _(netWriteResponseTime);
		if (!receiveData(m_socket, &writeResponse, sizeof(writeResponse)))
			return false;
	}
	++m_stats.netWriteResponseCount[writeResponse];
	m_stats.netWriteResponseTime[writeResponse] += netWriteResponseTime;

	do
	{
		if (writeResponse == WriteResponse_Skip)
		{
			processedByServer = true;
			return true;
		}

		if (writeResponse == WriteResponse_Link)
		{
			outWritten = cmd.info.fileSize;
			outLinked = true;
			processedByServer = true;
			return true;
		}

		if (writeResponse == WriteResponse_Odx)
		{
			outWritten = cmd.info.fileSize;
			processedByServer = true;
			return true;
		}

		if (writeResponse != WriteResponse_Hash)
			break;

		Hash hash;
		if (!getFileHash(hash, src, copyContext, m_stats.ioStats, m_hashContext, m_stats.hashTime))
			return false;
		if (!sendData(m_socket, &hash, sizeof(hash)))
			return false;

		u64 netWriteResponseTime = 0;
		{
			TimerScope _(netWriteResponseTime);
			if (!receiveData(m_socket, &writeResponse, sizeof(writeResponse)))
				return false;
		}
		++m_stats.netWriteResponseCount[writeResponse];
		m_stats.netWriteResponseTime[writeResponse] += netWriteResponseTime;

	} while (true);

	if (writeResponse == WriteResponse_Copy)
	{
		bool useBufferedIO = getUseBufferedIO(m_settings.useBufferedIO, cmd.info.fileSize);

		SendFileStats sendStats;
		if (!sendFile(m_socket, src, cmd.info.fileSize, writeType, copyContext, m_compressionStats, useBufferedIO, m_stats.ioStats, sendStats))
			return false;
		m_stats.sendTime += sendStats.sendTime;
		m_stats.sendSize += sendStats.sendSize;
		m_stats.compressTime += sendStats.compressTime;
		m_stats.compressionLevelSum += sendStats.compressionLevelSum;

		u8 writeSuccess;
		if (!receiveData(m_socket, &writeSuccess, sizeof(writeSuccess)))
			return false;

		if (!writeSuccess)
		{
			logErrorf(L"Failed to write file %ls: server returned failure after sending file", dst);
			return false;
		}

		outWritten = cmd.info.fileSize;
		processedByServer = true;

		return true;
	}
	
	if (writeResponse == WriteResponse_CopyUsingSmb) // It seems server is a proxy server and the server wants the client to copy the file directly to the share using smb
	{
		bool useSystemCopy = m_settings.useSystemCopy;
		bool existed;
		u64 written;
		WString fullDst = m_settings.destDirectory + dst;
		bool success = copyFile(src, srcInfo, srcAttributes, fullDst.c_str(), useSystemCopy, false, existed, written, copyContext, m_stats.ioStats, m_settings.useBufferedIO);
		u8 copyResult = success ? 1 : 0;
		if (!sendData(m_socket, &copyResult, sizeof(copyResult)))
			return false;
		if (success)
			outWritten = cmd.info.fileSize;
		return success;
	}

	if (writeResponse == WriteResponse_CopyDelta)
	{
		#if defined(EACOPY_ALLOW_RSYNC)
		RsyncStats rsyncStats;
		if (!clientHandleRsync(m_socket, src, rsyncStats))
			return false;
		m_stats.sendTime += rsyncStats.sendTime;
		m_stats.sendSize += rsyncStats.sendSize;
		m_stats.ioStats.readTime += rsyncStats.readTime;
		//m_stats.ioStats.readSize += rsyncStats.readSize;
		m_stats.deltaCompressionTime += rsyncStats.rsyncTime;

		u8 writeSuccess;
		if (!receiveData(m_socket, &writeSuccess, sizeof(writeSuccess)))
			return false;

		if (!writeSuccess)
		{
			logErrorf(L"Failed to write file %ls: server returned failure after sending file delta", dst);
			return false;
		}

		outWritten = cmd.info.fileSize;
		return true;
		#endif
		return false;
	}

	if (writeResponse == WriteResponse_BadDestination)
	{
		logErrorf(L"Failed to write file %ls: Server reported Bad destination (check your destination path)", src);
		return false;
	}

	return false;
}

Client::Connection::ReadFileResult
Client::Connection::sendReadFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, uint srcAttributes, u64& outSize, u64& outRead, NetworkCopyContext& copyContext, bool& processedByServer)
{
	outSize = 0;
	outRead = 0;
	processedByServer = false;

	// Make src a local path to server
	src += m_settings.sourceDirectory.size();

	char buffer[MaxPath*2 + sizeof(ReadFileCommand)];
	auto& cmd = *(ReadFileCommand*)buffer;
	cmd.commandType = CommandType_ReadFile;
	cmd.compressionLevel = m_settings.compressionLevel;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(src) * 2);
	if (!stringCopy(cmd.path, MaxPath, src))
	{
		logErrorf(L"Failed to read file %ls: wcscpy_s in sendReadFileCommand failed", src);
		return ReadFileResult_Error;
	}

	WString fullDest = m_settings.destDirectory + dst;

	if (uint fileAttributes = getFileInfo(cmd.info, fullDest.c_str(), m_stats.ioStats))
	{
		if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			logErrorf(L"Trying to copy to file %ls which is a directory", fullDest.c_str());
			return ReadFileResult_Error;
		}

		if (m_settings.excludeChangedFiles)
		{
			outSize = cmd.info.fileSize;
			return ReadFileResult_Success;
		}
	}

	if (srcInfo.fileSize != 0 && equals(srcInfo, cmd.info))
	{
		outSize = cmd.info.fileSize;
		return ReadFileResult_Success;
	}

	if (!sendCommand(cmd))
		return ReadFileResult_Error;

	ReadResponse readResponse;
	if (!receiveData(m_socket, &readResponse, sizeof(readResponse)))
		return ReadFileResult_Error;

	if (readResponse == ReadResponse_ServerBusy)
	{
		// Server was busy and ask this worker to sleep for a while and come back later (this is under quite extreme conditions so sleep for five seconds or so)
		return ReadFileResult_ServerBusy;
	}

	if (readResponse == ReadResponse_BadSource)
	{
		logErrorf(L"Unknown server side error while asking for file %ls: sendReadFileCommand failed", fullDest.c_str());
		return ReadFileResult_Error;
	}

	if (readResponse == ReadResponse_Hash)
	{
		Hash hash;
		// Test file exists before actually calculating hash or we will crash the program and leave the server hanging waiting for the hash input.
		// If file doesn't exist, we will just return empty hash info and the server will test for invalid hash.
		FILE *file;
		if (_wfopen_s(&file, fullDest.c_str(), L"r") == 0)
		{
			if (file != nullptr)
			{
				fclose(file);
				if (!getFileHash(hash, fullDest.c_str(), copyContext, m_stats.ioStats, m_hashContext, m_stats.hashTime))
					return ReadFileResult_Error;
			}
		}
		if (!sendData(m_socket, &hash, sizeof(hash)))
			return ReadFileResult_Error;
		if (!receiveData(m_socket, &readResponse, sizeof(readResponse)))
			return ReadFileResult_Error;
	}

	// Skip file, we already have the same file as source
	if (readResponse == ReadResponse_Skip)
	{
		outSize = cmd.info.fileSize;
		processedByServer = true;
		return ReadFileResult_Success;
	}

	FileTime newFileLastWriteTime;
	if (!receiveData(m_socket, &newFileLastWriteTime, sizeof(newFileLastWriteTime)))
		return ReadFileResult_Error;

	if (readResponse == ReadResponse_Copy)
	{
		// Read file size for new file from server
		u64 newFileSize;
		if (!receiveData(m_socket, &newFileSize, sizeof(newFileSize)))
			return ReadFileResult_Error;


		bool success = true;
		WriteFileType writeType = m_settings.compressionLevel != 0 ? WriteFileType_Compressed : WriteFileType_Send;
		bool useBufferedIO = getUseBufferedIO(m_settings.useBufferedIO, cmd.info.fileSize);
		uint commandSize = 0;

		// Read actual file from server
		RecvFileStats recvStats;
		if (!receiveFile(success, m_socket, fullDest.c_str(), newFileSize, newFileLastWriteTime, writeType, useBufferedIO, copyContext, nullptr, 0, commandSize, m_stats.ioStats, recvStats))
			return ReadFileResult_Error;
		m_stats.recvTime += recvStats.recvTime;
		m_stats.recvSize += recvStats.recvSize;
		m_stats.decompressTime += recvStats.decompressTime;

		outRead = newFileSize;
		outSize = newFileSize;
		processedByServer = success;
		return success ? ReadFileResult_Success : ReadFileResult_Error;
	}
	else if (readResponse == ReadResponse_CopyUsingSmb)
	{
		bool existed;
		u64 written;
		WString fullSrc = m_settings.sourceDirectory + src;
		bool useSystemCopy = m_settings.useSystemCopy;
		if (!copyFile(fullSrc.c_str(), srcInfo, srcAttributes, fullDest.c_str(), useSystemCopy, false, existed, written, copyContext, m_stats.ioStats, m_settings.useBufferedIO))
			return ReadFileResult_Error;
		outRead = written;
		outSize = written;
		processedByServer = true;
		return ReadFileResult_Success;
	}
	else // ReadResponse_CopyDelta
	{
		#if defined(EACOPY_ALLOW_DELTA_COPY)
		RecvDeltaStats recvStats;
		// Read file size for new file from server
		u64 newFileSize;
		if (!receiveData(m_socket, &newFileSize, sizeof(newFileSize)))
			return ReadFileResult_Error;
		if (!receiveDelta(m_socket, fullDest.c_str(), cmd.info.fileSize, fullDest.c_str(), newFileSize, newFileLastWriteTime, copyContext, m_stats.ioStats, recvStats))
			return ReadFileResult_Error;
		outSize = newFileSize;
		outRead = newFileSize;
		processedByServer = true;
		m_stats.recvTime += recvStats.recvTime;
		m_stats.recvSize += recvStats.recvSize;
		m_stats.decompressTime += recvStats.decompressTime;
		return ReadFileResult_Success;
		#endif
	}
	return ReadFileResult_Error;
}

bool
Client::Connection::sendCreateDirectoryCommand(const wchar_t* directory, FilesSet& outCreatedDirs)
{
	++m_stats.netCreateDirCount;
	TimerScope _(m_stats.netCreateDirTime);
	const wchar_t* relDir = directory + m_settings.destDirectory.size();

	char buffer[MaxPath*2 + sizeof(CreateDirCommand)+1];
	auto& cmd = *(CreateDirCommand*)buffer;
	cmd.commandType = CommandType_CreateDir;
	uint relDirLen = wcslen(relDir);
	cmd.commandSize = sizeof(cmd) + uint(relDirLen*2);
	
	if (!stringCopy(cmd.path, MaxPath, relDir))
	{
		logErrorf(L"Failed to create directory %ls: wcscpy_s in sendCreateDirectoryCommand failed", relDir);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	CreateDirResponse createDirResponse;
	if (!receiveData(m_socket, &createDirResponse, sizeof(createDirResponse)))
		return false;

	if (createDirResponse == CreateDirResponse_BadDestination)
	{
		logErrorf(L"Failed to create directory %ls: Server reported Bad destination (check your destination path)", relDir);
		return false;
	}

	if (createDirResponse == CreateDirResponse_Error)
	{
		logErrorf(L"Failed to create directory %ls: Server reported unknown error", relDir);
		return false;
	}

	if (createDirResponse > CreateDirResponse_SuccessExisted)
	{
		uint directoryCreationCount = createDirResponse - CreateDirResponse_SuccessExisted;
		WString tempDir(directory);
		while (true)
		{
			outCreatedDirs.insert(tempDir);
			if (!--directoryCreationCount)
				break;
			tempDir.resize(tempDir.size()-1);
			int index = tempDir.find_last_of(L'\\');
			tempDir.resize(index+1);
		}
	}
	return true;
}

bool
Client::Connection::sendDeleteAllFiles(const wchar_t* dir)
{
	char buffer[MaxPath*2 + sizeof(DeleteFilesCommand)+1];
	auto& cmd = *(DeleteFilesCommand*)buffer;
	cmd.commandType = CommandType_DeleteFiles;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dir)*2);

	if (!stringCopy(cmd.path, MaxPath, dir))
	{
		logErrorf(L"Failed to delete directory %ls: wcscpy_s in sendDeleteAllFiles failed", dir);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	DeleteFilesResponse deleteFilesResponse;
	if (!receiveData(m_socket, &deleteFilesResponse, sizeof(deleteFilesResponse)))
		return false;

	if (deleteFilesResponse == DeleteFilesResponse_BadDestination)
	{
		logErrorf(L"Failed to delete directory %ls: Server reported Bad destination (check your destination path)", dir);
		return false;
	}

	if (deleteFilesResponse == DeleteFilesResponse_Error)
	{
		logErrorf(L"Failed to delete directory %ls: Server reported unknown error", dir);
		return false;
	}
	return true;
}

bool
Client::Connection::sendFindFiles(const wchar_t* dirAndWildcard, Vector<NameAndFileInfo>& outFiles, CopyContext& copyContext)
{
	++m_stats.netFindFilesCount;
	TimerScope _(m_stats.netFindFilesTime);

	char buffer[MaxPath*2 + sizeof(FindFilesCommand)+1];
	auto& cmd = *(FindFilesCommand*)buffer;
	cmd.commandType = CommandType_FindFiles;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dirAndWildcard)*2);

	if (!stringCopy(cmd.pathAndWildcard, MaxPath, dirAndWildcard))
	{
		logErrorf(L"Failed to find files %ls: wcscpy_s in sendFindFiles failed", dirAndWildcard);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	u8* copyBuffer = copyContext.buffers[0]; // Important that we use '0'.. '1' is used by file wildcard reading

	while (true)
	{
		uint blockSize;
		if (!receiveData(m_socket, &blockSize, sizeof(blockSize)))
			return false;

		if (blockSize == 0)
			return true;

		if (blockSize == ~0u)
		{
			logErrorf(L"Can't find %ls", dirAndWildcard);
			return false;
		}
	
		if (!receiveData(m_socket, copyBuffer, blockSize))
			return false;

		u8* blockPos = copyBuffer;
		u8* blockEnd = blockPos + blockSize;
		while (blockPos != blockEnd)
		{
			NameAndFileInfo nafi;

			nafi.attributes = *(uint*)blockPos;
			blockPos += sizeof(uint);
			nafi.info.lastWriteTime = *(FileTime*)blockPos;
			blockPos += sizeof(u64);
			nafi.info.fileSize = *(u64*)blockPos;
			blockPos += sizeof(u64);
			nafi.name = (wchar_t*)blockPos;
			blockPos += (nafi.name.size()+1)*2;
			outFiles.push_back(std::move(nafi));
		}
	}

	return false;
}

bool
Client::Connection::sendGetFileAttributes(const wchar_t* path, FileInfo& outInfo, uint& outAttributes, uint& outError)
{
	++m_stats.netFileInfoCount;
	TimerScope _(m_stats.netFileInfoTime);

	char buffer[MaxPath*2 + sizeof(GetFileInfoCommand)+1];
	auto& cmd = *(GetFileInfoCommand*)buffer;
	cmd.commandType = CommandType_GetFileInfo;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(path)*2);

	if (!stringCopy(cmd.path, MaxPath, path))
	{
		logErrorf(L"Failed to get file info %ls: wcscpy_s in sendGetFileAttributes failed", path);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	alignas(8) u8 recvBuffer[3*8+2*4];
	static_assert(sizeof(recvBuffer) == sizeof(FileInfo) + sizeof(uint) + sizeof(uint), "");

	if (!receiveData(m_socket, recvBuffer, sizeof(recvBuffer)))
		return false;
	outInfo = *(FileInfo*)recvBuffer;
	outAttributes = *(uint*)(recvBuffer + sizeof(FileInfo));
	outError = *(uint*)(recvBuffer + sizeof(FileInfo) + sizeof(uint));

	return true;
}

bool
Client::Connection::destroy()
{
	ScopeGuard socketCleanup([this]() { closeSocket(m_socket); });

	// shutdown the connection since no more data will be sent
	if (shutdown(m_socket.socket, SD_SEND) == SOCKET_ERROR)
	{
		logErrorf(L"shutdown failed with error: %d", getLastNetworkError());
		return false;
	}

	// Receive until the peer closes the connection
	while (true)
	{
		char recvbuf[512];
		int recvbuflen = sizeof(recvbuf);
		int res = recv(m_socket.socket, recvbuf, recvbuflen, 0);

		if (res == 0)
		{
			//logDebugLinef(L"Connection closed");
			break;
		}
		
		if (res < 0)
		{
			logErrorf(L"recv failed with error: %d", getLastNetworkError());
			return false;
		}
			
		//logDebugLinef("Bytes received: %d", res);

	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace stdargv_c
{
	constexpr char NULCHAR    = '\0';
	constexpr char SPACECHAR  = ' ';
	constexpr char TABCHAR	  =	'\t';
	constexpr char CRCHAR	  = '\r';
	constexpr char LFCHAR	  = '\n';
	constexpr char DQUOTECHAR =	'\"';
	constexpr char SLASHCHAR  = '\\';

	static void parse_cmdline(
		const char *cmdstart,
		char **argv,
		char *args,
		int *numargs,
		int *numchars
		)
	{
			const char *p;
			char c;
			int inquote;                    /* 1 = inside quotes */
			int copychar;                   /* 1 = copy char to *args */
			unsigned numslash;              /* num of backslashes seen */

			*numchars = 0;
			*numargs = 1;                   /* the program name at least */

			/* first scan the program name, copy it, and count the bytes */
			p = cmdstart;
			if (argv)
				*argv++ = args;

	#ifdef WILDCARD
			/* To handle later wild card expansion, we prefix each entry by
			it's first character before quote handling.  This is done
			so _[w]cwild() knows whether to expand an entry or not. */
			if (args)
				*args++ = *p;
			++*numchars;

	#endif  /* WILDCARD */

			/* A quoted program name is handled here. The handling is much
			   simpler than for other arguments. Basically, whatever lies
			   between the leading double-quote and next one, or a terminal null
			   character is simply accepted. Fancier handling is not required
			   because the program name must be a legal NTFS/HPFS file name.
			   Note that the double-quote characters are not copied, nor do they
			   contribute to numchars. */
			inquote = 0;
			do {
				if (*p == DQUOTECHAR )
				{
					inquote = !inquote;
					c = (char) *p++;
					continue;
				}
				++*numchars;
				if (args)
					*args++ = *p;

				c = (char) *p++;
	#ifdef _MBCS
				if (_ismbblead(c)) {
					++*numchars;
					if (args)
						*args++ = *p;   /* copy 2nd byte too */
					p++;  /* skip over trail byte */
				}
	#endif  /* _MBCS */

			} while ( (c != NULCHAR && c != CRCHAR && c != LFCHAR && (inquote || (c !=SPACECHAR && c != TABCHAR))) );

			if ( c == NULCHAR ) {
				p--;
			} else {
				if (args)
					*(args-1) = NULCHAR;
			}

			inquote = 0;

			/* loop on each argument */
			for(;;) {

				if ( *p ) {
					while (*p == SPACECHAR || *p == TABCHAR || *p == CRCHAR || *p == LFCHAR)
						++p;
				}

				if (*p == NULCHAR)
					break;              /* end of args */

				/* scan an argument */
				if (argv)
					*argv++ = args;     /* store ptr to arg */
				++*numargs;

	#ifdef WILDCARD
			/* To handle later wild card expansion, we prefix each entry by
			it's first character before quote handling.  This is done
			so _[w]cwild() knows whether to expand an entry or not. */
			if (args)
				*args++ = *p;
			++*numchars;

	#endif  /* WILDCARD */

			/* loop through scanning one argument */
			for (;;) {
				copychar = 1;
				/* Rules: 2N backslashes + " ==> N backslashes and begin/end quote
				   2N+1 backslashes + " ==> N backslashes + literal "
				   N backslashes ==> N backslashes */
				numslash = 0;
				while (*p == SLASHCHAR) {
					/* count number of backslashes for use below */
					++p;
					++numslash;
				}
				if (*p == DQUOTECHAR) {
					/* if 2N backslashes before, start/end quote, otherwise
						copy literally */
					if (numslash % 2 == 0) {
						if (inquote && p[1] == DQUOTECHAR) {
							p++;    /* Double quote inside quoted string */
						} else {    /* skip first quote char and copy second */
							copychar = 0;       /* don't copy quote */
							inquote = !inquote;
						}
					}
					numslash /= 2;          /* divide numslash by two */
				}

				/* copy slashes */
				while (numslash--) {
					if (args)
						*args++ = SLASHCHAR;
					++*numchars;
				}

				/* if at end of arg, break loop */
				if (*p == NULCHAR || *p == CRCHAR || *p == LFCHAR || (!inquote && (*p == SPACECHAR || *p == TABCHAR)))
					break;

				/* copy character into argument */
	#ifdef _MBCS
				if (copychar) {
					if (args) {
						if (_ismbblead(*p)) {
							*args++ = *p++;
							++*numchars;
						}
						*args++ = *p;
					} else {
						if (_ismbblead(*p)) {
							++p;
							++*numchars;
						}
					}
					++*numchars;
				}
				++p;
	#else  /* _MBCS */
				if (copychar) {
					if (args)
						*args++ = *p;
					++*numchars;
				}
				++p;
	#endif  /* _MBCS */
				}

				/* null-terminate the argument */

				if (args)
					*args++ = NULCHAR;          /* terminate string */
				++*numchars;
			}

			/* We put one last argument in -- a null ptr */
			if (argv)
				*argv++ = NULL;
			++*numargs;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
