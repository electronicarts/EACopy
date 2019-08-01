// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"
#include <ws2tcpip.h>
#include <assert.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <strsafe.h>

#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
#include <EACopyRsync.h>
#endif

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
#include "EACopyZdelta.h"
#endif

#pragma comment (lib, "Shlwapi.lib") // PathMatchSpecW

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
	ScopeGuard wsaCleanup([this]() { if (m_networkWsaInitDone) WSACleanup(); });

	m_serverAddrInfo = nullptr;
	ScopeGuard addrCleanup([this]() { if (m_serverAddrInfo) FreeAddrInfoW(m_serverAddrInfo); });

	LogContext logContext(log);

	const WString& sourceDir = m_settings.sourceDirectory;
	if (sourceDir.size() < 5 || sourceDir[0] != '\\' || sourceDir[1] != '\\')
		m_useSourceServerFailed = true;

	const WString& destDir = m_settings.destDirectory;
	if (destDir.size() < 5 || destDir[0] != '\\' || destDir[1] != '\\')
		m_useDestServerFailed = true;

	// Try to connect to server (can fail to connect and still return true if settings allow it to fail)
	if (!connectToServer(destDir.c_str(), m_destConnection, m_useDestServerFailed, outStats))
		return -1;
	ScopeGuard destConnectionCleanup([this]() { delete m_destConnection; });

	// Spawn worker threads that will copy the files
	Vector<HANDLE> workerThreadList(m_settings.threadCount);
	struct WorkerThreadData { ClientStats stats; Client* client; };
	Vector<WorkerThreadData> workerThreadDataList(m_settings.threadCount);
	for (int i=0; i!=m_settings.threadCount; ++i)
	{
		auto& threadData = workerThreadDataList[i];
		threadData.client = this;
		workerThreadList[i] = CreateThread(NULL, 0, [](LPVOID p) -> DWORD
			{
				auto& data = *(WorkerThreadData*)p;
				return data.client->workerThread(data.stats);
			}, &threadData, 0, NULL);
	}

	// Setup guard that will make sure all threads are waited for before leaving method
	ScopeGuard waitThreadsGuard([&]()
	{
		// Wait for all threads to finish
		m_workersActive = false;
		WaitForMultipleObjects((uint)workerThreadList.size(), workerThreadList.data(), TRUE, INFINITE);
	});

	u64 startFindFileTimeMs = getTimeMs();

	// Collect exclusions provided through file
	for (auto& file : m_settings.filesExcludeFiles)
		if (!excludeFilesFromFile(sourceDir, file, destDir))
			break;

	{
		// Lazily create destination root folder based on if it is needed
		bool destDirCreated = false;
		auto createDirectoryFunc = [&]()
		{
			if (destDirCreated)
				return true;
			destDirCreated = true;
			return ensureDirectory(destDir.c_str());
		};

		// Traverse through and collect all files that needs copying (worker threads will handle copying). This code will also generate destination folders needed.
		if (!m_settings.filesOrWildcardsFiles.empty())
		{
			for (auto& file : m_settings.filesOrWildcardsFiles)
				if (!gatherFilesOrWildcardsFromFile(logContext, outStats, sourceDir, file, destDir, createDirectoryFunc))
					break;
		}
		else
		{
			for (auto& fileOrWildcard : m_settings.filesOrWildcards)
				if (!findFilesInDirectory(sourceDir, destDir, fileOrWildcard, m_settings.copySubdirDepth, createDirectoryFunc))
					break;
		}
	}

	outStats.findFileTimeMs = getTimeMs() - startFindFileTimeMs;


	// Connect to source if no destination is set
	Connection* sourceConnection = nullptr;
	if (!m_destConnection)
		if (!connectToServer(m_settings.sourceDirectory.c_str(), sourceConnection, m_useSourceServerFailed, outStats))
			return false;
	ScopeGuard sourceConnectionGuard([&] { delete sourceConnection; });

	// Process files (worker threads are doing the same right now)
	if (!processFiles(logContext, sourceConnection, m_destConnection, outStats, true))
		return -1;
	

	// Wait for all worker threads to finish
	waitThreadsGuard.execute();

	// If main thread had an error code, return that
	if (int exitCode = logContext.getLastError())
		return exitCode;

	// Go through all threads and see if any of them had an error code.
	for (HANDLE wt : workerThreadList)
	{
		DWORD threadExitCode;
		if (!GetExitCodeThread(wt, &threadExitCode))
			return -1;
		if (threadExitCode != 0)
			return threadExitCode;
		CloseHandle(wt);
	}

	// If purge feature is enabled.. traverse destination and remove unwanted files/folders
	if (m_settings.purgeDestination)
		if (!purgeFilesInDirectory(destDir, m_settings.copySubdirDepth))
			return -1;

	// Purge individual directories (can be provided in filelist file)
	for (auto& purgeDir : m_purgeDirs)
		if (!purgeFilesInDirectory(purgeDir.c_str(), m_settings.copySubdirDepth))
			return -1;


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

		//outStats.copyTimeMs += threadStats.copyTimeMs; // Only use main thread for time
		//outStats.skipTimeMs += threadStats.skipTimeMs; // Only use main thread for time
		//outStats.linkTimeMs += threadStats.linkTimeMs; // Only use main thread for time

		outStats.compressTimeMs += threadStats.compressTimeMs;
		outStats.deltaCompressionTimeMs += threadStats.deltaCompressionTimeMs;
		outStats.sendTimeMs += threadStats.sendTimeMs;
		outStats.sendSize += threadStats.sendSize;
		outStats.compressionLevelSum += threadStats.compressionLevelSum;
		outStats.failCount += threadStats.failCount;
		outStats.retryCount += threadStats.retryCount;
		outStats.connectTimeMs += threadStats.connectTimeMs;
		outStats.copyStats.createReadTimeMs += threadStats.copyStats.createReadTimeMs;
		outStats.copyStats.readTimeMs += threadStats.copyStats.readTimeMs;
		outStats.copyStats.createWriteTimeMs += threadStats.copyStats.createWriteTimeMs;
		outStats.copyStats.writeTimeMs += threadStats.copyStats.writeTimeMs;
		outStats.copyStats.setLastWriteTimeTimeMs += threadStats.copyStats.setLastWriteTimeTimeMs;
	}

	outStats.compressionAverageLevel = outStats.copySize ? (float)((double)outStats.compressionLevelSum / outStats.copySize) : 0;

	outStats.serverUsed =  m_settings.useServer != UseServer_Disabled && !m_useDestServerFailed;

	// Success!
	return 0;
}

int
Client::reportServerStatus(Log& log)
{
	resetWorkState(log);

	LogContext logContext(log);

	bool useServerFailed;
	ClientStats stats;

	// Create connection
	Connection* connection = createConnection(m_settings.destDirectory.c_str(), stats, useServerFailed);
	if (!connection)
	{
		logErrorf(L"Failed to connect to server. Is path '%s' a proper smb path?", m_settings.destDirectory.c_str());
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

	wchar_t buffer[2048];
	assert(dataSize < sizeof(buffer)-1);
	if (!receiveData(connection->m_socket, buffer, dataSize*2))
		return -1;

	buffer[dataSize] = 0;
	
	// Print report
	logInfof(buffer);
	return 0;
}

void
Client::resetWorkState(Log& log)
{
	m_log = &log;
	m_useSourceServerFailed = false;
	m_useDestServerFailed = false;
	m_workersActive = true;
	m_tryCopyFirst = true;
	m_networkInitDone = false;
	m_networkServerName.clear();
	m_copyEntries.clear();
	m_handledFiles.clear();
	m_destConnection = nullptr;
}

bool
Client::processFile(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats)
{
	// Pop first entry off the queue
	CopyEntry entry;
	m_copyEntriesCs.enter();
	if (!m_copyEntries.empty())
	{
		entry = m_copyEntries.front();
		m_copyEntries.pop_front();
	}
	m_copyEntriesCs.leave();

	// If no new entry queued, sleep a bit and return in order to try again
	if (entry.src.empty())
	{
		Sleep(1);
		return false;
	}

	// Get full destination path
	WString fullDst = m_settings.destDirectory + L'\\' + entry.dst;

	// Try to copy file
	int retryCount = m_settings.retryCount;
	while (m_workersActive)
	{
		// Use connection to server if available
		if (destConnection)
		{
			u64 startTimeMs = getTimeMs();
			u64 size;
			u64 written;
			bool linked;

			// Send file to server (might be skipped if server already has it).. returns false if it fails
			if (destConnection->sendWriteFileCommand(entry.src.c_str(), entry.dst.c_str(), size, written, linked, copyContext))
			{
				if (written)
				{
					if (m_settings.logProgress)
						logInfoLinef(L"%s   %s", linked ? L"Link File" : L"New File ", getRelativeSourceFile(entry.src));
					(linked ? stats.linkTimeMs : stats.copyTimeMs) += getTimeMs() - startTimeMs;
					++(linked ? stats.linkCount : stats.copyCount);
					(linked ? stats.linkSize : stats.copySize) += written;
				}
				else
				{
					if (m_settings.logProgress)
						logInfoLinef(L"Skip File   %s", getRelativeSourceFile(entry.src));
					stats.skipTimeMs += getTimeMs() - startTimeMs;
					++stats.skipCount;
					stats.skipSize += size;
				}
				return true;
			}
		}
		else if (sourceConnection)
		{
			u64 startTimeMs = getTimeMs();
			u64 size;
			u64 read;
			if (sourceConnection->sendReadFileCommand(entry.src.c_str(), entry.dst.c_str(), size, read, copyContext))
			{
				if (read)
				{
					if (m_settings.logProgress)
						logInfoLinef(L"%s   %s", L"New File ", getRelativeSourceFile(entry.src));
					stats.copyTimeMs += getTimeMs() - startTimeMs;
					++stats.copyCount;
					stats.copySize += size;
				}
				else
				{
					if (m_settings.logProgress)
						logInfoLinef(L"Skip File   %s", getRelativeSourceFile(entry.src));
					stats.skipTimeMs += getTimeMs() - startTimeMs;
					++stats.skipCount;
					stats.skipSize += size;
				}
				return true;
			}
			return false;
		}
		else
		{
			bool tryCopyFirst = m_tryCopyFirst;

			bool existed = false;
			u64 written;
			u64 startTimeMs = getTimeMs();

			// Try to copy file first without checking if it is there (we optimize for copying new files)
			if (tryCopyFirst)
			{
				if (copyFile(entry.src.c_str(), fullDst.c_str(), true, existed, written, copyContext, stats.copyStats, m_settings.useBufferedIO))
				{
					if (m_settings.logProgress)
						logInfoLinef(L"New File    %s", getRelativeSourceFile(entry.src));
					stats.copyTimeMs += getTimeMs() - startTimeMs;
					++stats.copyCount;
					stats.copySize += written;
					return true;
				}

				// Stop trying to copy first since we found something in target directory and there most likely are more (it is ok this is not thread safe.. it is just an optimization that can take effect later)
				// A failed copy is way more expensive that a failed "getFileInfo" so this change is to stop trying to copy first
				m_tryCopyFirst = false;
			}

			// Handle scenario of failing to copy because target existed or we never tried copy it
			if (existed || !tryCopyFirst)
			{
				FileInfo destInfo;
				DWORD fileAttributes = getFileInfo(destInfo, fullDst.c_str());

				// If no file attributes it might be that the file doesnt exist
				if (!fileAttributes)
				{
					if (m_settings.logProgress)
						logDebugLinef(L"Failed to get attributes from file %s", fullDst.c_str());
				}
				else
				{
					// Skip file if the same
					if (equals(entry.srcInfo, destInfo))
					{
						if (m_settings.logProgress)
							logInfoLinef(L"Skip File   %s", getRelativeSourceFile(entry.src));
						stats.skipTimeMs += getTimeMs() - startTimeMs;
						++stats.skipCount;
						stats.skipSize += destInfo.fileSize;

						return true;
					}
				}

				// if destination file is read-only then we will clear that flag so the copy can succeed
				if (fileAttributes & FILE_ATTRIBUTE_READONLY)
				{
					if (!SetFileAttributesW(fullDst.c_str(), FILE_ATTRIBUTE_NORMAL))
						logErrorf(L"Could not copy over read-only destination file (%s).  EACopy could not forcefully unset the destination file's read-only attribute.", fullDst.c_str());
				}
				
				if (copyFile(entry.src.c_str(), fullDst.c_str(), false, existed, written, copyContext, stats.copyStats, m_settings.useBufferedIO))
				{
					if (m_settings.logProgress)
						logInfoLinef(L"New File    %s", entry.src.c_str());

					stats.copyTimeMs += getTimeMs() - startTimeMs;
					++stats.copyCount;
					stats.copySize += written;

					return true;
				}
			}
		}

		if (retryCount-- == 0)
		{
			++stats.failCount;
			logErrorf(L"failed to copy file (%s)", entry.src.c_str());
			return true;
		}

		// Reset last error and try again!
		logContext.resetLastError();
		logInfoLinef(L"Warning - failed to copy file %s to %s, retrying in %i seconds", entry.src.c_str(), fullDst.c_str(), m_settings.retryWaitTimeMs/1000);
		Sleep(m_settings.retryWaitTimeMs);

		++stats.retryCount;
	}

	return true;
}

bool
Client::connectToServer(const wchar_t* networkPath, Connection*& outConnection, bool& failedToConnect, ClientStats& stats)
{
	outConnection = nullptr;
	if (m_settings.useServer == UseServer_Disabled || failedToConnect)
		return true;

	// Set temporary log context to swallow potential errors when trying to connect
	LogContext logContext(*m_log);

	u64 startConnect = getTimeMs();
	stats.serverAttempt = true;
	outConnection = createConnection(networkPath, stats, failedToConnect);
	stats.connectTimeMs += getTimeMs() - startConnect;

	if (failedToConnect && m_settings.useServer == UseServer_Required)
	{
		logErrorf(L"Failed to connect to server hosting %s at port %u", networkPath, m_settings.serverPort);
		return false;
	}
	return true;
}

bool
Client::processFiles(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, bool isMainThread)
{
	logDebugLinef(L"Worker started");

	NetworkCopyContext copyContext;
	CopyStats copyStats;
	uint filesProcessedCount = 0;

	// Process file queue
	while (m_workersActive)
	{
		if (processFile(logContext, sourceConnection, destConnection, copyContext, stats))
			++filesProcessedCount;
		else if (isMainThread)
			break;
	}

	logDebugLinef(L"Worker done - %u file(s) processed", filesProcessedCount);

	return true;
}

int
Client::workerThread(ClientStats& stats)
{
	// Try to connect to server (can fail to connect and still return true if settings allow it to fail)
	Connection* destConnection;
	if (!connectToServer(m_settings.destDirectory.c_str(), destConnection, m_useDestServerFailed, stats))
		return false;
	ScopeGuard destConnectionGuard([&] { delete destConnection; });

	Connection* sourceConnection = nullptr;
	if (!destConnection)
		if (!connectToServer(m_settings.sourceDirectory.c_str(), sourceConnection, m_useSourceServerFailed, stats))
			return false;
	ScopeGuard sourcesConnectionGuard([&] { delete sourceConnection; });


	// Help process the files
	LogContext logContext(*m_log);
	processFiles(logContext, sourceConnection, destConnection, stats, false);

	return logContext.getLastError();
}

bool
Client::handleFile(const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const FileInfo& fileInfo, const HandleFileFunc& handleFileFunc)
{
	const wchar_t* destFileName = fileName;

	// If destination is flatten we remove relative path
	if (m_settings.flattenDestination)
		if (const wchar_t* lastSlash = wcsrchr(fileName, L'\\'))
			destFileName = lastSlash + 1;

	// Chec if file should be excluded because of wild cards
	for (auto& excludeWildcard : m_settings.excludeWildcards)
		if (PathMatchSpecW(destFileName, excludeWildcard.c_str()))
			return true;

	WString destFile = (destPath + destFileName).c_str() + m_settings.destDirectory.size();

	// Keep track of handled files so we don't do duplicated work
	if (!m_handledFiles.insert(destFile).second)
		return true;

	// Call handler now when we've decided to copy file (will create directories on target side etc)
	if (handleFileFunc)
		if (!handleFileFunc())
			return false;

	WString srcFile = sourcePath + fileName;

	// Add entry (workers will pick this up as soon as possible )
	m_copyEntriesCs.enter();
	m_copyEntries.push_back(CopyEntry());
	auto& entry = m_copyEntries.back();
	entry.src = srcFile;
	entry.dst = destFile;
	entry.srcInfo = fileInfo;
	m_copyEntriesCs.leave();
	return true;
}

bool
Client::handleDirectory(const WString& sourcePath, const WString& destPath, const wchar_t* directory, const wchar_t* wildcard, int depthLeft, const HandleFileFunc& handleFileFunc)
{
	WString newSourceDirectory = sourcePath + directory + L'\\';
	WString newDestDirectory = destPath;
	if (!m_settings.flattenDestination && *directory)
		newDestDirectory = newDestDirectory + directory + L'\\';
	
	bool firstFound = m_settings.flattenDestination;

	// Lambda to create directory if decided to write file(s) in there
	auto createDirectoryFunc = [&]()
	{
		if (handleFileFunc)
			if (!handleFileFunc())
				return false;

		if (firstFound)
			return true;
		firstFound = true;

		const wchar_t* relDir = newDestDirectory.c_str() + m_settings.destDirectory.size();
		if (!m_handledFiles.insert(relDir).second)
			return true;

		if (!ensureDirectory(newDestDirectory.c_str()))
			return false;

		if (m_settings.dirCopyFlags != 0)
		{
			// TODO: Implement this
		}

		return true;
	};

	// Create directories regardless if there are files or not in there
	if (m_settings.copyEmptySubdirectories)
		if (!createDirectoryFunc())
			return false;

	// Find files to copy
	return findFilesInDirectory(newSourceDirectory, newDestDirectory, wildcard, depthLeft, createDirectoryFunc);
}

bool
Client::handlePath(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const HandleFileFunc& handleFileFunc)
{
	WString fullFileName = sourcePath;
	if (fileName[0])
		fullFileName += fileName;

	WIN32_FILE_ATTRIBUTE_DATA fd;

	int retryCount = m_settings.retryCount;
	while (true)
	{
		// Get file attributes (and allow retry if fails)
		BOOL ret = GetFileAttributesExW(fullFileName.c_str(), GetFileExInfoStandard, &fd); 
		if (ret != 0)
			break;

		wchar_t errorDesc[1024];

		DWORD error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error || ERROR_PATH_NOT_FOUND == error)
		{
			for (auto& optionalWildcard : m_settings.optionalWildcards)
				if (PathMatchSpecW(fileName, optionalWildcard.c_str()))
					return true;
			for (auto& excludeWildcard : m_settings.excludeWildcards)
				if (PathMatchSpecW(fileName, excludeWildcard.c_str()))
					return true;
			if (m_handledFiles.find(fileName) != m_handledFiles.end())
				return true;
			StringCbPrintfW(errorDesc, eacopy_sizeof_array(errorDesc), L"Can't find file/directory %s", fullFileName.c_str());
		}
		else
			StringCbPrintfW(errorDesc, eacopy_sizeof_array(errorDesc), L"%s getting attributes from file/directory %s", getErrorText(error).c_str(), fullFileName.c_str());

		if (retryCount-- == 0)
		{
			++stats.failCount;
			logErrorf(errorDesc);
			return true;
		}

		// Reset last error and try again!
		logContext.resetLastError();
		logInfoLinef(L"Warning - %s, retrying in %i seconds", errorDesc, m_settings.retryWaitTimeMs/1000);
		Sleep(m_settings.retryWaitTimeMs);

		++stats.retryCount;
	}

	// Handle directory
	if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		if (!handleDirectory(sourcePath, destPath, fileName, L"*.*", m_settings.copySubdirDepth, handleFileFunc))
			return false;
	}
	else //if (fd.dwFileAttributes & FILE_ATTRIBUTE_NORMAL) // Handle file
	{
		FileInfo fileInfo;
		fileInfo.creationTime = { 0, 0 };//fd.ftCreationTime;
		fileInfo.lastWriteTime = fd.ftLastWriteTime;
		fileInfo.fileSize = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;

		// This can happen in certain scenarios when wildcard files are used. The file is actually part of the source path
		if (!*fileName)
		{
			const wchar_t* lastSlash = wcsrchr(sourcePath.c_str(), L'\\');
			if (!lastSlash)
			{
				logErrorf(L"Something went wrong with the file paths. Source: %s Dest: %s", sourcePath.c_str(), destPath.c_str());
				return false;
			}

			WString newSourcePath(sourcePath.c_str(), lastSlash + 1);
			fileName = lastSlash + 1;
			if (!handleFile(newSourcePath, destPath, fileName, fileInfo, handleFileFunc))
				return false;
		}
		else
		{
			if (!handleFile(sourcePath, destPath, fileName, fileInfo, handleFileFunc))
				return false;
		}
	}

	return true;
}

bool
Client::findFilesInDirectory(const WString& sourcePath, const WString& destPath, const WString& wildcard, int depthLeft, const HandleFileFunc& handleFileFunc)
{
    WIN32_FIND_DATAW fd; 
    WString searchStr = sourcePath + wildcard;
    HANDLE hFind = ::FindFirstFileW(searchStr.c_str(), &fd); 
    if(hFind == INVALID_HANDLE_VALUE)
	{
		logErrorf(L"Can't find %s", searchStr.c_str());
		return false;
	}
	ScopeGuard _([&]() { FindClose(hFind); });

    do
	{ 
        if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			FileInfo fileInfo;
			fileInfo.creationTime = { 0, 0 };//fd.ftCreationTime;
			fileInfo.lastWriteTime = fd.ftLastWriteTime;
			fileInfo.fileSize = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
			if (!handleFile(sourcePath, destPath, fd.cFileName, fileInfo, handleFileFunc))
				return false;
        }
		else if (depthLeft)
		{
			if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
				if (!handleDirectory(sourcePath, destPath, fd.cFileName, wildcard.c_str(), depthLeft - 1, handleFileFunc))
					return false;
		}

	}
	while(FindNextFileW(hFind, &fd)); 
        
	return true;
}

bool
Client::handleFilesOrWildcardsFromFile(const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileOrWildcardFunc& func)
{
	WString fullPath;
	if (isAbsolutePath(fileName.c_str()))
		fullPath = fileName;
	else
		fullPath = sourcePath + L'\\' + fileName;

	HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		if (ERROR_FILE_NOT_FOUND == error)
			logErrorf(L"Can't find input file %s", fullPath.c_str());
		else
			logErrorf(L"Error %s. Failed to open input file %s", getErrorText(error).c_str(), fullPath.c_str());
		return false;
	}


	ScopeGuard _([&]() { CloseHandle(hFile); });

	char buffer[2048];
	uint left = 0;

	while (true)
	{
		DWORD read = 0;
		if (ReadFile(hFile, buffer + left, sizeof(buffer) - left - 1, &read, NULL) == 0)
		{
			logErrorf(L"Error %s. Failed reading input file %s", getLastErrorText().c_str(), fullPath.c_str());
			return false;
		}

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
					if (!func(startPos))
						return false;
				}
				consumePos = uint(newLinePos - buffer) + 1;
			}
			else
			{
				// Last line
				if (read == 0 && left != 0)
				{
					if (!func(startPos))
						return false;
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

bool
Client::excludeFilesFromFile(const WString& sourcePath, const WString& fileName, const WString& destPath)
{
	auto executeFunc = [&](char* str) -> bool
	{
		convertSlashToBackslash(str);
		if (strchr(str, '*') != nullptr)
		{
			logErrorf(L"Wildcards not supported in exclude list file %s", fileName.c_str());
			return false;
		}
		m_handledFiles.insert(WString(str, str + strlen(str)));
		return true;
	};

	return handleFilesOrWildcardsFromFile(sourcePath, fileName, destPath, executeFunc);
}

bool
Client::gatherFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& rootSourcePath, const WString& fileName, const WString& rootDestPath, const HandleFileFunc& handleFileFunc)
{
	auto executeFunc = [&](char* str) -> bool
	{
		bool handled = false;
		// Handle initial path that might exist inside input file

		// Parse line to figure out the parts. source [dest [file [file]...]] [options]
		WString wstr(str, str + strlen(str));
		int argc;
		wchar_t** argv = CommandLineToArgvW(wstr.c_str(), &argc);
		ScopeGuard _([&]{ LocalFree(argv); });

		if (argc == 0)
			return true;

		WString sourcePath = rootSourcePath;
		WString destPath = rootDestPath;
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

				if (isAbsolutePath(argv[0]))
					sourcePath = argv[0];
				else
					sourcePath += argv[0];
				wpath.clear();

				destPath += argv[1];
				destPath +=  L"\\";
				ensureDirectory(destPath.c_str());
			}
		}

		// Parse options
		for (int i=optionsStartIndex; i<argc; ++i)
		{
			if (_wcsnicmp(argv[i], L"/PURGE", 6) != 0)
			{
				logErrorf(L"Only '/PURGE' allowed after second separator in file list %s", fileName.c_str());
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
				logErrorf(L"Entry in file list %s is using absolute path %s that is not in source path %s", fileName.c_str(), wpath.c_str(), sourcePath.c_str());
				return false;
			}
		}

		HandleFileFunc tempHandleFileFunc;
		const HandleFileFunc* overriddenHandleFileFunc = &handleFileFunc;
		if (!m_settings.flattenDestination)
		{
			if (const wchar_t* lastSlash = wcsrchr(wpath.c_str(), L'\\'))
			{
				tempHandleFileFunc = [&]()
				{
					if (handled)
						return true;
					handled = true;
					if (!handleFileFunc())
						return false;
					WString relativePath(wpath.c_str(), lastSlash);
					if (!m_handledFiles.insert(relativePath + L'\\').second)
						return true;
					return ensureDirectory((destPath + L'\\' + relativePath).c_str());
				};
				overriddenHandleFileFunc = &tempHandleFileFunc;
			}
		}

		return handlePath(logContext, stats, sourcePath, destPath, wpath.c_str(), *overriddenHandleFileFunc);
	};

	return handleFilesOrWildcardsFromFile(rootSourcePath, fileName, rootDestPath, executeFunc);
}

bool
Client::purgeFilesInDirectory(const WString& path, int depthLeft)
{
	WString relPath;
	if (path.size() > m_settings.destDirectory.size())
		relPath.append(path.c_str() + m_settings.destDirectory.size());

	// If there is a connection and no files were handled inside the directory we can just do a full delete on the server side
	if (m_destConnection)
		if ((relPath.empty() && m_handledFiles.empty()) || m_handledFiles.find(relPath) == m_handledFiles.end())
			return m_destConnection->sendDeleteAllFiles(relPath.c_str());


    WIN32_FIND_DATAW fd; 
    WString searchStr = path + L"*.*";
    HANDLE hFind = ::FindFirstFileW(searchStr.c_str(), &fd); 
    if(hFind == INVALID_HANDLE_VALUE)
	{
		logErrorf(L"FindFirstFile failed with search string %s", searchStr.c_str());
		return false;
	}
	ScopeGuard _([&]() { FindClose(hFind); });

	bool res = true;
    do
	{ 
		// Check if file was copied here
		WString filePath = relPath + fd.cFileName;

		bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (isDir)
		{
			if ((wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0))
				continue;
			filePath += L'\\';
		}

		if (m_handledFiles.find(filePath) == m_handledFiles.end())
		{
			WString fullPath = (path + L'\\' + fd.cFileName);
	        if(isDir)
			{
				if (!deleteDirectory(fullPath.c_str()))
					res = false;
			}
			else
			{
				if (!deleteFile(fullPath.c_str()))
					res = false;
			}
		}
        else if(isDir)
		{
			if (!purgeFilesInDirectory(path + fd.cFileName + L'\\', depthLeft - 1))
				res = false;
		}

	}
	while(FindNextFileW(hFind, &fd)); 
        
	return res;
}

bool
Client::ensureDirectory(const wchar_t* directory)
{
	if (m_destConnection)
	{
		const wchar_t* relDir = directory + m_settings.destDirectory.size();

		// Ask connection to create new directory.
		if (!m_destConnection->sendCreateDirectoryCommand(relDir))
			return false;
	}
	else
	{
		// Create directory through windows api
		if (!eacopy::ensureDirectory(directory, true, m_settings.replaceSymLinksAtDestination))
			return false;
	}

	return true;
}

const wchar_t*
Client::getRelativeSourceFile(const WString& sourcePath) const
{
	const wchar_t* logStr = sourcePath.c_str();
	if (wcsstr(logStr, m_settings.sourceDirectory.c_str()))
		logStr += m_settings.sourceDirectory.size();
	return logStr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Client::Connection*
Client::createConnection(const wchar_t* networkPath, ClientStats& stats, bool& failedToConnect)
{
	u64 startTime = getTimeMs();

	m_networkInitCs.enter();
	ScopeGuard leaveNetworkInitCs([this](){ m_networkInitCs.leave(); });
	if (!m_networkInitDone)
	{
		m_networkInitDone = true;

		const wchar_t* serverNameStart = networkPath + 2;
		const wchar_t* serverNameEnd = wcschr(serverNameStart, L'\\');
		if (!serverNameEnd)
			return nullptr;

		// Initialize Winsock
		WSADATA wsaData;
		int res = WSAStartup(MAKEWORD(2,2), &wsaData);
		if (res != 0)
		{
			logErrorf(L"WSAStartup failed with error: %d", res);
			return nullptr;
		}
		m_networkWsaInitDone = true;

		WString networkServerName(serverNameStart, serverNameEnd);
    
		addrinfoW hints;
		ZeroMemory( &hints, sizeof(hints) );
		hints.ai_family = AF_INET; //AF_UNSPEC; (Skip AF_INET6)
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		wchar_t defaultPortStr[32];
		_itow_s(m_settings.serverPort, defaultPortStr, eacopy_sizeof_array(defaultPortStr), 10);

		// Resolve the server address and port
		res = GetAddrInfoW(networkServerName.c_str(), defaultPortStr, &hints, &m_serverAddrInfo);
		if (res != 0)
		{
			logErrorf(L"GetAddrInfoW failed with error: %d", res);
			return nullptr;
		}

		// Set server name and net directory (this will enable all connections to try to connect)
		m_networkServerName = networkServerName;
		m_networkServerNetDirectory = serverNameEnd + 1;
	}
	else if (m_networkServerName.empty())
		return nullptr;

	leaveNetworkInitCs.execute();

	SOCKET sock = INVALID_SOCKET;

	// Loop through and attempt to connect to an address until one succeeds
	for(auto addrInfoIt=m_serverAddrInfo; addrInfoIt!=NULL; addrInfoIt=addrInfoIt->ai_next)
	{
		// Create a socket for connecting to server
		sock = WSASocketW(addrInfoIt->ai_family, addrInfoIt->ai_socktype, addrInfoIt->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
		if (sock == INVALID_SOCKET)
		{
			logErrorf(L"socket failed with error: %ld", WSAGetLastError());
			return nullptr;
		}

		// Create guard in case we fail to connect (will be cancelled further down if we succeed)
		ScopeGuard socketClose([&]() { closesocket(sock); sock = INVALID_SOCKET; });

		// Set to non-blocking just for the connect call (we want to control the connect timeout after connect using select instead)
		if (!setBlocking(sock, false))
			break;

		// Connect to server.
		int res = connect(sock, addrInfoIt->ai_addr, (int)addrInfoIt->ai_addrlen);
		if (res == SOCKET_ERROR)
		{
			if (WSAGetLastError() != WSAEWOULDBLOCK)
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
		FD_SET(sock, &write);
		FD_SET(sock, &err);

		// check if the socket is ready
		select(0,NULL,&write,&err,&timeval);			
		if(!FD_ISSET(sock, &write)) 
			continue;

		// Socket is good, cancel the socket close scope and break out of the loop.
		socketClose.cancel();
		break;
	}

	u64 endTime = getTimeMs();
	logDebugLinef(L"Connect to server %s. (%.1f seconds)", sock != INVALID_SOCKET ? L"SUCCESS" : L"FAILED", float(endTime - startTime)/1000.0f);

	if (sock == INVALID_SOCKET)
	{
		failedToConnect = true;
		return nullptr;
	}

	ScopeGuard socketCleanup([&]() { closesocket(sock); });

	// Set send buffer size (makes some difference in the tests we've done)
	if (!setSendBufferSize(sock, 4*1024*1024))
		return nullptr;

	// Disable Nagle's algorithm (it is 2019 after all)
	if (!disableNagle(sock))
		return nullptr;

	{
		// Read version command
		char cmdBuf[sizeof(VersionCommand)];
		int cmdBufLen = sizeof(cmdBuf);
		if (!receiveData(sock, cmdBuf, cmdBufLen))
			return nullptr;
		auto& cmd = *(const VersionCommand*)cmdBuf;
		if (cmd.protocolVersion != ProtocolVersion)
		{
			static bool logOnce = true;
			if (logOnce)
			{
				logOnce = false;
				logInfoLinef(L"   !!Protocol mismatch, will not use server. (Local: v%u, Server: v%u)", ProtocolVersion, cmd.protocolVersion);
			}
			failedToConnect = true;
			return nullptr;
		}
	}

	// Connection is ready, cancel socket cleanup and create connection object
	socketCleanup.cancel();
	auto connection = new Connection(m_settings, stats, sock);
	ScopeGuard connectionGuard([&] { delete connection; });

	{
		// Send environment command
		char cmdBuf[MAX_PATH*2 + sizeof(EnvironmentCommand)+1];
		auto& cmd = *(EnvironmentCommand*)cmdBuf;
		cmd.commandType = CommandType_Environment;
		cmd.deltaCompressionThreshold = m_settings.deltaCompressionThreshold;
		cmd.commandSize = sizeof(cmd) + uint(m_networkServerNetDirectory.size()*2);
		
		if (wcscpy_s(cmd.netDirectory, MAX_PATH, m_networkServerNetDirectory.data()))
		{
			logErrorf(L"Failed send environment %s: wcscpy_s", m_networkServerNetDirectory.c_str());
			return false;
		}
	
		if (!connection->sendCommand(cmd))
			return nullptr;
	}

	connectionGuard.cancel();

	return connection;
}

Client::Connection::Connection(const ClientSettings& settings, ClientStats& stats, SOCKET s)
:	m_settings(settings)
,	m_stats(stats)
,	m_socket(s)
{
	m_compressionEnabled = settings.compressionEnabled;
	m_compressionData.fixedLevel = settings.compressionLevel != 0;
	m_compressionData.level = min(max(settings.compressionLevel, 1), 22);
}

Client::Connection::~Connection()
{
	DoneCommand cmd;
	cmd.commandType = CommandType_Done;
	cmd.commandSize = sizeof(cmd);
	sendCommand(cmd);

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
	cmd.commandSize = sizeof(cmd) + uint(wcslen(text));
	if (wcscpy_s(cmd.string, eacopy_sizeof_array(buffer) - sizeof(cmd), text))
	{
		logErrorf(L"wcscpy_s in sendTextCommand failed");
		return false;
	}
	return sendCommand(cmd);
}

bool
Client::Connection::sendWriteFileCommand(const wchar_t* src, const wchar_t* dst, u64& outSize, u64& outWritten, bool& outLinked, CopyContext& copyContext)
{
	outSize = 0;
	outWritten = 0;
	outLinked = false;

	WriteFileType writeType = m_compressionEnabled ? WriteFileType_Compressed : WriteFileType_Send;

	char buffer[MAX_PATH*2 + sizeof(WriteFileCommand)];
	auto& cmd = *(WriteFileCommand*)buffer;
	cmd.commandType = CommandType_WriteFile;
	cmd.writeType = writeType;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dst)*2);
	if (wcscpy_s(cmd.path, MAX_PATH, dst))
	{
		logErrorf(L"Failed to write file %s: wcscpy_s in sendWriteFileCommand failed", dst);
		return false;
	}
	
	if (getFileInfo(cmd.info, src) == 0)
		return false;

	outSize = cmd.info.fileSize;

	if (!sendCommand(cmd))
		return false;

	WriteResponse writeResponse;
	if (!receiveData(m_socket, &writeResponse, sizeof(writeResponse)))
		return false;

	if (writeResponse == WriteResponse_Skip)
		return true;

	if (writeResponse == WriteResponse_Link)
	{
		outWritten = cmd.info.fileSize;
		outLinked = true;
		return true;
	}

	if (writeResponse == WriteResponse_Copy)
	{
		bool useBufferedIO = getUseBufferedIO(m_settings.useBufferedIO, cmd.info.fileSize);

		SendFileStats sendStats;
		if (!sendFile(m_socket, src, cmd.info.fileSize, writeType, copyContext, m_compressionData, useBufferedIO, m_stats.copyStats, sendStats))
			return false;
		m_stats.sendTimeMs += sendStats.sendTimeMs;
		m_stats.sendSize += sendStats.sendSize;
		m_stats.compressTimeMs += sendStats.compressTimeMs;
		m_stats.compressionLevelSum += sendStats.compressionLevelSum;

		u8 writeSuccess;
		if (!receiveData(m_socket, &writeSuccess, sizeof(writeSuccess)))
			return false;

		if (!writeSuccess)
			return false;

		outWritten = cmd.info.fileSize;
		return true;
	}
	
	if (writeResponse == WriteResponse_CopyDelta)
	{
		#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
		RsyncStats rsyncStats;
		if (!clientHandleRsync(m_socket, src, rsyncStats))
			return false;
		m_stats.sendTimeMs += rsyncStats.sendTimeMs;
		m_stats.sendSize += rsyncStats.sendSize;
		m_stats.copyStats.readTimeMs += rsyncStats.readTimeMs;
		//m_stats.copyStats.readSize += rsyncStats.readSize;
		m_stats.deltaCompressionTimeMs += rsyncStats.rsyncTimeMs;

		u8 writeSuccess;
		if (!receiveData(m_socket, &writeSuccess, sizeof(writeSuccess)))
			return false;

		if (!writeSuccess)
			return false;

		outWritten = cmd.info.fileSize;
		return true;
		#endif
		return false;
	}

	if (writeResponse == WriteResponse_BadDestination)
	{
		logErrorf(L"Failed to write file %s: Server reported Bad destination (check your destination path)", src);
		return false;
	}

	return false;
}

bool
Client::Connection::sendReadFileCommand(const wchar_t* src, const wchar_t* dst, u64& outSize, u64& outRead, NetworkCopyContext& copyContext)
{
	outSize = 0;
	outRead = 0;

	// Make src a local path to server
	src += m_settings.sourceDirectory.size();

	char buffer[MAX_PATH*2 + sizeof(ReadFileCommand)];
	auto& cmd = *(ReadFileCommand*)buffer;
	cmd.commandType = CommandType_ReadFile;
	cmd.compressionEnabled = m_compressionEnabled;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(src)*2);
	if (wcscpy_s(cmd.path, MAX_PATH, src))
	{
		logErrorf(L"Failed to read file %s: wcscpy_s in sendReadFileCommand failed", src);
		return false;
	}

	WString fullDest = m_settings.destDirectory + dst;

	if (DWORD fileAttributes = getFileInfo(cmd.info, fullDest.c_str()))
		if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			logErrorf(L"Trying to copy to file %s which is a directory", fullDest.c_str());
			return false;
		}

	if (!sendCommand(cmd))
		return false;

	ReadResponse readResponse;
	if (!receiveData(m_socket, &readResponse, sizeof(readResponse)))
		return false;

	// Skip file, we already have the same file as source
	if (readResponse == ReadResponse_Skip)
	{
		outSize = cmd.info.fileSize;
		return true;
	}

	FILETIME newFileLastWriteTime;
	if (!receiveData(m_socket, &newFileLastWriteTime, sizeof(newFileLastWriteTime)))
		return false;

	if (readResponse == ReadResponse_Copy)
	{
		// Read file size and lastwritetime for new file from server
		u64 newFileSize;
		if (!receiveData(m_socket, &newFileSize, sizeof(newFileSize)))
			return false;


		bool success = true;
		WriteFileType writeType = m_compressionEnabled ? WriteFileType_Compressed : WriteFileType_Send;
		bool useBufferedIO = getUseBufferedIO(m_settings.useBufferedIO, cmd.info.fileSize);
		uint commandSize = 0;

		// Read actual file from server
		if (!receiveFile(success, m_socket, fullDest.c_str(), newFileSize, newFileLastWriteTime, writeType, useBufferedIO, copyContext, nullptr, 0, commandSize))
			return false;

		outRead = newFileSize;
		outSize = newFileSize;
		return success;
	}
	else // ReadResponse_CopyDelta
	{
		#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
		if (!receiveZdelta(m_socket, fullDest.c_str(), fullDest.c_str(), newFileLastWriteTime, copyContext))
			return false;
		return true;
		#endif
	}
	return false;
}

bool
Client::Connection::sendCreateDirectoryCommand(const wchar_t* dst)
{
	char buffer[MAX_PATH*2 + sizeof(CreateDirCommand)+1];
	auto& cmd = *(CreateDirCommand*)buffer;
	cmd.commandType = CommandType_CreateDir;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dst)*2);
	
	if (wcscpy_s(cmd.path, MAX_PATH, dst))
	{
		logErrorf(L"Failed to create folder %s: wcscpy_s in sendCreateDirectoryCommand failed", dst);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	CreateDirResponse createDirResponse;
	if (!receiveData(m_socket, &createDirResponse, sizeof(createDirResponse)))
		return false;

	if (createDirResponse == CreateDirResponse_BadDestination)
	{
		logErrorf(L"Failed to create directory %s: Server reported Bad destination (check your destination path)", dst);
		return false;
	}

	if (createDirResponse == CreateDirResponse_Error)
	{
		logErrorf(L"Failed to create directory %s: Server reported unknown error", dst);
		return false;
	}

	return true;
}

bool
Client::Connection::sendDeleteAllFiles(const wchar_t* dir)
{
	char buffer[MAX_PATH*2 + sizeof(DeleteFilesCommand)+1];
	auto& cmd = *(DeleteFilesCommand*)buffer;
	cmd.commandType = CommandType_DeleteFiles;
	cmd.commandSize = sizeof(cmd) + uint(wcslen(dir)*2);

	if (wcscpy_s(cmd.path, MAX_PATH, dir))
	{
		logErrorf(L"Failed to delete directory %s: wcscpy_s in sendDeleteAllFiles failed", dir);
		return false;
	}

	if (!sendCommand(cmd))
		return false;

	DeleteFilesResponse deleteFilesResponse;
	if (!receiveData(m_socket, &deleteFilesResponse, sizeof(deleteFilesResponse)))
		return false;

	if (deleteFilesResponse == DeleteFilesResponse_BadDestination)
	{
		logErrorf(L"Failed to delete directory %s: Server reported Bad destination (check your destination path)", dir);
		return false;
	}

	if (deleteFilesResponse == DeleteFilesResponse_Error)
	{
		logErrorf(L"Failed to delete directory %s: Server reported unknown error", dir);
		return false;
	}
	return true;
}

bool
Client::Connection::destroy()
{
	ScopeGuard socketCleanup([this]() { closesocket(m_socket); m_socket = INVALID_SOCKET; });

	// shutdown the connection since no more data will be sent
	if (shutdown(m_socket, SD_SEND) == SOCKET_ERROR)
	{
		logErrorf(L"shutdown failed with error: %d", WSAGetLastError());
		return false;
	}

	// Receive until the peer closes the connection
	while (true)
	{
		char recvbuf[512];
		int recvbuflen = sizeof(recvbuf);
		int res = recv(m_socket, recvbuf, recvbuflen, 0);

		if (res == 0)
		{
			//logDebugLinef(L"Connection closed");
			break;
		}
		
		if (res < 0)
		{
			logErrorf(L"recv failed with error: %d", WSAGetLastError());
			return false;
		}
			
		//logDebugLinef("Bytes received: %d", res);

	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
