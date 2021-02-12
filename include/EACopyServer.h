// (c) Electronic Arts. All Rights Reserved.

#pragma once

#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { DefaultHistorySize = 500000 }; // Number of files 
constexpr char ServerVersion[] = "0.934" CFG_STR; // Version of server

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using ReportServerStatus = Function<bool(uint dwCurrentState, uint dwWin32ExitCode, uint dwWaitHint)>;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ServerSettings
{
	uint			listenPort					= DefaultPort;
	uint			maxHistory					= DefaultHistorySize;
	bool			useSecurityFile				= false;
	bool			logDebug					= false;
	UseBufferedIO	useBufferedIO				= UseBufferedIO_Auto;
	WString			primingDirectory;
	uint			maxConcurrentDownloadCount	= 100;
	WString			user;
	WString			password;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Server
{
public:
					Server() {}
					~Server() {}

					// Starts the server. Call will not return until some other thread call stop() or
					// if it is started with isConsole true, then it will also exit on pressing esc or 'q'
	void			start(const ServerSettings& settings, Log& log, bool isConsole, ReportServerStatus reportStatus);

					// Stops the server. This call will return before the server is fully stopped. When start() returns server is stopped
	void			stop();

					// Will parse provided directory and add all found files to history
	bool			primeDirectory(const wchar_t* directory);

private:
	struct			ConnectionInfo;
	struct			FileKey;

	uint			connectionThread(ConnectionInfo& info);
	void			addToLocalFilesHistory(const FileKey& key, const WString& fullFileName);
	bool			getLocalFromNet(WString& outServerDirectory, bool& outIsExternalDirectory, const wchar_t* netDirectory);
	bool			primeDirectoryRecursive(const WString& directory);
	bool			findFileForDeltaCopy(WString& outFile, const FileKey& key);

	using			FilesHistory = List<FileKey>;
	struct			FileRec { WString name; FilesHistory::iterator historyIt; };
	using			FilesMap = Map<FileKey, FileRec>;

	FilesMap		m_localFiles;
	FilesHistory	m_localFilesHistory;
	CriticalSection	m_localFilesCs;

	struct			GuidLess { bool operator()(const Guid& a, const Guid& b) const { return memcmp(&a, &b, sizeof(Guid)) < 0; } };
	using			GuidSet = Set<Guid, GuidLess>;

	GuidSet			m_validSecretGuids;
	CriticalSection m_validSecretGuidsCs;

	u64				m_startTime;
	bool			m_isConsole = false;
	bool			m_loopServer = true;
	SOCKET			m_listenSocket = INVALID_SOCKET;

	// Stats
	u64				m_bytesCopied = 0;
	u64				m_bytesReceived = 0;
	u64				m_bytesLinked = 0;
	u64				m_bytesSkipped = 0;
	uint			m_activeConnectionCount = 0;
	uint			m_handledConnectionCount = 0;

	// Working state for priority
	using			Queue = std::vector<void*>;
	enum			{ MaxPriorityQueueCount = 32 };
	Queue			m_queues[MaxPriorityQueueCount];
	CriticalSection	m_queuesCs;


					Server(const Server&) = delete;
	void			operator=(const Server&) = delete;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct Server::ConnectionInfo
{
	ConnectionInfo(Log& l, const ServerSettings& s, Socket so) : log(l), settings(s), socket(so) {}

	Log& log;
	const ServerSettings& settings;
	Thread* thread = nullptr;
	Socket socket;
};

struct Server::FileKey
{
	WString name;
	FileTime lastWriteTime;
	u64 fileSize;

	bool operator<(const FileKey& o) const;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
