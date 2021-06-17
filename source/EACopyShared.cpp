// (c) Electronic Arts. All Rights Reserved.

#include "EACopyShared.h"
#include <utility>
#include <codecvt>
#include <assert.h>
#if defined(_WIN32)
#define NOMINMAX
#include <shlwapi.h>
#include <strsafe.h>
#include <windows.h>
#include <wincrypt.h>
#include "RestartManager.h"
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Rstrtmgr.lib")
#else
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>   // open
#include <limits.h>
#include <locale>
#include <stdarg.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>
#include <pthread.h>
#endif

//#define EACOPY_USE_OUTPUTDEBUGSTRING
#define EACOPY_IS_DEBUGGER_PRESENT false//::IsDebuggerPresent()

// Use this to use symlinks as workaround to handle long paths... all cases have probably not been tested yet
//#define EACOPY_USE_SYMLINK_FOR_LONGPATHS

#if defined(_WIN32)
#else
namespace eacopy {
void Sleep(uint milliseconds)
{
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    int res;
    do { res = nanosleep(&ts, &ts); } while (res && errno == EINTR);
}
thread_local uint t_lastError;
uint GetLastError() { return t_lastError; }
String toLinuxPath(const wchar_t* path)
{
	String str = toString(path);
	std::replace(str.begin(), str.end(), '\\', '/');
	return std::move(str);
}
int CreateDirectoryW(const wchar_t* path, void* lpSecurityAttributes)
{
	String str = toLinuxPath(path);
	if (str[str.size()-1] == '/')
		str.resize(str.size()-1);
	if (mkdir(str.c_str(), 0777) == 0)
		return 1;
	if (errno == EEXIST)
	{
		t_lastError = ERROR_ALREADY_EXISTS;
		return 0;
	}
	if (errno == ENOENT)
	{
		t_lastError = ERROR_PATH_NOT_FOUND;
		return 0;
	}
	EACOPY_NOT_IMPLEMENTED 	// TODO: Set error code
	return 0;
}
bool RemoveDirectoryW(const wchar_t* lpPathName)
{
	String file = toLinuxPath(lpPathName);
	if (remove(file.c_str()) == 0)
		return true;
	EACOPY_NOT_IMPLEMENTED
	return false;
}
struct FindFileDataLinux
{
	dirent* entry;
	wchar_t name[128];
	char path[1024];
};
}
#endif

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

CriticalSection::CriticalSection()
{
	#if defined(_WIN32)
	static_assert(sizeof(CRITICAL_SECTION) == sizeof(data), "Need to change size of data to match CRITICAL_SECTION");
	InitializeCriticalSectionAndSpinCount((CRITICAL_SECTION*)&data, 0x00000400);
	#else
	static_assert(sizeof(pthread_mutex_t) <= sizeof(data), "Need to change size of data to match pthread_mutex_t");
	new (data) pthread_mutex_t(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP);
	#endif
}

CriticalSection::~CriticalSection()
{
	#if defined(_WIN32)
	DeleteCriticalSection((CRITICAL_SECTION*)&data);
	#else
	((pthread_mutex_t*)&data)->~pthread_mutex_t();
	#endif
}

void
CriticalSection::enter()
{
	#if defined(_WIN32)
	EnterCriticalSection((CRITICAL_SECTION*)&data);
	#else
	pthread_mutex_lock((pthread_mutex_t*)&data);
	#endif
}

void
CriticalSection::leave()
{
	#if defined(_WIN32)
	LeaveCriticalSection((CRITICAL_SECTION*)&data);
	#else
	pthread_mutex_unlock((pthread_mutex_t*)&data);
	#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Event::Event()
{
	#if defined(_WIN32)
	ev = CreateEvent(nullptr, true, false, nullptr);
	#else
	ev = nullptr;
	#endif
}

Event::~Event()
{
	#if defined(_WIN32)
	CloseHandle(ev);
	#else
	#endif
}

void
Event::set()
{
	#if defined(_WIN32)
	SetEvent(ev);
	#else
	ScopedCriticalSection c(cs);
	ev = (void*)(uintptr_t)1;
	#endif
}

void
Event::reset()
{
	#if defined(_WIN32)
	ResetEvent(ev);
	#else
	ScopedCriticalSection c(cs);
	ev = nullptr;
	#endif
}

bool
Event::isSet(uint timeOutMs)
{
	#if defined(_WIN32)
	return WaitForSingleObject(ev, timeOutMs) == WAIT_OBJECT_0;
	#else
	if (timeOutMs == 0)
	{
		uintptr_t v;
		cs.scoped([&]() { v = (uintptr_t)ev; });
		return v == 1;
	}

	u64 startMs = getTimeMs();
	u64 lastMs = startMs;
	while (true)
	{
		uintptr_t v;
		cs.scoped([&]() { v = (uintptr_t)ev; });
		if (v)
			return true;
		if (lastMs - startMs >= timeOutMs)
			break;
		Sleep(1);
		lastMs = getTimeMs();
	}
	return false;
	#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Thread::Thread()
:	handle(nullptr)
,	exitCode(0)
,	joined(false)
{
}

Thread::Thread(Function<int()>&& f)
:	exitCode(0)
,	joined(false)
{
	start(std::move(f));
}

Thread::~Thread()
{
	if (!handle)
		return;
	wait();
	#if defined(_WIN32)
	CloseHandle(handle);
	#else
	#endif
}

void
Thread::start(Function<int()>&& f)
{
	func = std::move(f);
	#if defined(_WIN32)
	handle = CreateThread(NULL, 0, [](LPVOID p) -> uint { return ((Thread*)p)->func(); }, this, 0, NULL);
	#else
	static_assert(sizeof(pthread_t) <= sizeof(handle), "");
	auto& tid = *(pthread_t*)&handle;
	int err = pthread_create(&tid, NULL, [](void* p) -> void* { int res = ((Thread*)p)->func(); return (void*)(uintptr_t)res; }, this);
    if (err != 0)
        logErrorf(L"can't create thread :[%hs]", strerror(err));
	#endif
}

void
Thread::wait()
{
	if (!handle)
		return;
	if (joined)
		return;
	joined = true;
	#if defined(_WIN32)
	WaitForSingleObject(handle, INFINITE);
	#else
	int* ptr = 0;
	pthread_join(*(pthread_t*)&handle, (void**)&ptr);
	exitCode = (uint)(uintptr_t)ptr;
	#endif
}

bool
Thread::getExitCode(uint& outExitCode)
{
	if (!handle)
		return false;
	#if defined(_WIN32)
	if (GetExitCodeThread(handle, (uint*)&outExitCode))
		return true;
	#else
	if (joined)
	{
		outExitCode = exitCode;
		return true;
	}
	#endif

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

	if (m_logFile != InvalidFileHandle)
	{
		auto temp = toString(entry.str.c_str());
		IOStats ioStats;
		writeFile(m_logFileName.c_str(), m_logFile, temp.c_str(), temp.size(), ioStats);
		if (entry.linefeed)
			writeFile(m_logFileName.c_str(), m_logFile, "\r\n", 2, ioStats);
	}
	else
	{
		ScopedCriticalSection cs(g_logCs);
		fputws(entry.str.c_str(), stdout);
		if (entry.linefeed)
			fputws(L"\n", stdout);
	}

	if (entry.isError && m_cacheRecentErrors)
	{
		ScopedCriticalSection cs(g_logCs);
		if (m_recentErrors.size() > 10)
			m_recentErrors.pop_back();
		m_recentErrors.push_front(entry.str.c_str());
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

	if (m_logFile != InvalidFileHandle)
	{
		#if defined(_WIN32)
		FlushFileBuffers(m_logFile);
		#else
		fsync((int)(uintptr_t)m_logFile);
		#endif
	}
	else
		fflush(stdout);

	return (uint)temp.size();
}

uint Log::logQueueThread()
{
	bool isDebuggerPresent = EACOPY_IS_DEBUGGER_PRESENT;

	IOStats ioStats;
	if (!m_logFileName.empty())
		openFileWrite(m_logFileName.c_str(), m_logFile, ioStats, true);

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
	ScopedCriticalSection cs(m_logQueueCs);
	m_logQueue = new List<LogEntry>();
	m_logThreadActive = true;
	m_logThread = new Thread([this]() { return logQueueThread(); });
}

void Log::deinit(const Function<void()>& lastChanceLogging)
{
	bool isDebuggerPresent = EACOPY_IS_DEBUGGER_PRESENT;

	m_logQueueCs.scoped([&]() { m_logThreadActive = false; });
	
	delete m_logThread;

	ScopedCriticalSection cs(m_logQueueCs);
	processLogQueue(isDebuggerPresent);
	if (lastChanceLogging)
	{
		lastChanceLogging();
		processLogQueue(isDebuggerPresent);
	}
	delete m_logQueue;
	m_logQueue = nullptr;
	IOStats ioStats;
	if (m_logFile != InvalidFileHandle)
		closeFile(m_logFileName.c_str(), m_logFile, AccessType_Write, ioStats);
}

void Log::traverseRecentErrors(const Function<bool(const WString&)>& errorFunc)
{
	ScopedCriticalSection cs(g_logCs);
	for (auto& err : m_recentErrors)
		errorFunc(err);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void logInternal(const wchar_t* buffer, bool flush, bool linefeed, bool isError)
{
	if (LogContext* context = t_logContext)
	{
		if (context->m_muted)
			return;
		Log& log = context->log;
		ScopedCriticalSection cs(log.m_logQueueCs);
		if (log.m_logQueue)
		{
			log.m_logQueue->push_back({buffer, linefeed, isError});
			log.m_logQueueFlush |= flush;
		}
	}
	else
	{
		ScopedCriticalSection cs(g_logCs);
		fputws(buffer, stdout);
		if (linefeed)
			fputws(L"\n", stdout);

		if (flush)
			fflush(stdout);
		#if defined(EACOPY_USE_OUTPUTDEBUGSTRING)
		if (EACOPY_IS_DEBUGGER_PRESENT)
			OutputDebugStringW(buffer);
		#endif
	}
}

void logErrorf(const wchar_t* fmt, ...)
{
    va_list arg; 
    va_start(arg, fmt);
	wchar_t buffer[4096];
	stringCopy(buffer, eacopy_sizeof_array(buffer), L"!!ERROR - ");
	auto len = wcslen(buffer);
	int written = vswprintf_s(buffer + len, eacopy_sizeof_array(buffer) - len, fmt, arg);
	logInternal(buffer, true, true, true);
	va_end(arg);
	if (LogContext* c = t_logContext)
		c->m_lastError = -1;
}

void logInfo(const wchar_t* str)
{
	logInternal(str, false, false, false);
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
	uint size = swprintf(buf, L"   %ls:%ls%ls", name, getPadding(name), toPretty(bytes, 7).c_str());
	addCount(buf, size, 0);
	stats.push_back(buf);
}
void populateStatsValue(Vector<WString>& stats, const wchar_t* name, float value)
{
	if (!value)
		return;
	wchar_t buf[512];
	uint size = swprintf(buf, L"   %ls:%ls%8.1f", name, getPadding(name), value);
	addCount(buf, size, 0);
	stats.push_back(buf);
}

void populateStatsValue(Vector<WString>& stats, const wchar_t* name, uint value)
{
	if (!value)
		return;
	wchar_t buf[512];
	uint size = swprintf(buf, L"   %ls:%ls%8u", name, getPadding(name), value);
	addCount(buf, size, 0);
	stats.push_back(buf);
}

void populateIOStats(Vector<WString>& stats, const IOStats& ioStats)
{
	populateStatsTime(stats, L"FindFile", ioStats.findFileTime, ioStats.findFileCount);
	populateStatsTime(stats, L"ReadFile", ioStats.readTime, ioStats.createReadCount);
	populateStatsTime(stats, L"WriteFile", ioStats.writeTime, ioStats.createWriteCount);
	populateStatsTime(stats, L"LinkFile", ioStats.createLinkTime, ioStats.createLinkCount);
	populateStatsTime(stats, L"DeleteFile", ioStats.deleteFileTime, ioStats.deleteFileCount);
	populateStatsTime(stats, L"CopyFile", ioStats.copyFileTime, ioStats.copyFileCount);
	populateStatsTime(stats, L"CreateDir", ioStats.createDirTime, ioStats.createDirCount);
	populateStatsTime(stats, L"RemoveDir", ioStats.removeDirTime, ioStats.removeDirCount);
	populateStatsTime(stats, L"FileInfo", ioStats.fileInfoTime, ioStats.fileInfoCount);
	populateStatsTime(stats, L"SetWriteTime", ioStats.setLastWriteTime, ioStats.setLastWriteTimeCount);
}

void logInfoStats(const Vector<WString>& stats)
{
	for (uint i = 0; i < stats.size(); i += 2)
		if (i + 1 < stats.size())
			logInfoLinef(L"%ls%ls", stats[i].c_str(), stats[i + 1].c_str());
		else
			logInfoLinef(L"%ls", stats[i].c_str());
}

void logDebugStats(const Vector<WString>& stats)
{
	for (uint i = 0; i < stats.size(); i += 2)
		if (i + 1 < stats.size())
			logDebugLinef(L"%ls%ls", stats[i].c_str(), stats[i + 1].c_str());
		else
			logDebugLinef(L"%ls", stats[i].c_str());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(EACOPY_USE_SYMLINK_FOR_LONGPATHS)

CriticalSection g_temporarySymlinkCacheCs;
Map<WString, WString> g_temporarySymlinkCache;
ScopeGuard g_temporarySymlinkGuard([]()  // Will run at static dtor and remove all symlinks
	{
		for (auto& i : g_temporarySymlinkCache)
			RemoveDirectoryW(i.second.c_str());
	});

bool createTemporarySymlink(const wchar_t* dest, WString& outNewDest)
{
	wchar_t tempPath[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, tempPath))
	{
		logErrorf(L"Failed to get temp path. Reason: %ls", getLastErrorText().c_str());
		return false;
	}

	uint retryCount = 3;
	while (true)
	{
		// Use time as unique id
		uint64_t v;
		if (!QueryPerformanceCounter((union _LARGE_INTEGER*)&v))
		{
			logErrorf(L"Failed to get performance counter value. Reason: %ls", getLastErrorText().c_str());
			return false;
		}

		auto str = std::to_wstring(v);
		wcscat_s(tempPath, eacopy_sizeof_array(tempPath), str.c_str());

		if (!CreateSymbolicLinkW(tempPath, dest, 0))
		{
			if (!retryCount--)
			{
				logErrorf(L"Failed to create symbolic link from %ls to %ls. Reason: %ls", tempPath, dest, getLastErrorText().c_str());
				return false;
			}
			Sleep(1);
			continue;
		}

		outNewDest = tempPath;
		return true;
	}

	return false;
}
#endif

const wchar_t* convertToShortPath(const wchar_t* path, WString& outTempBuffer)
{
#if defined(_WIN32)
	uint pathLen = wcslen(path);
	if (pathLen < MAX_PATH - 12)
		return path;

#if !defined(EACOPY_USE_SYMLINK_FOR_LONGPATHS)

	if (path[0] == L'\\' && path[1] == L'\\')
	{
		if (path[2] == '?') // Already formatted
			return path;
		outTempBuffer = L"\\\\?\\UNC\\";
		outTempBuffer += path + 2;
	}
	else
	{
		outTempBuffer = L"\\\\?\\";
		outTempBuffer += path;

		//outTempBuffer += L"localhost\\";
		//outTempBuffer += path[0];
		//outTempBuffer += L'$';
		//outTempBuffer += path + 2;
	}
	return outTempBuffer.c_str();
#else
	uint position = 220;// Use slightly less length than MAX_PATH to increase chances of reusing symlink for multiple files
	while (path[position] != '\\')
		--position;

	enum { TempBufferCapacity = 1024 };
	wchar_t tempBuffer[TempBufferCapacity];

	wcsncpy_s(tempBuffer, TempBufferCapacity, path, position);

	ScopedCriticalSection cs(g_temporarySymlinkCacheCs);

	auto ins = g_temporarySymlinkCache.insert({tempBuffer, WString()});
	if (ins.second)
	{
		if (!createTemporarySymlink(tempBuffer, ins.first->second))
			return path;
	}

	stringCopy(tempBuffer, TempBufferCapacity, ins.first->second.c_str());
	wcscat_s(tempBuffer, TempBufferCapacity, path + position);

	outTempBuffer = tempBuffer;

	return outTempBuffer.c_str();
#endif
#else
	return path;
#endif
}

bool isDotOrDotDot(const wchar_t* str)
{
	return stringEquals(str, L".") || stringEquals(str, L"..");
}

FindFileHandle
findFirstFile(const wchar_t* searchStr, FindFileData& findFileData, IOStats& ioStats)
{
	++ioStats.findFileCount;
	TimerScope _(ioStats.findFileTime);
#if defined(_WIN32)
	WString tempBuffer;
	searchStr = convertToShortPath(searchStr, tempBuffer);
	static_assert(sizeof(WIN32_FIND_DATAW) <= sizeof(FindFileData), "");
	return FindFirstFileExW(searchStr, FindExInfoBasic, (WIN32_FIND_DATAW*)&findFileData, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
#else
	static_assert(sizeof(FindFileDataLinux) <= sizeof(FindFileData), "");
	String str = toLinuxPath(searchStr);
	
	// TODO: BAAAAAAD
	size_t starPos = str.find_first_of("*");
	if (starPos != String::npos)
		str.resize(starPos);
	
	DIR* dir = opendir(str.c_str());
	if (!dir)
	{
		if (errno == ENOENT)
		{
			t_lastError = ERROR_PATH_NOT_FOUND;
			return InvalidFindFileHandle;
		}
		EACOPY_NOT_IMPLEMENTED
		return InvalidFindFileHandle;
	}
	auto& fd = *(FindFileDataLinux*)&findFileData;

	strcpy(fd.path, str.c_str());
	fd.entry = readdir(dir);
	if (fd.entry)
		return dir;
	EACOPY_NOT_IMPLEMENTED //  Need to set error codes
	closedir(dir);
	return InvalidFindFileHandle;
#endif
}

bool
findNextFile(FindFileHandle handle, FindFileData& findFileData, IOStats& ioStats)
{
	TimerScope _(ioStats.findFileTime);
#if defined(_WIN32)
	return FindNextFileW(handle, (WIN32_FIND_DATAW*)&findFileData) != 0;
#else
	auto dir = (DIR*)handle;
	auto& fd = *(FindFileDataLinux*)&findFileData;
	errno = 0;
	fd.entry = readdir(dir);
	if (fd.entry)
		return true;
	if (errno == 0)
	{
		t_lastError = ERROR_NO_MORE_FILES;
		return false;
	}

	EACOPY_NOT_IMPLEMENTED //  Need to set error codes
	t_lastError = ERROR_INVALID_HANDLE;
	return false;
#endif
}

void
findClose(FindFileHandle handle, IOStats& ioStats)
{
	TimerScope _(ioStats.findFileTime);
#if defined(_WIN32)
	FindClose(handle);
#else
	auto dir = (DIR*)handle;
	closedir(dir);
#endif
}

uint
getFileInfo(FileInfo& outInfo, FindFileData& findFileData)
{
#if defined(_WIN32)
	auto& fd = *(WIN32_FIND_DATAW*)&findFileData;
	outInfo.creationTime = { 0, 0 };
	outInfo.lastWriteTime = *(FileTime*)&fd.ftLastWriteTime;
	outInfo.fileSize = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
	return fd.dwFileAttributes;
#else
	auto& fd = *(FindFileDataLinux*)&findFileData;
	struct stat st;
	char fullPath[1024];
	strcpy(fullPath, fd.path);
	strcat(fullPath, fd.entry->d_name);
	if (stat(fullPath, &st) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return 0;
	}

	outInfo.creationTime = { 0, 0 };
	outInfo.lastWriteTime = { uint(st.st_mtim.tv_sec >> 32), uint(st.st_mtim.tv_sec) };
	outInfo.fileSize = st.st_size;

	uint attr = 0;
	if ((st.st_mode & S_IWUSR) == 0)
		attr |= FILE_ATTRIBUTE_READONLY;


	if (fd.entry->d_type == DT_REG)
		attr |= FILE_ATTRIBUTE_NORMAL;
	else if (fd.entry->d_type == DT_DIR)
		attr |= FILE_ATTRIBUTE_DIRECTORY;
	else
		EACOPY_NOT_IMPLEMENTED;
	return attr;
#endif
}

bool
getFileHash(Hash& outHash, const wchar_t* fullFileName, CopyContext& copyContext, IOStats& ioStats, HashContext& hashContext, u64& hashTime)
{
	bool useBufferedIO = true;
	FileHandle handle;
	if (!openFileRead(fullFileName, handle, ioStats, useBufferedIO))
		return false;
	ScopeGuard fileGuard([&]() { closeFile(fullFileName, handle, AccessType_Read, ioStats); });

	HashBuilder builder(hashContext);

	while (true)
	{
		uint toRead = CopyContextBufferSize;
		uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

		u64 read;
		if (!readFile(fullFileName, handle, copyContext.buffers[0], toReadAligned, read, ioStats))
			return false;
		if (read == 0)
			break;
		if (!builder.add(copyContext.buffers[0], read))
			return false;
	}

	return builder.getHash(outHash);
}

wchar_t*
getFileName(FindFileData& findFileData)
{
#if defined(_WIN32)
	auto& fd = *(WIN32_FIND_DATAW*)&findFileData;
	return fd.cFileName;
#else
	auto& fd = *(FindFileDataLinux*)&findFileData;
	WString temp(fd.entry->d_name, fd.entry->d_name + strlen(fd.entry->d_name));
	stringCopy(fd.name, 512, temp.c_str());
	return fd.name;
#endif
}

WString getCleanedupPath(wchar_t* path, uint startIndex, bool lastWasSlash)
{
	// Change all slash to backslash
	// Remove double backslashes after <startIndex> character..
	// Add backslash at the end if missing

	uint pathLen = wcslen(path);
	if (pathLen < startIndex)
		startIndex = pathLen;
	wchar_t tempBuffer[4096];
	stringCopy(tempBuffer, eacopy_sizeof_array(tempBuffer), path);
	wchar_t* readIt = tempBuffer + startIndex;
	wchar_t* writeIt = tempBuffer + startIndex;
	while (*readIt)
	{
		wchar_t c = *readIt;
		bool isSlash = c == '/' || c == '\\';
		++readIt;

		if (isSlash && lastWasSlash)
			continue;
		lastWasSlash = isSlash;

		*writeIt = isSlash ? L'\\' : c;
		++writeIt;
	}
	if (!lastWasSlash)
	{
		*writeIt = L'\\';
		++writeIt;
	}
	*writeIt = 0;

	return tempBuffer;
}

bool isLocalPath(const wchar_t* path)
{
	return path[1] != L'\\';
}

void removeTemporarySymlinks(const wchar_t* path)
{
#if defined(EACOPY_USE_SYMLINK_FOR_LONGPATHS)
	uint pathLen = wcslen(path);
	if (pathLen < MAX_PATH - 12)
		return;

	g_temporarySymlinkCacheCs.enter();
	ScopeGuard csGuard([&]() { g_temporarySymlinkCacheCs.leave();; });

	auto it = g_temporarySymlinkCache.upper_bound(path);
	while (it != g_temporarySymlinkCache.end())
	{
		if (StrCmpNIW(it->first.c_str(), path, pathLen) > 0)
			break;
		RemoveDirectoryW(it->second.c_str());
		it = g_temporarySymlinkCache.erase(it);
	}

	g_temporarySymlinkCache.clear();
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

uint getFileInfo(FileInfo& outInfo, const wchar_t* fullFileName, IOStats& ioStats)
{
	++ioStats.fileInfoCount;
	TimerScope _(ioStats.fileInfoTime);

	#if defined(_WIN32)
	WString temp;
	fullFileName = convertToShortPath(fullFileName, temp);

	WIN32_FILE_ATTRIBUTE_DATA fd;
	BOOL ret = GetFileAttributesExW(fullFileName, GetFileExInfoStandard, &fd);
	if (ret == 0)
	{
		outInfo = FileInfo();
		return 0;
	}

	outInfo.creationTime = { 0, 0 }; //fd.ftCreationTime;
	outInfo.lastWriteTime = *(FileTime*)&fd.ftLastWriteTime;
	outInfo.fileSize = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
	return fd.dwFileAttributes;
	#else
	String str = toLinuxPath(fullFileName);
	if (str[str.size()-1] == '/')
		str.resize(str.size()-1);

	struct stat st;
	if (stat(str.c_str(), &st) == -1)
	{
		if (errno == ENOENT)
		{
			t_lastError = ERROR_FILE_NOT_FOUND;
			return 0;
		}
		EACOPY_NOT_IMPLEMENTED
		return 0;
	}

	outInfo.creationTime = { 0, 0 };
	outInfo.lastWriteTime = { uint(st.st_mtim.tv_sec >> 32), uint(st.st_mtim.tv_sec) };
	outInfo.fileSize = st.st_size;

	uint attr = 0;
	if ((st.st_mode & S_IWUSR) == 0)
		attr |= FILE_ATTRIBUTE_READONLY;

	if ((st.st_mode & S_IFREG) != 0)
		attr |= FILE_ATTRIBUTE_NORMAL;
	else if ((st.st_mode & S_IFDIR) != 0)
		attr |= FILE_ATTRIBUTE_DIRECTORY;
	else
		EACOPY_NOT_IMPLEMENTED
	return attr;
	#endif
}

bool equals(const FileInfo& a, const FileInfo& b)
{
	return memcmp(&a, &b, sizeof(FileInfo)) == 0;
}

bool replaceIfSymLink(const wchar_t* directory, uint attributes, IOStats& ioStats)
{
	bool isDir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if (!isDir)
	{
		logErrorf(L"Trying to treat file as directory %ls: %ls", directory, getLastErrorText().c_str());
		return false;
	}

	bool isSymlink = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	if (!isSymlink)
		return true;

	{
		// Delete reparsepoint and treat path as not existing
		++ioStats.removeDirCount;
		TimerScope _(ioStats.removeDirTime);
		if (!RemoveDirectoryW(directory))
		{
			logErrorf(L"Trying to remove reparse point while ensuring directory %ls: %ls", directory, getLastErrorText().c_str());
			return false;
		}
	}

	++ioStats.createDirCount;
	TimerScope _(ioStats.createDirTime);
	if (CreateDirectoryW(directory, NULL) != 0)
		return true;

	logErrorf(L"Error creating directory %ls: %ls", directory, getLastErrorText().c_str());
	return false;
}

bool ensureDirectory(const wchar_t* directory, IOStats& ioStats, bool replaceIfSymlink, bool expectCreationAndParentExists, FilesSet* outCreatedDirs)
{
	// This is an optimization to reduce kernel calls
	if (expectCreationAndParentExists)
	{
		{
			++ioStats.createDirCount;
			TimerScope _(ioStats.createDirTime);
			if (CreateDirectoryW(directory, NULL) != 0)
			{
				if (outCreatedDirs)
					outCreatedDirs->insert(directory);
				return true;
			}
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS) 
		{
			FileInfo dirInfo;
			uint destDirAttributes = getFileInfo(dirInfo, directory, ioStats);
			if (!destDirAttributes)
			{
				logErrorf(L"Trying to get info for %ls: %ls", directory, getLastErrorText().c_str());
				return false;
			}

			if (!(destDirAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				logErrorf(L"Trying to treat file as directory %ls", directory);
				return false;
			}

			if (replaceIfSymlink)
				if (!replaceIfSymLink(directory, destDirAttributes, ioStats))
					return false;
			return true;
		}
	}

	WString temp;
	size_t pathLen = wcslen(directory);
	if (pathLen > 3) // If path is like "d:\" we don't want to remove the last slash
	{
		while (directory[pathLen-1] == '\\')
		{
			temp.assign(directory, directory + pathLen - 1);
			directory = temp.c_str();
		}
	}

	const wchar_t* lastBackslash = wcsrchr(directory, '\\');
	if (!lastBackslash)
	{
		if (directory[1] == L':') // We got all the way to the root
			return true;
		logErrorf(L"Error validating directory %ls: Bad format.. must contain a slash", directory);
		return false;
	}

	FileInfo dirInfo;
	uint destDirAttributes = getFileInfo(dirInfo, directory, ioStats);
	if (destDirAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		if (replaceIfSymlink)
			if (!replaceIfSymLink(directory, destDirAttributes, ioStats))
				return false;
		return true;
	}
	else
	{
		if (destDirAttributes != 0)
		{
			logErrorf(L"Trying to treat file as directory %ls", directory);
			return false;
		}
		uint error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
		{
			logErrorf(L"Error getting attributes from directory %ls: %ls", directory, getErrorText(error).c_str());
			return false;
		}
	}

	if (lastBackslash)
	{
		WString shorterDirectory(directory, 0, lastBackslash - directory);
		if (!ensureDirectory(shorterDirectory.c_str(), ioStats, false, false, outCreatedDirs))
			return false;
	}

	{
		++ioStats.createDirCount;
		TimerScope _(ioStats.createDirTime);
		WString tempBuffer;
		const wchar_t* validDirectory = convertToShortPath(directory, tempBuffer);
		if (CreateDirectoryW(validDirectory, NULL) != 0)
		{
			if (outCreatedDirs)
				outCreatedDirs->insert(directory);
			return true;
		}
	}

	uint error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS) 
		return true;

	logErrorf(L"Error creating directory %ls: %ls", directory, getErrorText(error).c_str());
	return false;
}

bool isError(uint error, bool errorOnMissingFile)
{
	return errorOnMissingFile || (ERROR_FILE_NOT_FOUND != error && ERROR_PATH_NOT_FOUND != error);
}

bool deleteAllFiles(const wchar_t* directory, bool& outPathFound, IOStats& ioStats, bool errorOnMissingFile)
{
	outPathFound = true;
	FindFileData fd;
	WString dir(directory);
	if (dir[dir.length()-1] != L'\\')
		dir += L'\\';
    WString searchStr = dir + L"*.*";
    FindFileHandle findHandle = findFirstFile(searchStr.c_str(), fd, ioStats); 
    if(findHandle == InvalidFileHandle)
	{
		uint error = GetLastError();
		if (ERROR_PATH_NOT_FOUND == error)
		{
			outPathFound = false;
			return true;
		}

		logErrorf(L"deleteDirectory failed using FindFirstFile for directory %ls: %ls", directory, getErrorText(error).c_str());
		return false;
	}

	ScopeGuard closeFindGuard([&]() { findClose(findHandle, ioStats); });

    do
	{ 
		FileInfo fileInfo;
		uint fileAttr = getFileInfo(fileInfo, fd);
		const wchar_t* fileName = getFileName(fd);
        if(!(fileAttr & FILE_ATTRIBUTE_DIRECTORY))
		{
			WString fullName(dir + fileName);
			if (fileAttr & FILE_ATTRIBUTE_READONLY)
				if (!setFileWritable(fullName.c_str(), true))
					if (isError(GetLastError(), errorOnMissingFile))
					{
						logErrorf(L"Failed to set file attributes to writable for file %ls", fullName.c_str());
						return false;
					}

			if (!deleteFile(fullName.c_str(), ioStats, errorOnMissingFile))
				return false;
		}
		else if (!isDotOrDotDot(fileName))
		{
			WString fullName(dir + fileName);
			bool isSymlink = (fileAttr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
			if (isSymlink)
			{
				WString buffer;
				const wchar_t* fullName2 = convertToShortPath(fullName.c_str(), buffer);
				// Delete reparsepoint and treat path as not existing
				++ioStats.removeDirCount;
				TimerScope _(ioStats.removeDirTime);
				if (!RemoveDirectoryW(fullName2))
				{
					uint error = GetLastError();
					if (isError(error, errorOnMissingFile))
					{
						logErrorf(L"Trying to remove reparse point while ensuring directory %ls: %ls", directory, getErrorText(fullName2, error).c_str());
						return false;
					}
				}
			}
			else if (!deleteDirectory(fullName.c_str(), ioStats, errorOnMissingFile))
				return false;
		}

	}
	while(findNextFile(findHandle, fd, ioStats)); 

	uint error = GetLastError();

	closeFindGuard.execute(); // Need to close find handle otherwise RemoveDirectory will fail now and then

	if (error == ERROR_NO_MORE_FILES)
		return true;

	logErrorf(L"FindNextFile failed for path %ls", directory);
	return false;
}

bool deleteAllFiles(const wchar_t* directory, IOStats& ioStats, bool errorOnMissingFile)
{
	bool pathFound;
	return deleteAllFiles(directory, pathFound, ioStats, errorOnMissingFile);
}

bool deleteDirectory(const wchar_t* directory, IOStats& ioStats, bool errorOnMissingFile)
{
	bool outPathFound;
	if (!deleteAllFiles(directory, outPathFound, ioStats, errorOnMissingFile))
		return false;

	if (!outPathFound)
		return true;

	// Clear out temporary symlinks
	removeTemporarySymlinks(directory);

	WString tempBuffer;
	const wchar_t* validDirectory = convertToShortPath(directory, tempBuffer);

	++ioStats.removeDirCount;
	TimerScope _(ioStats.removeDirTime);
	if (RemoveDirectoryW(validDirectory))
		return true;

	uint error = GetLastError();
	if (!isError(error, errorOnMissingFile))
		return true;

	logErrorf(L"Trying to remove directory  %ls: %ls", validDirectory, getErrorText(validDirectory, error).c_str());
	return false;
}

bool isAbsolutePath(const wchar_t* path)
{
	uint pathLen = wcslen(path);
	if (pathLen < 3)
		return false;
	#if defined(_WIN32)
	return path[1] == ':' || (path[0] == '\\' && path[1] == '\\');
	#else
	return path[0] == '\\';
	#endif
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

bool openFileRead(const wchar_t* fullPath, FileHandle& outFile, IOStats& ioStats, bool useBufferedIO, _OVERLAPPED* overlapped, bool isSequentialScan, bool sharedRead)
{
	TimerScope _(ioStats.createReadTime);
	++ioStats.createReadCount;
	#if defined(_WIN32)
	uint nobufferingFlag = useBufferedIO ? 0 : FILE_FLAG_NO_BUFFERING;
	uint sequentialScanFlag = isSequentialScan ? FILE_FLAG_SEQUENTIAL_SCAN : 0;
	DWORD shareMode = sharedRead ? FILE_SHARE_READ : 0;
	WString temp;
	fullPath = convertToShortPath(fullPath, temp);
	outFile = CreateFileW(fullPath, GENERIC_READ, shareMode, NULL, OPEN_EXISTING, sequentialScanFlag | nobufferingFlag, overlapped);
	if (outFile != InvalidFileHandle)
		return true;

	logErrorf(L"Failed to open file %ls: %ls", fullPath, getErrorText(fullPath, GetLastError()).c_str());
	return false;
	#else
	String path = toLinuxPath(fullPath);
    int fileHandle = open(path.c_str(), O_RDONLY, 0);
	if (fileHandle == -1)
	{
		outFile = InvalidFileHandle;
		EACOPY_NOT_IMPLEMENTED
		return false;
	}
	outFile = (FileHandle)(uintptr_t)fileHandle;
	//if (!sharedRead)
	//	flock(fileHandle, LOCK_EX); // This does not work.. don't know how to do this on linux
	return true;
	#endif
}

bool openFileWrite(const wchar_t* fullPath, FileHandle& outFile, IOStats& ioStats, bool useBufferedIO, _OVERLAPPED* overlapped, bool hidden)
{
	#if defined(_WIN32)
	uint nobufferingFlag = useBufferedIO ? 0 : FILE_FLAG_NO_BUFFERING;
	uint writeThroughFlag = CopyFileWriteThrough ? FILE_FLAG_WRITE_THROUGH : 0;

	uint flagsAndAttributes = FILE_ATTRIBUTE_NORMAL | nobufferingFlag | writeThroughFlag | FILE_FLAG_SEQUENTIAL_SCAN;
	if (overlapped)
		flagsAndAttributes |= FILE_FLAG_OVERLAPPED;
	if (hidden)
		flagsAndAttributes |= FILE_ATTRIBUTE_HIDDEN;

	++ioStats.createWriteCount;
	TimerScope _(ioStats.createWriteTime);
	WString temp;
	fullPath = convertToShortPath(fullPath, temp);
	outFile = CreateFileW(fullPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, flagsAndAttributes, NULL);
	if (outFile != InvalidFileHandle)
		return true;
	logErrorf(L"Trying to create file %ls: %ls", fullPath, getErrorText(fullPath, GetLastError()).c_str());
	return false;
	#else
	String path = toLinuxPath(fullPath);
    int fileHandle = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fileHandle == -1)
	{
		outFile = InvalidFileHandle;
		EACOPY_NOT_IMPLEMENTED
		return false;
	}
	outFile = (FileHandle)(uintptr_t)fileHandle;
	return true;
	#endif
}

bool writeFile(const wchar_t* fullPath, FileHandle& file, const void* data, u64 dataSize, IOStats& ioStats, _OVERLAPPED* overlapped)
{
	++ioStats.writeCount;
	TimerScope _(ioStats.writeTime);
	#if defined(_WIN32)
	if (overlapped)
	{
		if (dataSize >= u64(INT_MAX-1))
		{
			logErrorf(L"TODO: Data size too big, needs implementation");
			return false;
		}

		uint written;
		if (!WriteFile(file, data, (uint)dataSize, &written, overlapped))
			return false;
		return true;
	}

	u64 left = dataSize;
	while (left)
	{
		uint written;
		uint toWrite = (uint)std::min(left, u64(INT_MAX-1));
		if (!WriteFile(file, data, toWrite, &written, nullptr))
		{
			logErrorf(L"Trying to write data to %ls: %ls", fullPath, getErrorText(fullPath, GetLastError()).c_str());
			if (!overlapped)
				if (CloseHandle(file))
					file = InvalidFileHandle;
			return false;
		}
		(char*&)data += written;
		left -= written;
	}
	return true;
	#else
	int fileHandle = (int)(uintptr_t)file;
	u64 left = dataSize;
	while (left)
	{
		size_t  toWrite = (uint)std::min(left, u64(INT_MAX-1));
		size_t written = write(fileHandle, data, toWrite);
		if (written == -1)
		{
			EACOPY_NOT_IMPLEMENTED
			return false;
		}
		(char*&)data += written;
		left -= written;
	}
	return true;
	#endif
}

bool readFile(const wchar_t* fullPath, FileHandle& file, void* destData, u64 toRead, u64& read, IOStats& ioStats)
{
	TimerScope _(ioStats.readTime);
	++ioStats.readCount;
	#if defined(_WIN32)
	DWORD dwRead = 0;
	bool success = ReadFile(file, destData, toRead, &dwRead, NULL) != 0;
	read = dwRead;
	if (success)
		return true;

	if (GetLastError() == ERROR_IO_PENDING)
		return true;

	logErrorf(L"Fail reading file %ls: %ls", fullPath, getLastErrorText().c_str());
	return false;
	#else
	int fileHandle = (int)(uintptr_t)file;
	size_t size = ::read(fileHandle, destData, toRead);
	if (size == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}
	read = size;
	return true;
	#endif
}

bool setFileLastWriteTime(const wchar_t* fullPath, FileHandle& file, FileTime lastWriteTime, IOStats& ioStats)
{
	if (file == InvalidFileHandle)
		return false;

	// This should not be needed!
	//if (!FlushFileBuffers(file))
	//{
	//	logErrorf(L"Failed flushing buffer for file %ls: %ls", fullPath, getLastErrorText().c_str());
	//	return false;
	//}

	++ioStats.setLastWriteTimeCount;
	TimerScope _(ioStats.setLastWriteTime);
	#if defined(_WIN32)
	if (SetFileTime(file, NULL, NULL, (FILETIME*)&lastWriteTime))
		return true;
	logErrorf(L"Failed to set file time on %ls", fullPath);
	if (CloseHandle(file))
		file = InvalidFileHandle;
	return false;
	#else
	EACOPY_NOT_IMPLEMENTED
	return false;
	#endif
}

bool setFilePosition(const wchar_t* fullPath, FileHandle& file, u64 position, IOStats& ioStats)
{
	#if defined(_WIN32)
	LARGE_INTEGER li;
	li.QuadPart = position;

	DWORD dwPtrLow = SetFilePointer(file, li.LowPart, &li.HighPart, FILE_BEGIN);
	if (dwPtrLow != INVALID_SET_FILE_POINTER)
		return true;
	int lastError = GetLastError();
	if (lastError == NO_ERROR)
		return true;
	logErrorf(L"Fail setting file position on file %ls: %ls", fullPath, getErrorText(lastError).c_str());
	return false;
	#else
	EACOPY_NOT_IMPLEMENTED
	return false;
	#endif
}

bool closeFile(const wchar_t* fullPath, FileHandle& file, AccessType accessType, IOStats& ioStats)
{
	if (file == InvalidFileHandle)
		return true;

	++(accessType == AccessType_Read ? ioStats.closeReadCount : ioStats.closeWriteCount);
	TimerScope _(accessType == AccessType_Read ? ioStats.closeReadTime : ioStats.closeWriteTime);
	#if defined(_WIN32)
	bool success = CloseHandle(file) != 0;
	file = InvalidFileHandle;
	if (!success)
		logErrorf(L"Failed to close file %ls", fullPath);
	return success;
	#else
	int fileHandle = (int)(uintptr_t)file;
	file = InvalidFileHandle;
    if (close(fileHandle) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}
	return true;
	#endif
}

bool createFile(const wchar_t* fullPath, const FileInfo& info, const void* data, IOStats& ioStats, bool useBufferedIO, bool hidden)
{
	FileHandle file;
	if (!openFileWrite(fullPath, file, ioStats, useBufferedIO, nullptr, hidden))
		return false;
	if (!writeFile(fullPath, file, data, info.fileSize, ioStats))
		return false;
	if (info.lastWriteTime.dwLowDateTime || info.lastWriteTime.dwHighDateTime)
		if (!setFileLastWriteTime(fullPath, file, info.lastWriteTime, ioStats))
			return false;
	return closeFile(fullPath, file, AccessType_Write, ioStats);
}

bool createFileLink(const wchar_t* fullPath, const FileInfo& info, const wchar_t* sourcePath, bool& outSkip, IOStats& ioStats)
{
	outSkip = false;
	#if defined(_WIN32)
	WString tempBuffer1;
	fullPath = convertToShortPath(fullPath, tempBuffer1);

	WString tempBuffer2;
	sourcePath = convertToShortPath(sourcePath, tempBuffer2);
	do
	{
		{
			++ioStats.createLinkCount;
			TimerScope _(ioStats.createLinkTime);
			if (CreateHardLinkW(fullPath, sourcePath, NULL))
				return true;
		}

		uint error = GetLastError();
		if (error != ERROR_ALREADY_EXISTS)
		{
			logDebugLinef(L"Failed creating hardlink from %ls to %ls: %ls", fullPath, sourcePath, getErrorText(error).c_str());
			return false;
		}

		FileInfo other;
		uint attributes = getFileInfo(other, fullPath, ioStats);
		if (equals(info, other))
		{
			outSkip = true;
			return true;
		}

		// Delete file and try again
		if (!deleteFile(fullPath, ioStats))
			return false;

	} while (true); // Should only really get here once

	return false;
	#else
	EACOPY_NOT_IMPLEMENTED
	return false;
	#endif
}

#if defined(_WIN32)
uint internalCopyProgressRoutine(LARGE_INTEGER TotalFileSize, LARGE_INTEGER TotalBytesTransferred, LARGE_INTEGER StreamSize, LARGE_INTEGER StreamBytesTransferred, uint dwStreamNumber, uint dwCallbackReason, HANDLE hSourceFile, HANDLE hDestinationFile, LPVOID lpData)
{
	if (TotalBytesTransferred.QuadPart == TotalFileSize.QuadPart)
		*(u64*)lpData = TotalBytesTransferred.QuadPart;
	return PROGRESS_CONTINUE;
}
#endif

CopyContext::CopyContext()
{
	u8* data = new u8[CopyContextBufferSize * 3];
	buffers[0] = data + CopyContextBufferSize*0;
	buffers[1] = data + CopyContextBufferSize*1;
	buffers[2] = data + CopyContextBufferSize*2;
}

CopyContext::~CopyContext()
{
	delete[] buffers[0];
}

bool copyFile(const wchar_t* source, const wchar_t* dest, bool useSystemCopy, bool failIfExists, bool& outExisted, u64& outBytesCopied, IOStats& ioStats, UseBufferedIO useBufferedIO)
{
	CopyContext copyContext;
	FileInfo sourceInfo;
	uint sourceAttributes = getFileInfo(sourceInfo, source, ioStats);
	if (!sourceAttributes)
	{
		logErrorf(L"Failed to get file info for source file %ls: %ls", source, getLastErrorText().c_str());
		return false;
	}

	if ((sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		logErrorf(L"Failed to copy source file %ls: File is a directory", source);
		return false;
	}
	return copyFile(source, sourceInfo, dest, useSystemCopy, failIfExists, outExisted, outBytesCopied, copyContext, ioStats, useBufferedIO);
}

bool copyFile(const wchar_t* source, const FileInfo& sourceInfo, const wchar_t* dest, bool useSystemCopy, bool failIfExists, bool& outExisted, u64& outBytesCopied, CopyContext& copyContext, IOStats& ioStats, UseBufferedIO useBufferedIO)
{
	outExisted = false;
	outBytesCopied = 0;

	#if defined(_WIN32)

	// This kind of sucks but since machines might not have long paths enabled we have to work around it by making a symlink and copy through that
	WString tempBuffer1;
	source = convertToShortPath(source, tempBuffer1);

	WString tempBuffer2;
	dest = convertToShortPath(dest, tempBuffer2);

	if (UseOwnCopyFunction && !useSystemCopy)
	{
		enum { ReadChunkSize = 2 * 1024 * 1024 };
		static_assert(ReadChunkSize <= CopyContextBufferSize, "ReadChunkSize must be smaller than CopyContextBufferSize");

		uint overlappedFlag = UseOverlappedCopy ? FILE_FLAG_OVERLAPPED : 0;
		uint nobufferingFlag = getUseBufferedIO(useBufferedIO, sourceInfo.fileSize) ? 0 : FILE_FLAG_NO_BUFFERING;
		uint writeThroughFlag = CopyFileWriteThrough ? FILE_FLAG_WRITE_THROUGH : 0;

		OVERLAPPED osWrite = {0,0,0};
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;
		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);

		++ioStats.createWriteCount;
		u64 startCreateWriteTime = getTime();
		HANDLE destFile = CreateFileW(dest, FILE_WRITE_DATA|FILE_WRITE_ATTRIBUTES, 0, NULL, failIfExists ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | overlappedFlag | nobufferingFlag | writeThroughFlag, &osWrite);
		ioStats.createWriteTime += getTime() - startCreateWriteTime;

		if (destFile == InvalidFileHandle)
		{
			uint error = GetLastError();
			if (ERROR_FILE_EXISTS == error)
			{
				outExisted = true;
				return false;
			}

			logErrorf(L"Failed to create file %ls: %ls", dest, getErrorText(dest, error).c_str());
			return false;
		}

		bool result = true;
		
		ScopeGuard destGuard([&]() { CloseHandle(osWrite.hEvent); result &= closeFile(dest, destFile, AccessType_Write, ioStats); });

		OVERLAPPED osRead  = {0,0,0};
		osRead.Offset = 0;
		osRead.OffsetHigh = 0;
		osRead.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

		u64 startCreateReadTime = getTime();
		HANDLE sourceFile = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | overlappedFlag | nobufferingFlag, &osRead);
		ioStats.createReadTime += getTime() - startCreateReadTime;
		++ioStats.createReadCount;

		if (sourceFile == InvalidFileHandle)
		{
			logErrorf(L"Failed to open file %ls for read: %ls", source, getErrorText(source, GetLastError()).c_str());
			return false;
		}

		ScopeGuard sourceGuard([&]() { CloseHandle(osRead.hEvent); result &= closeFile(source, sourceFile, AccessType_Read, ioStats); });

		uint activeBufferIndex = 0;
		uint sizeFilled = 0;
		u8* bufferFilled = nullptr;

		u64 left = sourceInfo.fileSize;
		u64 read = 0;
		while (true)
		{
			if (sizeFilled)
			{
				++ioStats.writeCount;
				TimerScope _(ioStats.writeTime);

				if (UseOverlappedCopy && WaitForSingleObject(osWrite.hEvent, INFINITE) != WAIT_OBJECT_0)
				{
					logErrorf(L"WaitForSingleObject failed on write");
					return false;
				}

				uint written = 0;
				uint toWrite = nobufferingFlag ? (((sizeFilled + 4095) / 4096) * 4096) : sizeFilled;
				if (!WriteFile(destFile, bufferFilled, toWrite, &written, &osWrite))
				{
					if (GetLastError() != ERROR_IO_PENDING)
					{
						logErrorf(L"Fail writing file %ls: %ls", dest, getLastErrorText().c_str());
						return false;
					}
				}
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
				++ioStats.readCount;
				TimerScope _(ioStats.readTime);

				uint toRead = (uint)std::min(left, u64(ReadChunkSize));
				activeBufferIndex = (activeBufferIndex + 1) % 3;

				uint toReadAligned = nobufferingFlag ? (((toRead + 4095) / 4096) * 4096) : toRead;

				osRead.Offset = (LONG)read;
				osRead.OffsetHigh = (LONG)(read >> 32);

				if (!ReadFile(sourceFile, copyContext.buffers[activeBufferIndex], toReadAligned, NULL, &osRead))
				{
					if (GetLastError() != ERROR_IO_PENDING)
					{
						logErrorf(L"Fail reading file %ls: %ls", source, getLastErrorText().c_str());
						return false;
					}
				}

				read += toRead;
				left -= toRead;
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

		if (!setFileLastWriteTime(dest, destFile, sourceInfo.lastWriteTime, ioStats))
			return false;

		if (nobufferingFlag)
		{
			LONG lowSize = (LONG)sourceInfo.fileSize;
			LONG highSize = sourceInfo.fileSize >> 32;
			SetFilePointer(destFile, lowSize, &highSize, FILE_BEGIN);
			SetEndOfFile(destFile);
		}

		outBytesCopied = sourceInfo.fileSize;

		return result;
	}
	else
	{
		++ioStats.copyFileCount;
		TimerScope _(ioStats.copyFileTime);
		//BOOL cancel = false;
		//uint flags = COPY_FILE_NO_BUFFERING;
		//if (failIfExists)
		//	flags |= COPY_FILE_FAIL_IF_EXISTS;
		//if (CopyFileExW(source, dest, NULL, NULL, NULL, flags) != 0)
		//if (CopyFileExW(source, dest, (LPPROGRESS_ROUTINE)internalCopyProgressRoutine, &outBytesCopied, &cancel, flags) != 0)
		if (CopyFileW(source, dest, failIfExists) != 0)
		{
			outBytesCopied = sourceInfo.fileSize;
			return true;
		}

		uint error = GetLastError();
		if (ERROR_FILE_EXISTS == error)
		{
			outExisted = true;
			return false;
		}

		logErrorf(L"Failed to copy file %ls to %ls. Reason: %ls", source, dest, getErrorText(error).c_str());
		return false;
	}

	#else

	int destFlags = O_WRONLY | O_CREAT;
	if (failIfExists)
		destFlags |= O_EXCL;
	String to = toLinuxPath(dest);
    int destHandle = open(to.c_str(), destFlags, 0644);
	if (destHandle == -1)
	{
		if (errno == EEXIST)
		{
			outExisted = true;
			return false;
		}
		if (errno == EACCES)
		{
			outExisted = true;
			t_lastError = FILE_ATTRIBUTE_READONLY;
			return false;
		}
		EACOPY_NOT_IMPLEMENTED
		return false;
	}

	String from = toLinuxPath(source);
	int sourceHandle = open(from.c_str(), O_RDONLY, 0);
	if (sourceHandle == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}

	u64 written = 0;
	u8* buf = copyContext.buffers[0];
    while (true)
	{
		size_t size = read(sourceHandle, buf, CopyContextBufferSize);
		if (size == 0)
			break;
		if (size == -1)
		{
			EACOPY_NOT_IMPLEMENTED
			break;
		}
        if (write(destHandle, buf, size) == -1)
		{
			EACOPY_NOT_IMPLEMENTED
			break;
		}
		written += size;
	}

	if (ftruncate(destHandle, written) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}


	if (close(sourceHandle) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}

	if (close(destHandle) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}


	struct stat sourceStat;
	if (stat(from.c_str(), &sourceStat) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}

	utimbuf newTimes;
	newTimes.actime = time(NULL);
	newTimes.modtime = sourceStat.st_mtime;

	if (utime(to.c_str(), &newTimes) == -1)
	{
		EACOPY_NOT_IMPLEMENTED
		return false;
	}

	outBytesCopied += written;

	return true;
	#endif
}

bool deleteFile(const wchar_t* fullPath, IOStats& ioStats, bool errorOnMissingFile)
{
	++ioStats.deleteFileCount;
	TimerScope _(ioStats.deleteFileTime);

	#if defined(_WIN32)
	WString tempBuffer;
	fullPath = convertToShortPath(fullPath, tempBuffer);
	if (DeleteFileW(fullPath) != 0)
		return true;

	uint error = GetLastError();
	if (!isError(error, errorOnMissingFile))
		return true;

	if (ERROR_FILE_NOT_FOUND == error)
	{
		logErrorf(L"File not found. Failed to delete file %ls", fullPath);
		return false;
	}
	logErrorf(L"Failed to delete file %ls. Reason: %ls", fullPath, getErrorText(error).c_str());
	return false;
	#else

	String file = toLinuxPath(validFullPath);
	if (remove(file.c_str()) == 0)
		return true;
	EACOPY_NOT_IMPLEMENTED
	return false;
	#endif
}

bool moveFile(const wchar_t* source, const wchar_t* dest, IOStats& ioStats)
{
	if (MoveFileExW(source, dest, MOVEFILE_REPLACE_EXISTING))
		return true;
	logErrorf(L"Failed to move file from %ls to %ls. Reason: %ls", source, dest, getLastErrorText().c_str());
	return false;
}

bool setFileWritable(const wchar_t* fullPath, bool writable)
{
	#if defined(_WIN32)
	WString temp;
	fullPath = convertToShortPath(fullPath, temp);
	return SetFileAttributesW(fullPath, writable ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY) != 0;
	#else
	String file = toLinuxPath(fullPath);
	int mode = S_IREAD;
	if (writable)
		mode |= S_IWRITE;
    if (chmod(file.c_str(), mode) == 0)
		return true;
	EACOPY_NOT_IMPLEMENTED
	return false;
	#endif
}

bool setFileHidden(const wchar_t* fullPath, bool hidden)
{
#if defined(_WIN32)
	WString temp;
	fullPath = convertToShortPath(fullPath, temp);
	return SetFileAttributesW(fullPath, hidden ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL) != 0;
#else
	EACOPY_NOT_IMPLEMENTED
#endif
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

u64 getTime()
{
	#if defined(_WIN32)
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	LARGE_INTEGER li;
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;
	return (li.QuadPart - 116444736000000000LL);
	#else
	timeval tv;
    u64 result = 11644473600LL;
    gettimeofday(&tv, NULL);
    result += tv.tv_sec;
    result *= 10000000LL;
    result += tv.tv_usec * 10;
    return result;
#endif
}

bool equalsIgnoreCase(const wchar_t* a, const wchar_t* b)
{
	return _wcsicmp(a, b) == 0;
}

bool lessIgnoreCase(const wchar_t* a, const wchar_t* b)
{
	return _wcsicmp(a, b) < 0;
}

bool startsWithIgnoreCase(const wchar_t* str, const wchar_t* substr)
{
	#if defined(_WIN32)
	return StrStrIW(str, substr) == str;
	#else
	size_t strLen = wcslen(str);
	size_t substrLen = wcslen(substr);
	if (strLen < substrLen)
		return false;
	return wcsncasecmp(str, substr, substrLen) == 0;
	#endif
}

WString getErrorText(uint error)
{
	#if defined(_WIN32)
	uint dwRet;
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
	#else
	wchar_t buffer[128];
	swprintf(buffer, 128, L"ERROR: %i", error);
	return WString(buffer);
	#endif
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
	#if defined(_WIN32)
	uint dwSession;
	WCHAR szSessionKey[CCH_RM_SESSION_KEY+1] = { 0 };
	if (uint dwError = RmStartSession(&dwSession, 0, szSessionKey))
		return L"";
	ScopeGuard sessionGuard([&]() { RmEndSession(dwSession); });

	if (uint dwError = RmRegisterResources(dwSession, 1, &resourceName, 0, NULL, 0, NULL))
		return L"";

	WString result;

	uint dwReason;
	UINT nProcInfoNeeded;
	UINT nProcInfo = 10;
	RM_PROCESS_INFO rgpi[10];
	if (uint dwError = RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, rgpi, &dwReason))
		return L"";

	for (uint i = 0; i < nProcInfo; i++)
	{
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rgpi[i].Process.dwProcessId);
		if (hProcess == InvalidFileHandle)
			continue;
		ScopeGuard processGuard([&]() { CloseHandle(hProcess); });

		FILETIME ftCreate, ftExit, ftKernel, ftUser;
		if (!GetProcessTimes(hProcess, &ftCreate, &ftExit, &ftKernel, &ftUser) && CompareFileTime(&rgpi[i].Process.ProcessStartTime, &ftCreate) == 0)
			continue;
		WCHAR sz[MaxPath];
		uint cch = MaxPath;
		if (!QueryFullProcessImageNameW(hProcess, 0, sz, &cch))
			continue;

		if (!result.empty())
			result += L", ";
		result += sz;
	}

	return std::move(result);
	#else
	return WString();
	#endif
}

WString toPretty(u64 bytes, uint alignment)
{
	auto fmtValue = [](u64 v) -> float { auto d = (double)v; int it = 3; while (d > 1000.0 && it--) d /= 1000.0; return (float)d; };
	auto fmtSuffix = [](u64 v) -> const wchar_t* { return v > 1000*1000*1000 ? L"g" : (v > 1000*1000 ? L"m" : (v > 1000 ? L"k" : L"b")); };
	float value = fmtValue(bytes);
	const wchar_t* suffix = fmtSuffix(bytes);

	wchar_t buffer[128];
	wchar_t* dest = buffer + 64;

	StringCbPrintfW(dest, 64, L"%.1f%ls", value, suffix);

	uint len = (uint)wcslen(dest);
	while (len <= alignment)
	{
		--dest;
		*dest = L' ';
		++len;
	}
	return dest;
}

WString toHourMinSec(u64 time, uint alignment)
{
	u64 timeMs = time / 10000;
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
		itow((int)days, printfDest, 64);
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

void itow(int value, wchar_t* dst, uint dstCapacity)
{
	#if defined(_WIN32)
	_itow_s(value, dst, dstCapacity, 10);
	#else
	swprintf(dst, dstCapacity, L"%d", value);
	#endif
}

int stringEquals(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b) == 0; }
int	stringEquals(const char* a, const char* b) { return strcmp(a, b) == 0; }
bool stringCopy(wchar_t* dest, uint destCapacity, const wchar_t* source) { return wcscpy_s(dest, destCapacity, source) == 0; }
WString getVersionString(uint major, uint minor, bool isDebug)
{
	wchar_t buffer[64];
	StringCbPrintfW(buffer, 64, L"%u.%02u %ls", major, minor, isDebug ? L"DBG" : L"");
	return buffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool
FileKey::operator<(const FileKey& o) const
{
	// Sort by name first (we need this for delta-copy)
	int cmp = wcscmp(name.c_str(), o.name.c_str());
	if (cmp != 0)
		return cmp < 0;

	// Sort by write time first (we need this for delta-copy)
	LONG timeDiff = CompareFileTime((FILETIME*)&lastWriteTime, (FILETIME*)&o.lastWriteTime);
	if (timeDiff != 0)
		return timeDiff < 0;

	return fileSize < o.fileSize;
}

FileDatabase::FileRec
FileDatabase::getRecord(const FileKey& key)
{
	ScopedCriticalSection cs(m_localFilesCs);
	auto findIt = m_localFiles.find(key);
	if (findIt != m_localFiles.end())
		return findIt->second;
	return FileRec();
}

FileDatabase::FileRec
FileDatabase::getRecord(const Hash& hash)
{
	auto findIt = m_localFileHashes.find(hash);
	if (findIt != m_localFileHashes.end())
		return *findIt->second;
	return {};
}

uint
FileDatabase::getHistorySize()
{
	ScopedCriticalSection cs(m_localFilesCs);
	return (uint)m_localFiles.size();
}

bool
FileDatabase::findFileForDeltaCopy(WString& outFile, const FileKey& key)
{
	ScopedCriticalSection cs(m_localFilesCs);
	FileKey searchKey { key.name, 0, 0 };
	auto searchIt = m_localFiles.lower_bound(searchKey);
	while (searchIt != m_localFiles.end())
	{
		if (searchIt->first.name != key.name)
			return false;
		outFile = searchIt->second.name;
		return true;
	}
	return false;
}

void
FileDatabase::addToLocalFilesHistory(const FileKey& key, const Hash& hash, const WString& fullFileName)
{
	ScopedCriticalSection cs(m_localFilesCs);
	auto insres = m_localFiles.insert({key, FileRec()});
	if (!insres.second)
		m_localFilesHistory.erase(insres.first->second.historyIt);
	m_localFilesHistory.push_back(key);
	FileRec& rec = insres.first->second;
	rec.name = fullFileName;
	rec.hash = hash;
	rec.historyIt = --m_localFilesHistory.end();
	if (isValid(hash))
		m_localFileHashes[hash] = &rec;
}

uint
FileDatabase::garbageCollect(uint maxHistory)
{
	ScopedCriticalSection cs(m_localFilesCs);
	if (m_localFilesHistory.size() < maxHistory)
		return 0;
	uint removeCount = (uint)m_localFilesHistory.size() - maxHistory;
	uint it = removeCount;
	while (it--)
	{
		auto findIt = m_localFiles.find(m_localFilesHistory.front());
		auto hashFindIt = m_localFileHashes.find(findIt->second.hash);
		if (hashFindIt->second == &findIt->second)
			m_localFileHashes.erase(hashFindIt);
		m_localFiles.erase(findIt);
		m_localFilesHistory.pop_front();
	}
	return removeCount;
}

bool
FileDatabase::primeDirectory(const WString& directory, IOStats& ioStats, bool flush)
{
	ScopedCriticalSection cs(m_primeDirsCs);
	m_primeDirs.push_back(directory);
	if (!flush)
		return true;
	while (primeUpdate(ioStats))
		;
	return true;
}

bool
FileDatabase::primeUpdate(IOStats& ioStats)
{
	WString directory;
	m_primeDirsCs.scoped([&]()
		{
			if (!m_primeDirs.empty())
			{
				directory = std::move(m_primeDirs.front());
				m_primeDirs.pop_front();
				++m_primeActive;
			}
		});

	// If no new directory queued
	if (directory.empty())
		return false;

	ScopeGuard activeGuard([this]()
		{
			m_primeDirsCs.scoped([this]() { --m_primeActive; });
		});

    FindFileData fd; 
    WString searchStr = directory + L"*.*";
	FindFileHandle fh = findFirstFile(searchStr.c_str(), fd, ioStats);
    if(fh == InvalidFileHandle)
	{
		logErrorf(L"FindFirstFile failed with search string %ls", searchStr.c_str());
		return false;
	}

	ScopeGuard _([&]() { findClose(fh, ioStats); });
    do
	{
		FileInfo fileInfo;
		uint attr = getFileInfo(fileInfo, fd);
		if ((attr & FILE_ATTRIBUTE_HIDDEN))
			continue;
		const wchar_t* fileName = getFileName(fd);
		if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			if (isDotOrDotDot(fileName))
				continue;
			ScopedCriticalSection cs(m_primeDirsCs);
			m_primeDirs.push_back(directory + fileName + L'\\');
		}
		else
		{
			Hash hash; // TODO: Should use hashes also calculate hash for files?
			addToLocalFilesHistory({ fileName, fileInfo.lastWriteTime, fileInfo.fileSize }, hash, directory + fileName);
		}
	} 
	while(findNextFile(fh, fd, ioStats)); 

	uint error = GetLastError();
	if (error != ERROR_NO_MORE_FILES)
	{
		logErrorf(L"FindNextFile failed for %ls: %ls", searchStr.c_str(), getErrorText(error).c_str());
		return false;
	}

	return true;
}

bool
FileDatabase::primeWait(IOStats& ioStats)
{
	while (true)
	{
		if (primeUpdate(ioStats))
			continue;
		ScopedCriticalSection cs(m_primeDirsCs);
		if (m_primeActive == 0 && m_primeDirs.empty())
			break;
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

HashContext::HashContext(u64& time, u64& count)
:	m_time(time)
,	m_count(count)
{
}

bool
HashContext::init()
{
	TimerScope _(m_time); // Skip these since they makes the user think hashing happens when it is not
	if (CryptAcquireContext(&(HCRYPTPROV&)m_handle, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
		return true;
	logErrorf(L"CryptAcquireContext failed: %ls", getLastErrorText().c_str());
	return false;
}

HashContext::~HashContext()
{
	if (!m_handle)
		return;
	TimerScope _(m_time); // Skip these since they makes the user think hashing happens when it is not
	CryptReleaseContext((HCRYPTPROV&)m_handle, 0);
}

HashBuilder::HashBuilder(HashContext& c) : m_context(c)
{
	if (!m_context.m_handle)
		m_context.init();

	++m_context.m_count;
	TimerScope _(m_context.m_time);

	if (!CryptCreateHash((HCRYPTPROV&)m_context.m_handle, CALG_MD5, 0, 0, &(HCRYPTHASH&)m_handle))
		logErrorf(L"CryptCreateHash failed: %ls", getLastErrorText().c_str());
}

HashBuilder::~HashBuilder()
{
	TimerScope _(m_context.m_time);
	CryptDestroyHash((HCRYPTHASH&)m_handle);
}

bool
HashBuilder::add(u8* data, u64 size)
{
	TimerScope _(m_context.m_time);
	if (CryptHashData((HCRYPTHASH&)m_handle, data, size, 0))
		return true;
	logErrorf(L"CryptHashData failed: %ls", getLastErrorText().c_str());
	return false;
}

bool
HashBuilder::getHash(Hash& outHash)
{
	TimerScope _(m_context.m_time);
	DWORD cbHash = sizeof(Hash);
	if (CryptGetHashParam((HCRYPTHASH&)m_handle, HP_HASHVAL, (BYTE*)&outHash, &cbHash, 0))
		return true;
	logErrorf(L"CryptGetHashParam failed: %ls", getLastErrorText().c_str());
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
