// (c) Electronic Arts. All Rights Reserved.

#pragma once

#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { DefaultHistorySize = 500000 }; // Number of files 
constexpr char ServerVersion[] = "0.86"; // Version of server

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using ReportServerStatus = Function<BOOL(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)>;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ServerSettings
{
	uint			listenPort		= DefaultPort;
	uint			maxHistory		= DefaultHistorySize;
	bool			logDebug		= false;
	UseBufferedIO	useBufferedIO	= UseBufferedIO_Auto;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Server
{
public:
					Server() {}
					~Server() {}

	void			start(const ServerSettings& settings, Log& log, bool isConsole, ReportServerStatus reportStatus);
	void			stop();

private:
	struct			ConnectionInfo;
	struct			FileKey;

	DWORD			connectionThread(ConnectionInfo& info);
	void			addToLocalFilesHistory(const FileKey& key, const WString& fullFileName);
	bool			findFileForDeltaCopy(WString& outFile, const FileKey& key);

	using			FilesHistory = List<FileKey>;
	struct			FileRec { WString name; FilesHistory::iterator historyIt; };
	using			FilesMap = Map<FileKey, FileRec>;

	FilesMap		m_localFiles;
	FilesHistory	m_localFilesHistory;
	CriticalSection	m_localFilesCs;

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

					Server(const Server&) = delete;
	void			operator=(const Server&) = delete;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct Server::ConnectionInfo
{
	ConnectionInfo(Log& l, const ServerSettings& s, SOCKET so) : log(l), settings(s), socket(so) {}

	Log& log;
	const ServerSettings& settings;
	Thread* thread = nullptr;
	SOCKET socket;
};

struct Server::FileKey
{
	WString name;
	FILETIME lastWriteTime;
	u64 fileSize;

	bool operator<(const FileKey& o) const
	{
		// Sort by name first (we need this for delta-copy)
		int cmp = wcscmp(name.c_str(), o.name.c_str());
		if (cmp != 0)
			return cmp < 0;

		// Sort by write time first (we need this for delta-copy)
		LONG timeDiff = CompareFileTime(&lastWriteTime, &o.lastWriteTime);
		if (timeDiff != 0)
			return timeDiff < 0;

		return fileSize < o.fileSize;
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
