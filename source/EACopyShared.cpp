// (c) Electronic Arts. All Rights Reserved.

#include "EACopyShared.h"
#include <assert.h>
#include <codecvt>
#include <shlwapi.h>
#include <strsafe.h>
#include "RestartManager.h"
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Rstrtmgr.lib")

//#define EACOPY_USE_OUTPUTDEBUGSTRING
#define EACOPY_IS_DEBUGGER_PRESENT false//::IsDebuggerPresent()

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Constants. Tweaked to give best performance in tested scenarios
enum { UseOwnCopyFunction = true };
enum { UseOverlappedCopy = false };
enum { CopyFileWriteThrough = false }; // Enabling this makes all tests slower in our test environment

enum { NoBufferingIOUseTreshold = false }; // Enabling this makes all tests slower in our test environment
enum { NoBufferingIOTreshold = 16 * 1024 * 1024 }; // Treshold for when unbuffered io is enabled if UseBufferedIO_Auto is used

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

thread_local LogContext* t_logContext;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Thread::Thread(Function<int()>&& f)
:	func(std::move(f))
{
	handle = CreateThread(NULL, 0, [](LPVOID p) -> DWORD { return ((Thread*)p)->func(); }, this, 0, NULL);
}

Thread::~Thread()
{
	wait();
	CloseHandle(handle);
}

void
Thread::wait()
{
	WaitForSingleObject(handle, INFINITE);
}

bool
Thread::getExitCode(DWORD& outExitCode)
{
	if (GetExitCodeThread(handle, &outExitCode))
		return true;

	logErrorf(L"Failed to get exit code from thread");
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

LogContext::LogContext(Log& l)
:	log(l)
{
	m_lastContext = t_logContext;
	t_logContext = this;
}

LogContext::~LogContext()
{
	t_logContext = m_lastContext;
}

CriticalSection g_logCs;

void Log::writeEntry(bool isDebuggerPresent, const LogEntry& entry)
{
	#if defined(EACOPY_USE_OUTPUTDEBUGSTRING)
	if (isDebuggerPresent)
		OutputDebugStringW(entry.c_str());
	#endif

	if (m_logFile != INVALID_HANDLE_VALUE)
	{
		char buffer[2048];
		BOOL usedDefaultChar = FALSE;
		int bufferWritten = WideCharToMultiByte(CP_ACP, 0, entry.str.c_str(), (int)entry.str.size(), buffer, sizeof(buffer), NULL, &usedDefaultChar);
		buffer[bufferWritten] = 0;
		DWORD written;
		if (entry.linefeed)
		{
			if (strcat_s(buffer + bufferWritten, sizeof(buffer)-bufferWritten, "\r\n") == 0)
				bufferWritten += 2;
		}
		WriteFile(m_logFile, buffer, (DWORD)bufferWritten, &written, nullptr);
	}
	else
	{
		g_logCs.enter();
		fputws(entry.str.c_str(), stdout);
		if (entry.linefeed)
			fputws(L"\n", stdout);
		g_logCs.leave();
	}

	if (entry.isError && m_cacheRecentErrors)
	{
		g_logCs.enter();
		if (m_recentErrors.size() > 10)
			m_recentErrors.pop_back();
		m_recentErrors.push_front(entry.str.c_str());
		g_logCs.leave();
	}
}

uint Log::processLogQueue(bool isDebuggerPresent)
{
	List<LogEntry> temp;
	m_logQueueCs.enter();
	bool flush = m_logQueueFlush;
	temp.swap(*m_logQueue);
	m_logQueueFlush = false;
	m_logQueueCs.leave();
	if (temp.empty())
		return 0;

	for (auto& entry : temp)
		writeEntry(isDebuggerPresent, entry);

	if (!flush)
		return (uint)temp.size();

	if (m_logFile != INVALID_HANDLE_VALUE)
		FlushFileBuffers(m_logFile);
	else
		fflush(stdout);

	return (uint)temp.size();
}

DWORD Log::logQueueThread()
{
	bool isDebuggerPresent = EACOPY_IS_DEBUGGER_PRESENT;

	if (!m_logFileName.empty())
		m_logFile = CreateFileW(m_logFileName.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	while (m_logThreadActive)
		if (!processLogQueue(isDebuggerPresent))
			Sleep(5);

	return 0;
}

void Log::init(const wchar_t* logFile, bool logDebug, bool cacheRecentErrors)
{
	m_logDebug = logDebug;
	m_cacheRecentErrors = cacheRecentErrors;
	m_logFileName = logFile ? logFile : L"";
	m_logQueueCs.enter();
	m_logQueue = new List<LogEntry>();
	m_logThreadActive = true;
	m_logThread = new Thread([this]() { return logQueueThread(); });
	m_logQueueCs.leave();
}

void Log::deinit(const Function<void()>& lastChanceLogging)
{
	bool isDebuggerPresent = EACOPY_IS_DEBUGGER_PRESENT;

	m_logQueueCs.enter();
	m_logThreadActive = false;
	m_logQueueCs.leave();
	
	delete m_logThread;

	m_logQueueCs.enter();
	processLogQueue(isDebuggerPresent);
	if (lastChanceLogging)
	{
		lastChanceLogging();
		processLogQueue(isDebuggerPresent);
	}
	delete m_logQueue;
	m_logQueue = nullptr;
	CloseHandle(m_logFile);
	m_logFile = INVALID_HANDLE_VALUE;
	m_logQueueCs.leave();
}

void Log::traverseRecentErrors(const Function<bool(const WString&)>& errorFunc)
{
		g_logCs.enter();
		for (auto& err : m_recentErrors)
			errorFunc(err);
		g_logCs.leave();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void logInternal(const wchar_t* buffer, bool flush, bool linefeed, bool isError)
{
	if (LogContext* context = t_logContext)
	{
		Log& log = context->log;
		log.m_logQueueCs.enter();
		if (log.m_logQueue)
		{
			log.m_logQueue->push_back({buffer, linefeed, isError});
			log.m_logQueueFlush |= flush;
		}
		log.m_logQueueCs.leave();
	}
	else
	{
		g_logCs.enter();
		fputws(buffer, stdout);
		if (linefeed)
			fputws(L"\n", stdout);

		if (flush)
			fflush(stdout);
		#if defined(EACOPY_USE_OUTPUTDEBUGSTRING)
		if (EACOPY_IS_DEBUGGER_PRESENT)
			OutputDebugStringW(buffer);
		#endif
		g_logCs.leave();
	}
}

void logErrorf(const wchar_t* fmt, ...)
{
    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	wcscpy_s(buffer, eacopy_sizeof_array(buffer), L"!!ERROR - ");
	auto len = wcslen(buffer);
	int written = vswprintf_s(buffer + len, eacopy_sizeof_array(buffer) - len, fmt, arg);
	logInternal(buffer, true, true, true);
	va_end(arg);
	if (LogContext* c = t_logContext)
		c->m_lastError = -1;
}

void logInfof(const wchar_t* fmt, ...)
{
    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	vswprintf_s(buffer, eacopy_sizeof_array(buffer), fmt, arg);
	logInternal(buffer, false, false, false);
	va_end(arg);
}

void logInfoLinef(const wchar_t* fmt, ...)
{
    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	int count = vswprintf_s(buffer, eacopy_sizeof_array(buffer), fmt, arg);
	if (count)
		logInternal(buffer, false, true, false);
	va_end(arg);
}

void logInfoLinef()
{
	logInternal(L"", false, true, false);
}

void logDebugf(const wchar_t* fmt, ...)
{
	if (LogContext* c = t_logContext)
		if (!c->log.isDebug())
			return;

    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	vswprintf_s(buffer, eacopy_sizeof_array(buffer), fmt, arg);
	logInternal(buffer, false, false, false);
	va_end(arg);
}

void logDebugLinef(const wchar_t* fmt, ...)
{
	if (LogContext* c = t_logContext)
		if (!c->log.isDebug())
			return;

    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	vswprintf_s(buffer, eacopy_sizeof_array(buffer), fmt, arg);
	logInternal(buffer, false, true, false);
	va_end(arg);
}

void logScopeEnter()
{
	if (LogContext* c = t_logContext)
		c->log.m_logQueueCs.enter();
}

void logScopeLeave()
{
	if (LogContext* c = t_logContext)
		c->log.m_logQueueCs.leave();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

DWORD getFileInfo(FileInfo& outInfo, const wchar_t* fullFileName)
{
	WIN32_FILE_ATTRIBUTE_DATA fd;
	BOOL ret = GetFileAttributesExW(fullFileName, GetFileExInfoStandard, &fd); 
	if (ret == 0)
	{
		outInfo = FileInfo();
		return 0;
	}

	outInfo.creationTime = { 0, 0 }; //fd.ftCreationTime;
	outInfo.lastWriteTime = fd.ftLastWriteTime;
	outInfo.fileSize = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
	return fd.dwFileAttributes;
}

bool equals(const FileInfo& a, const FileInfo& b)
{
	return memcmp(&a, &b, sizeof(FileInfo)) == 0;
}

bool replaceIfSymLink(const wchar_t* directory, DWORD attributes)
{
	bool isDir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if (!isDir)
	{
		logErrorf(L"Trying to treat file as directory %s: %s", directory, getLastErrorText().c_str());
		return false;
	}

	bool isSymlink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	if (!isSymlink)
		return true;

	// Delete reparsepoint and treat path as not existing
	if (!RemoveDirectoryW(directory))
	{
		logErrorf(L"Trying to remove reparse point while ensuring directory %s: %s", directory, getLastErrorText().c_str());
		return false;
	}

	if (CreateDirectoryW(directory, NULL) != 0)
		return true;

	logErrorf(L"Error creating directory %s: %s", directory, getLastErrorText().c_str());
	return false;
}

bool ensureDirectory(const wchar_t* directory, bool replaceIfSymlink, bool expectCreationAndParentExists)
{
	// This is an optimization to reduce kernel calls
	if (expectCreationAndParentExists)
	{
		if (CreateDirectoryW(directory, NULL) != 0)
			return true;
		if (GetLastError() == ERROR_ALREADY_EXISTS) 
		{
			FileInfo dirInfo;
			DWORD destDirAttributes = getFileInfo(dirInfo, directory);
			if (!(destDirAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				logErrorf(L"Trying to treat file as directory %s", directory);
				return false;
			}

			if (replaceIfSymlink)
				if (!replaceIfSymLink(directory, destDirAttributes))
					return false;
			return true;
		}
	}

	WString temp;
	size_t pathLen = wcslen(directory);
	while (directory[pathLen-1] == '\\')
	{
		temp.assign(directory, directory + pathLen - 1);
		directory = temp.c_str();
	}

	const wchar_t* lastBackslash = wcsrchr(directory, '\\');
	if (!lastBackslash)
	{
		logErrorf(L"Error validating directory %s: Bad format.. must contain a slash", directory);
		return false;
	}

	FileInfo dirInfo;
	DWORD destDirAttributes = getFileInfo(dirInfo, directory);
	if (destDirAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		if (replaceIfSymlink)
			if (!replaceIfSymLink(directory, destDirAttributes))
				return false;
		return true;
	}
	else
	{
		if (destDirAttributes != 0)
		{
			logErrorf(L"Trying to treat file as directory %s", directory);
			return false;
		}
		DWORD error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
		{
			logErrorf(L"Error getting attributes from directory %s: %s", directory, getErrorText(error).c_str());
			return false;
		}
	}

	if (lastBackslash)
	{
		WString shorterDirectory(directory, 0, lastBackslash - directory);
		if (!ensureDirectory(shorterDirectory.c_str(), false, false))
			return false;
	}

	if (CreateDirectoryW(directory, NULL) != 0)
		return true;

	DWORD error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS) 
		return true;

	logErrorf(L"Error creating directory %s: %s", directory, getErrorText(error).c_str());
	return false;
}

bool deleteAllFiles(const wchar_t* directory, bool& outPathFound)
{
	outPathFound = true;
    WIN32_FIND_DATAW fd; 
	WString dir(directory);
	dir += L'\\';
    WString searchStr = dir + L"*.*";
    HANDLE hFind = ::FindFirstFileW(searchStr.c_str(), &fd); 
    if(hFind == INVALID_HANDLE_VALUE)
	{
		DWORD error = GetLastError();
		if (ERROR_PATH_NOT_FOUND == error)
		{
			outPathFound = false;
			return true;
		}

		logErrorf(L"deleteDirectory failed using FindFirstFile for directory %s: %s", directory, getErrorText(error).c_str());
		return false;
	}

	ScopeGuard closeFindGuard([&]() { FindClose(hFind); });

    do
	{ 
        if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			WString fullName(dir + fd.cFileName);
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				if (!SetFileAttributesW(fullName.c_str(), FILE_ATTRIBUTE_NORMAL))
					return false;

			if (!deleteFile(fullName.c_str()))
				return false;
		}
		else if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
		{
			WString fullName(dir + fd.cFileName);
			bool isSymlink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
			if (isSymlink)
			{
				// Delete reparsepoint and treat path as not existing
				if (!RemoveDirectoryW(fullName.c_str()))
				{
					logErrorf(L"Trying to remove reparse point while ensuring directory %s: %s", directory, getLastErrorText().c_str());
					return false;
				}
			}
			else if (!deleteDirectory(fullName.c_str()))
				return false;
		}

	}
	while(FindNextFileW(hFind, &fd)); 

	closeFindGuard.execute(); // Need to close find handle otherwise RemoveDirectory will fail now and then

	return true;
}

bool deleteAllFiles(const wchar_t* directory)
{
	bool pathFound;
	return deleteAllFiles(directory, pathFound);
}

bool deleteDirectory(const wchar_t* directory)
{
	bool outPathFound;
	if (!deleteAllFiles(directory, outPathFound))
		return false;

	if (!outPathFound)
		return true;

	if (!RemoveDirectoryW(directory))
	{
		logErrorf(L"Trying to remove directory  %s: %s", directory, getLastErrorText().c_str());
		return false;
	}

	return true;
}

bool isAbsolutePath(const wchar_t* path)
{
	if (wcslen(path) < 3)
		return false;
	return path[1] == ':' || (path[0] == '\\' && path[1] == '\\');
}

bool getUseBufferedIO(UseBufferedIO use, u64 fileSize)
{
	switch (use)
	{
	case UseBufferedIO_Enabled:
		return true;
	case UseBufferedIO_Disabled:
		return false;
	default: // UseBufferedIO_Auto:
		return NoBufferingIOUseTreshold ? fileSize < NoBufferingIOTreshold : true;
	}
}

bool openFileRead(const wchar_t* fullPath, HANDLE& outFile, bool useBufferedIO, OVERLAPPED* overlapped, bool isSequentialScan)
{
	DWORD nobufferingFlag = useBufferedIO ? 0 : FILE_FLAG_NO_BUFFERING;
	DWORD sequentialScanFlag = isSequentialScan ? FILE_FLAG_SEQUENTIAL_SCAN : 0;

	outFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, sequentialScanFlag | nobufferingFlag, overlapped);
	if (outFile != INVALID_HANDLE_VALUE)
		return true;

	logErrorf(L"Failed to open file %s: %s", fullPath, getErrorText(fullPath, GetLastError()).c_str());
	return false;

}

bool openFileWrite(const wchar_t* fullPath, HANDLE& outFile, bool useBufferedIO, OVERLAPPED* overlapped)
{
	DWORD nobufferingFlag = useBufferedIO ? 0 : FILE_FLAG_NO_BUFFERING;
	DWORD writeThroughFlag = CopyFileWriteThrough ? FILE_FLAG_WRITE_THROUGH : 0;

	DWORD flagsAndAttributes = FILE_ATTRIBUTE_NORMAL | nobufferingFlag | writeThroughFlag;
	if (overlapped)
		flagsAndAttributes |= FILE_FLAG_OVERLAPPED;

	outFile = CreateFileW(fullPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, flagsAndAttributes, NULL);
	if (outFile != INVALID_HANDLE_VALUE)
		return true;
	logErrorf(L"Trying to create file %s: %s", fullPath, getErrorText(fullPath, GetLastError()).c_str());
	return false;
}

bool writeFile(const wchar_t* fullPath, HANDLE& file, const void* data, u64 dataSize, OVERLAPPED* overlapped)
{
	if (overlapped)
	{
		assert(dataSize < u64(INT_MAX-1));
		DWORD written;
		if (!WriteFile(file, data, (uint)dataSize, &written, overlapped))
			return false;
		return true;
	}

	u64 left = dataSize;
	while (left)
	{
		DWORD written;
		uint toWrite = (uint)min(left, u64(INT_MAX-1));
		if (!WriteFile(file, data, toWrite, &written, nullptr))
		{
			logErrorf(L"Trying to write data to %s", fullPath);
			CloseHandle(file);
			file = INVALID_HANDLE_VALUE;
			return false;
		}
		(char*&)data += written;
		left -= written;
	}
	return true;
}

bool setFileLastWriteTime(const wchar_t* fullPath, HANDLE& file, FILETIME lastWriteTime)
{
	if (file == INVALID_HANDLE_VALUE)
		return false;

	// This should not be needed!
	//if (!FlushFileBuffers(file))
	//{
	//	logErrorf(L"Failed flushing buffer for file %s: %s", fullPath, getLastErrorText().c_str());
	//	return false;
	//}

	if (SetFileTime(file, NULL, NULL, &lastWriteTime))
		return true;

	logErrorf(L"Failed to set file time on %s", fullPath);
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	return false;
}

bool setFilePosition(const wchar_t* fullPath, HANDLE& file, u64 position)
{
	LARGE_INTEGER li;
	li.QuadPart = position;

	if (SetFilePointer(file, li.LowPart, &li.HighPart, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
		return true;
	logErrorf(L"Fail setting file position on file %s: %s", fullPath, getLastErrorText().c_str());
	return false;
}

bool closeFile(const wchar_t* fullPath, HANDLE& file)
{
	if (file == INVALID_HANDLE_VALUE)
		return true;

	bool success = CloseHandle(file) != 0;
	file = INVALID_HANDLE_VALUE;

	if (!success)
		logErrorf(L"Failed to close file %s", fullPath);

	return success;
}

bool createFile(const wchar_t* fullPath, const FileInfo& info, const void* data, bool useBufferedIO)
{
	HANDLE file;
	if (!openFileWrite(fullPath, file, useBufferedIO))
		return false;

	if (!writeFile(fullPath, file, data, info.fileSize))
		return false;

	if (!setFileLastWriteTime(fullPath, file, info.lastWriteTime))
		return false;

	return closeFile(fullPath, file);
}

bool createFileLink(const wchar_t* fullPath, const FileInfo& info, const wchar_t* sourcePath, bool& outSkip)
{
	outSkip = false;
	if (CreateHardLinkW(fullPath, sourcePath, NULL))
		return true;

	DWORD error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS)
	{
		FileInfo other;
		DWORD attributes = getFileInfo(other, fullPath);
		if (!equals(info, other))
			return false;
		outSkip = true;
		return true;
	}
	logDebugLinef(L"Failed creating hardlink on %s", sourcePath);
	return false;
}

DWORD internalCopyProgressRoutine(LARGE_INTEGER TotalFileSize, LARGE_INTEGER TotalBytesTransferred, LARGE_INTEGER StreamSize, LARGE_INTEGER StreamBytesTransferred, DWORD dwStreamNumber, DWORD dwCallbackReason, HANDLE hSourceFile, HANDLE hDestinationFile, LPVOID lpData)
{
	if (TotalBytesTransferred.QuadPart == TotalFileSize.QuadPart)
		*(u64*)lpData = TotalBytesTransferred.QuadPart;
	return PROGRESS_CONTINUE;
}

CopyContext::CopyContext()
{
	for (uint i=0; i!=3; ++i)
		buffers[i] = new u8[CopyContextBufferSize];
}

CopyContext::~CopyContext()
{
	for (uint i=0; i!=3; ++i)
		delete[] buffers[i];
}

bool copyFile(const wchar_t* source, const wchar_t* dest, bool failIfExists, bool& outExisted, u64& outBytesCopied, UseBufferedIO useBufferedIO)
{
	CopyContext copyContext;
	CopyStats copyStats;
	return copyFile(source, dest, failIfExists, outExisted, outBytesCopied, copyContext, copyStats, useBufferedIO);
}

bool copyFile(const wchar_t* source, const wchar_t* dest, bool failIfExists, bool& outExisted, u64& outBytesCopied, CopyContext& copyContext, CopyStats& copyStats, UseBufferedIO useBufferedIO)
{
	outExisted = false;

	if (UseOwnCopyFunction)
	{
		enum { ReadChunkSize = 2 * 1024 * 1024 };
		static_assert(ReadChunkSize <= CopyContextBufferSize, "ReadChunkSize must be smaller than CopyContextBufferSize");

		FileInfo sourceInfo;
		DWORD sourceAttributes = getFileInfo(sourceInfo, source);
		if (!sourceAttributes)
			return false;
		if ((sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return false;

		DWORD overlappedFlag = UseOverlappedCopy ? FILE_FLAG_OVERLAPPED : 0;
		DWORD nobufferingFlag = getUseBufferedIO(useBufferedIO, sourceInfo.fileSize) ? 0 : FILE_FLAG_NO_BUFFERING;
		DWORD writeThroughFlag = CopyFileWriteThrough ? FILE_FLAG_WRITE_THROUGH : 0;

		OVERLAPPED osRead  = {0,0,0};
		osRead.Offset = 0;
		osRead.OffsetHigh = 0;
		osRead.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

		u64 startCreateReadTimeMs = getTimeMs();
		HANDLE sourceFile = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | overlappedFlag | nobufferingFlag, &osRead);
		copyStats.createReadTimeMs += getTimeMs() - startCreateReadTimeMs;

		if (sourceFile == INVALID_HANDLE_VALUE)
		{
			logErrorf(L"Failed to open file %s for read: %s", source, getErrorText(source, GetLastError()).c_str());
			return false;
		}

		ScopeGuard sourceGuard([&]() { CloseHandle(osRead.hEvent); CloseHandle(sourceFile); });

		OVERLAPPED osWrite = {0,0,0};
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;
		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);

		u64 startCreateWriteTimeMs = getTimeMs();
		HANDLE destFile = CreateFileW(dest, FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES, 0, NULL, failIfExists ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | overlappedFlag | nobufferingFlag | writeThroughFlag, &osWrite);
		copyStats.createWriteTimeMs += getTimeMs() - startCreateWriteTimeMs;

		if (destFile == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			if (ERROR_FILE_EXISTS == error)
			{
				outExisted = true;
				return false;
			}

			logErrorf(L"Failed to create file %s: %s", dest, getErrorText(dest, error).c_str());
			return false;
		}
		ScopeGuard destGuard([&]() { CloseHandle(osWrite.hEvent); CloseHandle(destFile); });

		uint activeBufferIndex = 0;
		uint sizeFilled = 0;
		u8* bufferFilled = nullptr;

		u64 left = sourceInfo.fileSize;
		u64 read = 0;
		while (true)
		{
			if (sizeFilled)
			{
				u64 startWriteMs = getTimeMs();

				if (UseOverlappedCopy && WaitForSingleObject(osWrite.hEvent, INFINITE) != WAIT_OBJECT_0)
				{
					logErrorf(L"WaitForSingleObject failed on write");
					return false;
				}

				DWORD written = 0;
				DWORD toWrite = nobufferingFlag ? (((sizeFilled + 4095) / 4096) * 4096) : sizeFilled;
				if (!WriteFile(destFile, bufferFilled, toWrite, &written, &osWrite))
				{
					if (GetLastError() != ERROR_IO_PENDING)
					{
						logErrorf(L"Fail writing file %s: %s", dest, getLastErrorText().c_str());
						return false;
					}
				}

				copyStats.writeTimeMs += getTimeMs() - startWriteMs;
			}

			sizeFilled = (uint)osRead.InternalHigh;
			bufferFilled = copyContext.buffers[activeBufferIndex];

			if (osRead.InternalHigh)
			{
				if (UseOverlappedCopy && WaitForSingleObject(osRead.hEvent, INFINITE) != WAIT_OBJECT_0)
				{
					logErrorf(L"WaitForSingleObject failed on read");
					return false;
				}
			}

			if (left)
			{
				u64 startReadMs = getTimeMs();

				uint toRead = (uint)min(left, ReadChunkSize);
				activeBufferIndex = (activeBufferIndex + 1) % 3;

				uint toReadAligned = nobufferingFlag ? (((toRead + 4095) / 4096) * 4096) : toRead;

				osRead.Offset = (LONG)read;
				osRead.OffsetHigh = (LONG)(read >> 32);

				if (!ReadFile(sourceFile, copyContext.buffers[activeBufferIndex], toReadAligned, NULL, &osRead))
				{
					if (GetLastError() != ERROR_IO_PENDING)
					{
						logErrorf(L"Fail reading file %s: %s", source, getLastErrorText().c_str());
						return false;
					}
				}

				read += toRead;
				left -= toRead;

				copyStats.readTimeMs += getTimeMs() - startReadMs;
			}
			else
			{
				if (!osRead.InternalHigh)
				{
					if (UseOverlappedCopy && WaitForSingleObject(osWrite.hEvent, INFINITE) != WAIT_OBJECT_0)
					{
						logErrorf(L"WaitForSingleObject failed on last write");
						return false;
					}
					break;
				}
				osRead.InternalHigh = 0;
			}
		}

		u64 startSetLastWriteTimeTimeMs = getTimeMs();
		if (!setFileLastWriteTime(dest, destFile, sourceInfo.lastWriteTime))
			return false;
		copyStats.setLastWriteTimeTimeMs += getTimeMs() - startSetLastWriteTimeTimeMs;

		if (nobufferingFlag)
		{
			LONG lowSize = (LONG)sourceInfo.fileSize;
			LONG highSize = sourceInfo.fileSize >> 32;
			SetFilePointer(destFile, lowSize, &highSize, FILE_BEGIN);
			SetEndOfFile(destFile);
		}

		{
			u64 startReadMs = getTimeMs();
			CloseHandle(sourceFile);
			sourceFile = INVALID_HANDLE_VALUE;
			copyStats.readTimeMs += getTimeMs() - startReadMs;
		}

		{
			u64 startWriteMs = getTimeMs();
			CloseHandle(destFile);
			destFile = INVALID_HANDLE_VALUE;
			copyStats.writeTimeMs += getTimeMs() - startWriteMs;
		}

		outBytesCopied = sourceInfo.fileSize;

		//copyStats.copyTimeMs += getTimeMs() - startCopyTimeMs;

		return true;
	}
	else
	{
		BOOL cancel = false;
		DWORD flags = 0;
		if (failIfExists)
			flags |= COPY_FILE_FAIL_IF_EXISTS;
		if (CopyFileExW(source, dest, (LPPROGRESS_ROUTINE)internalCopyProgressRoutine, &outBytesCopied, &cancel, flags) != 0)
			return true;

		DWORD error = GetLastError();
		if (ERROR_FILE_EXISTS == error)
		{
			outExisted = true;
			return false;
		}

		logErrorf(L"Failed to copy file %s to %s. Reason: %s", source, dest, getErrorText(error).c_str());
		return false;
	}
}

bool deleteFile(const wchar_t* fullPath)
{
	if (DeleteFileW(fullPath) != 0)
		return true;

	DWORD error = GetLastError();
	if (ERROR_FILE_NOT_FOUND == error)
	{
		logErrorf(L"File not found. Failed to delete file %s", fullPath);
		return false;
	}
	logErrorf(L"Failed to delete file %s. Reason: ", fullPath, getErrorText(error).c_str());
	return false;
}

void convertSlashToBackslash(wchar_t* path)
{
	convertSlashToBackslash(path, wcslen(path));
}

void convertSlashToBackslash(wchar_t* path, size_t size)
{
	for (uint i=0; i!=size; ++i, ++path)
		if (*path == L'/')
			*path = L'\\';
}

void convertSlashToBackslash(char* path)
{
	convertSlashToBackslash(path, strlen(path));
}

void convertSlashToBackslash(char* path, size_t size)
{
	for (uint i=0; i!=size; ++i, ++path)
		if (*path == '/')
			*path = '\\';
}

u64 getTimeMs()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	LARGE_INTEGER li;
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;
	return (li.QuadPart - 116444736000000000LL) / 10000;
}

bool equalsIgnoreCase(const wchar_t* a, const wchar_t* b)
{
	return _wcsicmp(a, b) == 0;
}

bool startsWithIgnoreCase(const wchar_t* str, const wchar_t* substr)
{
	return StrStrIW(str, substr) == str;
}

WString getErrorText(uint error)
{
	DWORD dwRet;
	wchar_t* lpszTemp = NULL;

	dwRet = FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |FORMAT_MESSAGE_ARGUMENT_ARRAY,
							NULL, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (LPWSTR)&lpszTemp, 0, NULL);

	if (!lpszTemp)
		return WString();

	// Remove line feed in the end
	if (dwRet > 0 && lpszTemp[dwRet-1] == L'\n')
		lpszTemp[--dwRet] = 0;
	if (dwRet > 0 && lpszTemp[dwRet-1] == L'\r')
		lpszTemp[--dwRet] = 0;

	WString res = lpszTemp;
	LocalFree((HLOCAL)lpszTemp);
	return std::move(res);
}

WString	getErrorText(const wchar_t* resourceName, uint error)
{
	WString errorText = getErrorText(error);
	if (error == ERROR_SHARING_VIOLATION)
	{
		WString processes = getProcessesUsingResource(resourceName);
		if (!processes.empty())
			return L"Already in use by " + processes;
	}

	return getErrorText(error);
}

WString getLastErrorText()
{
	return getErrorText(GetLastError());
}

WString getProcessesUsingResource(const wchar_t* resourceName)
{
	DWORD dwSession;
	WCHAR szSessionKey[CCH_RM_SESSION_KEY+1] = { 0 };
	if (DWORD dwError = RmStartSession(&dwSession, 0, szSessionKey))
		return L"";
	ScopeGuard sessionGuard([&]() { RmEndSession(dwSession); });

	if (DWORD dwError = RmRegisterResources(dwSession, 1, &resourceName, 0, NULL, 0, NULL))
		return L"";

	WString result;

	DWORD dwReason;
	uint nProcInfoNeeded;
	uint nProcInfo = 10;
	RM_PROCESS_INFO rgpi[10];
	if (DWORD dwError = RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, rgpi, &dwReason))
		return L"";

	for (uint i = 0; i < nProcInfo; i++)
	{
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rgpi[i].Process.dwProcessId);
		if (hProcess == INVALID_HANDLE_VALUE)
			continue;
		ScopeGuard processGuard([&]() { CloseHandle(hProcess); });

		FILETIME ftCreate, ftExit, ftKernel, ftUser;
		if (!GetProcessTimes(hProcess, &ftCreate, &ftExit, &ftKernel, &ftUser) && CompareFileTime(&rgpi[i].Process.ProcessStartTime, &ftCreate) == 0)
			continue;
		WCHAR sz[MaxPath];
		DWORD cch = MaxPath;
		if (!QueryFullProcessImageNameW(hProcess, 0, sz, &cch))
			continue;

		if (!result.empty())
			result += L", ";
		result += sz;
	}

	return std::move(result);
}

WString toPretty(u64 bytes, uint alignment)
{
	auto fmtValue = [](u64 v) -> float { auto d = (double)v; int it = 3; while (d > 1000.0 && it--) d /= 1000.0; return (float)d; };
	auto fmtSuffix = [](u64 v) -> const wchar_t* { return v > 1000*1000*1000 ? L"g" : (v > 1000*1000 ? L"m" : (v > 1000 ? L"k" : L"b")); };
	float value = fmtValue(bytes);
	const wchar_t* suffix = fmtSuffix(bytes);

	wchar_t buffer[128];
	wchar_t* dest = buffer + 64;
	StringCbPrintfW(dest, 64, L"%.1f%s", value, suffix);

	uint len = (uint)wcslen(dest);
	while (len <= alignment)
	{
		--dest;
		*dest = L' ';
		++len;
	}
	return dest;
}

WString toHourMinSec(u64 timeMs, uint alignment)
{
	u64 timeSec = timeMs / 1000;
	u64 days = timeSec / (24*60*60);
	timeSec -= days * (24*60*60);
	u64 hours = timeSec / (60*60);
	timeSec -= hours * (60*60);
	u64 minutes = timeSec / (60);
	timeSec -= minutes * (60);
	u64 seconds = timeSec;

	wchar_t buffer[128];
	wchar_t* dest = buffer + 64;

	if (timeMs < 100)
	{

		StringCbPrintfW(dest, 64, L"%ums", uint(timeMs));
	}
	else
	if (timeMs < 60*1000)
	{

		StringCbPrintfW(dest, 64, L"%.2fs", float(timeMs)/1000.0f);
	}
	else if (!days)
	{
		StringCbPrintfW(dest, 64, L"%02u:%02u:%02u", hours, minutes, seconds);
	}
	else
	{
		wchar_t* printfDest = dest;
		_itow_s((int)days, printfDest, 64, 10);
		wcscat_s(printfDest, 64, L"d ");
		printfDest += wcslen(printfDest);

		StringCbPrintfW(printfDest, 64, L"%02u:%02u", hours, minutes);
	}

	uint len = (uint)wcslen(dest);
	while (len <= alignment)
	{
		--dest;
		*dest = L' ';
		++len;
	}

	return dest;
}

String toString(const wchar_t* str)
{
	using convert_type = std::codecvt_utf8<wchar_t>;
	return std::wstring_convert<convert_type, wchar_t>().to_bytes( str );
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
