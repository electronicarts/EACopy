// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char ClientVersion[] = "0.992"; // Version of client (visible when printing help info)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum FileFlags { FileFlags_Data = 1, FileFlags_Attributes = 2, FileFlags_Timestamps = 4 };
enum UseServer { UseServer_Automatic, UseServer_Required, UseServer_Disabled };


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ClientSettings
{
	using				StringList = List<WString>;

	WString				sourceDirectory;
	WString				destDirectory;
	StringList			filesOrWildcards;
	StringList			filesOrWildcardsFiles;
	StringList			filesExcludeFiles;
	StringList			excludeWildcards;
	StringList			optionalWildcards; // Will not causes error if source file fulfill optionalWildcards
	uint				threadCount					= 0;
	uint				retryWaitTimeMs				= 30 * 1000;
	uint				retryCount					= 1000000;
	int					dirCopyFlags				= FileFlags_Data | FileFlags_Attributes;
	bool				flattenDestination			= false;
	int					copySubdirDepth				= 0;
	bool				copyEmptySubdirectories		= false;
	bool				purgeDestination			= false;
	UseServer			useServer					= UseServer_Automatic;
	uint				serverPort					= DefaultPort;
	uint				serverConnectTimeoutMs		= 500;
	u64					deltaCompressionThreshold	= ~u64(0);
	bool				compressionEnabled			= false;
	int					compressionLevel			= 0;
	bool				logProgress					= true;
	bool				logDebug					= false;
	UseBufferedIO		useBufferedIO				= UseBufferedIO_Auto;
	bool				replaceSymLinksAtDestination= true; // When writing to destination and a folder is a symlink we remove symlink and create real directory
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ClientStats
{
	u64					copyCount					= 0;
	u64					copySize					= 0;
	u64					copyTimeMs					= 0;
	u64					skipCount					= 0;
	u64					skipSize					= 0;
	u64					skipTimeMs					= 0;
	u64					linkCount					= 0;
	u64					linkSize					= 0;
	u64					linkTimeMs					= 0;
	u64					failCount					= 0;
	u64					retryCount					= 0;
	u64					connectTimeMs				= 0;
	u64					sendTimeMs					= 0;
	u64					sendSize					= 0;
	u64					compressTimeMs				= 0;
	u64					compressionLevelSum			= 0;
	float				compressionAverageLevel		= 0;
	u64					deltaCompressionTimeMs		= 0;
	CopyStats			copyStats;

	bool				serverUsed					= false;
	bool				serverAttempt				= false;
	u64					findFileTimeMs				= 0;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Client
{
public:
						// Ctor
						Client(const ClientSettings& settings);

						// Process files from source to dest
	int					process(Log& log);
	int					process(Log& log, ClientStats& outStats);

						// Report server status using destination path
	int					reportServerStatus(Log& log);

private:

	// Types
	struct				CopyEntry { WString src; WString dst; FileInfo srcInfo; };
	using				HandleFileFunc = Function<bool()>;
	using				HandleFileOrWildcardFunc = Function<bool(char*)>;
	struct				NoCaseWStringLess { bool operator()(const WString& a, const WString& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
	using				FilesSet = Set<WString, NoCaseWStringLess>;
	using				CopyEntries = List<CopyEntry>;
	class				Connection;

	// Methods
	void				resetWorkState(Log& log);
	bool				processFile(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, CopyBuffer& copyBuffer, ClientStats& stats);
	bool				processFiles(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, bool isMainThread);
	bool				connectToServer(const wchar_t* networkPath, class Connection*& outConnection, bool& failedToConnect, ClientStats& stats);
	int					workerThread(ClientStats& stats);
	bool				findFilesInDirectory(const WString& sourcePath, const WString& destPath, const WString& wildcard, int depthLeft, const HandleFileFunc& handleFileFunc);
	bool				handleFile(const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const FileInfo& fileInfo, const HandleFileFunc& handleFileFunc);
	bool				handleDirectory(const WString& sourcePath, const WString& destPath, const wchar_t* directory, const wchar_t* wildcard, int depthLeft, const HandleFileFunc& handleFileFunc);
	bool				handlePath(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const HandleFileFunc& handleFileFunc);
	bool				handleFilesOrWildcardsFromFile(const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileOrWildcardFunc& func);
	bool				excludeFilesFromFile(const WString& sourcePath, const WString& fileName, const WString& destPath);
	bool				gatherFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileFunc& handleFileFunc);
	bool				purgeFilesInDirectory(const WString& destPath, int depthLeft);
	bool				ensureDirectory(const wchar_t* directory);
	const wchar_t*		getRelativeSourceFile(const WString& sourcePath) const;
	Connection*			createConnection(const wchar_t* networkPath, ClientStats& stats, bool& failedToConnect);


	// Settings
	const ClientSettings& m_settings;

	// Work state. Will initialize at beginning of each process call.
	Log*				m_log;
	bool				m_useSourceServerFailed;
	bool				m_useDestServerFailed;
	bool				m_workersActive;
	bool				m_tryCopyFirst;
	Connection*			m_destConnection;
	CriticalSection		m_copyEntriesCs;
	CopyEntries			m_copyEntries;
	FilesSet			m_handledFiles;
	FilesSet			m_purgeDirs;
	CriticalSection		m_networkInitCs;
	bool				m_networkWsaInitDone;
	bool				m_networkInitDone;
	WString				m_networkServerName;
	WString				m_networkServerNetDirectory;
	addrinfoW*			m_serverAddrInfo;

						Client(const Client&) = delete;
	void				operator=(const Client&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Client::Connection
{
public:
						Connection(const ClientSettings& settings, ClientStats& stats, SOCKET s);
						~Connection();
	bool				sendCommand(const Command& cmd);
	bool				sendTextCommand(const wchar_t* text);
	bool				sendWriteFileCommand(const wchar_t* src, const wchar_t* dst, u64& outSize, u64& outWritten, bool& outLinked, CopyBuffer& copyBuffer);
	bool				sendReadFileCommand(const wchar_t* src, const wchar_t* dst, u64& outSize, u64& outRead, CopyBuffer& copyBuffer);
	bool				sendCreateDirectoryCommand(const wchar_t* dst);
	bool				destroy();

	const ClientSettings& m_settings;
	ClientStats&		m_stats;

	SOCKET				m_socket;
	bool				m_compressionEnabled;
	CompressionData		m_compressionData;

						Connection(const Connection&) = delete;
	void				operator=(const Connection&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
