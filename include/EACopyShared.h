// (c) Electronic Arts. All Rights Reserved.

#pragma once
#undef UNICODE
#define WIN32_LEAN_AND_MEAN
#define _HAS_EXCEPTIONS 0
#include <windows.h>

#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global Constants

enum { CopyContextBufferSize = 8*1024*1024 }; // This is the chunk size used when reading/writing/copying files
enum { MaxPath = 4096 }; // Max path for EACopy

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Types

using								u8			= unsigned char;
using								uint		= unsigned int;
using								s64			= long long;
using								u64			= unsigned long long;
using								String		= std::string;
using								WString		= std::wstring;
template<class T> using				List		= std::list<T>;
template<class K, class V> using	Map			= std::map<K, V>;
template<class K, class L> using	Set			= std::set<K, L>;
template<class T> using				Vector		= std::vector<T>;
template<class T> using				Function	= std::function<T>;



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// ScopeGuard - Will call provided function when exiting scope

class ScopeGuard
{
public:
						ScopeGuard(Function<void()>&& f) : func(std::move(f)) {}
						~ScopeGuard() { func(); }
	void				cancel() { func = []() {}; }
	void				execute() { func(); func = []() {}; }

private:
	Function<void()>	func;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CriticalSection

class CriticalSection
{
public:
						CriticalSection() { InitializeCriticalSectionAndSpinCount(&cs, 0x00000400); }
						~CriticalSection() { DeleteCriticalSection(&cs); }

	void				enter() { EnterCriticalSection(&cs); }
	void				leave() { LeaveCriticalSection(&cs); }

private:
	CRITICAL_SECTION	cs;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Thread

class Event
{
public:
						Event();
						~Event();
	void				set();
	void				reset();
	bool				isSet(uint timeOutMs = INFINITE);

private:
	HANDLE				ev;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Thread

class Thread
{
public:
						Thread(Function<int()>&& func);
						~Thread();

	void				wait();
	bool				getExitCode(DWORD& outExitCode);

private:
	Function<int()>		func;
	HANDLE				handle;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Misc

u64						getTimeMs();
bool					equalsIgnoreCase(const wchar_t* a, const wchar_t* b);
bool					startsWithIgnoreCase(const wchar_t* str, const wchar_t* substr);
WString					getErrorText(uint error);
WString					getErrorText(const wchar_t* resourceName, uint error);
WString					getLastErrorText();
WString					getProcessesUsingResource(const wchar_t* resourceName);
WString					toPretty(u64 bytes, uint alignment = 0);
WString					toHourMinSec(u64 timeMs, uint alignment = 0);
String					toString(const wchar_t* str);
#define					eacopy_sizeof_array(array) int(sizeof(array)/sizeof(array[0]))



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// IO

struct FileInfo
{
	FILETIME			creationTime = { 0, 0 };
	FILETIME			lastWriteTime = { 0, 0 };
	u64					fileSize = 0;
};

struct CopyContext
{
						CopyContext();
						~CopyContext();
	u8*					buffers[3];
};

struct CopyStats
{
	u64					createReadTimeMs = 0;
	u64					readTimeMs = 0;
	u64					createWriteTimeMs = 0;
	u64					writeTimeMs = 0;
	u64					setLastWriteTimeTimeMs = 0;
};

struct					NoCaseWStringLess { bool operator()(const WString& a, const WString& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
using					FilesSet = Set<WString, NoCaseWStringLess>;

enum					UseBufferedIO { UseBufferedIO_Auto, UseBufferedIO_Enabled, UseBufferedIO_Disabled };
bool					getUseBufferedIO(UseBufferedIO use, u64 fileSize);

DWORD					getFileInfo(FileInfo& outInfo, const wchar_t* fullFileName);
bool					equals(const FileInfo& a, const FileInfo& b);
bool					ensureDirectory(const wchar_t* directory, bool replaceIfSymlink = false, bool expectCreationAndParentExists = true, FilesSet* outCreatedDirs = nullptr);
bool					deleteDirectory(const wchar_t* directory, bool errorOnMissingFile = true);
bool					deleteAllFiles(const wchar_t* directory, bool errorOnMissingFile = true);
bool					isAbsolutePath(const wchar_t* path);
bool					openFileRead(const wchar_t* fullPath, HANDLE& outFile, bool useBufferedIO, OVERLAPPED* overlapped = nullptr, bool isSequentialScan = true);
bool					openFileWrite(const wchar_t* fullPath, HANDLE& outFile, bool useBufferedIO, OVERLAPPED* overlapped = nullptr);
bool					writeFile(const wchar_t* fullPath, HANDLE& file, const void* data, u64 dataSize, OVERLAPPED* overlapped = nullptr);
bool					setFileLastWriteTime(const wchar_t* fullPath, HANDLE& file, FILETIME lastWriteTime);
bool					setFilePosition(const wchar_t* fullPath, HANDLE& file, u64 position);
bool					closeFile(const wchar_t* fullPath, HANDLE& file);
bool					createFile(const wchar_t* fullPath, const FileInfo& info, const void* data, bool useBufferedIO);
bool					createFileLink(const wchar_t* fullPath, const FileInfo& info, const wchar_t* sourcePath, bool& outSkip);
bool					copyFile(const wchar_t* source, const wchar_t* dest, bool failIfExists, bool& outExisted, u64& outBytesCopied, UseBufferedIO useBufferedIO);
bool					copyFile(const wchar_t* source, const wchar_t* dest, bool failIfExists, bool& outExisted, u64& outBytesCopied, CopyContext& copyContext, CopyStats& copyStats, UseBufferedIO useBufferedIO);
bool					deleteFile(const wchar_t* fullPath, bool errorOnMissingFile = true);
void					convertSlashToBackslash(wchar_t* path);
void					convertSlashToBackslash(wchar_t* path, size_t size);
void					convertSlashToBackslash(char* path);
void					convertSlashToBackslash(char* path, size_t size);
const wchar_t*			convertToShortPath(const wchar_t* path, WString& outTempBuffer);



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Logging

void					logErrorf(const wchar_t* fmt, ...);
void					logInfo(const wchar_t* str);
void					logInfof(const wchar_t* fmt, ...);
void					logInfoLinef(const wchar_t* fmt, ...);
void					logInfoLinef();
void					logDebugf(const wchar_t* fmt, ...);
void					logDebugLinef(const wchar_t* fmt, ...);
void					logScopeEnter();
void					logScopeLeave();


class Log
{
public:
	void				init(const wchar_t* logFile, bool logDebug, bool cacheRecentErrors);
	void				deinit(const Function<void()>& lastChanceLogging = Function<void()>());
	bool				isDebug() const { return m_logDebug; }
	void				traverseRecentErrors(const Function<bool(const WString&)>& errorFunc);

private:
	struct				LogEntry { WString str; bool linefeed; bool isError; };

	void				writeEntry(bool isDebuggerPresent, const LogEntry& entry);
	uint				processLogQueue(bool isDebuggerPresent);
	DWORD				logQueueThread();

	WString				m_logFileName;
	bool				m_logDebug = false;
	bool				m_cacheRecentErrors = false;
	CriticalSection		m_logQueueCs;
	List<LogEntry>*		m_logQueue = nullptr;
	List<WString>		m_recentErrors;
	WString				m_logLastText;
	bool				m_logQueueFlush = false;
	Thread*				m_logThread = nullptr;
	HANDLE				m_logFile = INVALID_HANDLE_VALUE;
	bool				m_logThreadActive = false;
	friend void			logInternal(const wchar_t* buffer, bool flush, bool linefeed, bool isError);
	friend void			logScopeEnter();
	friend void			logScopeLeave();
};

class LogContext
{
public:
						LogContext(Log& log);
						~LogContext();

	int					getLastError() const { return m_lastError; }
	void				resetLastError() { m_lastError = 0; }

	Log&				log;

private:
	LogContext*			m_lastContext;
	int					m_lastError = 0;
	friend void			logErrorf(const wchar_t* fmt, ...);
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(NDEBUG)
#define CFG_STR ""
#else
#define CFG_STR "DBG"
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
