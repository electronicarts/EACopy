// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

constexpr wchar_t ClientVersion[] = L"0.9997" CFG_STR; // Version of client (visible when printing help info)

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
	StringList			excludeWildcardDirectories;
	StringList			optionalWildcards; // Will not causes error if source file fulfill optionalWildcards
	uint				threadCount					= 0;
	uint				retryWaitTimeMs				= 30 * 1000;
	uint				retryCount					= 1000000;
	int					dirCopyFlags				= FileFlags_Data | FileFlags_Attributes;
	bool				forceCopy					= false;
	bool				flattenDestination			= false;
	int					copySubdirDepth				= 0;
	bool				copyEmptySubdirectories		= false;
	bool				purgeDestination			= false;
	UseServer			useServer					= UseServer_Automatic;
	WString				serverAddress;
	uint				serverPort					= DefaultPort;
	uint				serverConnectTimeoutMs		= 500;
	u64					deltaCompressionThreshold	= ~u64(0);
	bool				compressionEnabled			= false;
	int					compressionLevel			= 0;
	bool				logProgress					= true;
	bool				logDebug					= false;
	UseBufferedIO		useBufferedIO				= UseBufferedIO_Auto;
	bool				replaceSymLinksAtDestination= true; // When writing to destination and a directory is a symlink we remove symlink and create real directory
	bool				useOptimizedWildCardFileSearch = true;
	bool				useFileLinks				= false;
	StringList			additionalLinkDirectories;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ClientStats
{
	u64					copyCount					= 0;
	u64					copySize					= 0;
	u64					copyTime					= 0;
	u64					skipCount					= 0;
	u64					skipSize					= 0;
	u64					skipTime					= 0;
	u64					linkCount					= 0;
	u64					linkSize					= 0;
	u64					linkTime					= 0;
	u64					createDirCount				= 0;
	u64					failCount					= 0;
	u64					retryCount					= 0;
	u64					connectTime					= 0;
	u64					sendTime					= 0;
	u64					sendSize					= 0;
	u64					recvTime					= 0;
	u64					recvSize					= 0;
	u64					purgeTime					= 0;
	u64					compressTime				= 0;
	u64					compressionLevelSum			= 0;
	float				compressionAverageLevel		= 0;
	u64					decompressTime				= 0;
	u64					deltaCompressionTime		= 0;
	IOStats				ioStats;

	bool				destServerUsed				= false;
	bool				sourceServerUsed			= false;
	bool				serverAttempt				= false;

	WString				info;
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
	struct				DirEntry { 	WString sourceDir; WString destDir; WString wildcard; int depthLeft; };
	using				HandleFileOrWildcardFunc = Function<bool(char*)>;
	using				CopyEntries = List<CopyEntry>;
	using				DirEntries = List<DirEntry>;
	using				CachedFindFileEntries = std::map<WString, Set<WString, NoCaseWStringLess>, NoCaseWStringLess>;
	class				Connection;
	struct				NameAndFileInfo { WString name; FileInfo info; uint attributes; };

	// Methods
	void				resetWorkState(Log& log);
	bool				processDir(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats);
	bool				processFile(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats);
	bool				processQueues(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats, bool isMainThread);
	bool				connectToServer(const wchar_t* networkPath, uint connectionIndex, Connection*& outConnection, bool& failedToConnect, ClientStats& stats);
	int					workerThread(uint connectionIndex, ClientStats& stats);
	bool				traverseFilesInDirectory(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, const WString& sourcePath, const WString& destPath, const WString& wildcard, int depthLeft, ClientStats& stats);
	bool				findFilesInDirectory(Vector<NameAndFileInfo>& outEntries, LogContext& logContext, Connection* connection, NetworkCopyContext& copyContext, const WString& path, ClientStats& stats);
	bool				addDirectoryToHandledFiles(LogContext& logContext, Connection* destConnection, const WString& destFullPath, ClientStats& stats);
	bool				handleFile(LogContext& logContext, Connection* destConnection, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const FileInfo& fileInfo, ClientStats& stats);
	bool				handleDirectory(LogContext& logContext, Connection* destConnection, const WString& sourcePath, const WString& destPath, const wchar_t* directory, const wchar_t* wildcard, int depthLeft, ClientStats& stats);
	bool				handleMissingFile(const wchar_t* fileName);
	bool				handlePath(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName);
	bool				handlePath(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, uint attributes, const FileInfo& fileInfo);
	bool				handleFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileOrWildcardFunc& func);
	bool				excludeFilesFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath);
	bool				gatherFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, CachedFindFileEntries& findFileCache, const WString& sourcePath, const WString& fileName, const WString& destPath);
	bool				processQueuedWildcardFileEntries(LogContext& logContext, ClientStats& stats, CachedFindFileEntries& findFileCache, const WString& rootSourcePath, const WString& rootDestPath);
	bool				purgeFilesInDirectory(const WString& destPath, uint destPathAttributes, int depthLeft, ClientStats& stats);
	bool				ensureDirectory(Connection* destConnection, const WString& directory, IOStats& ioStats);
	const wchar_t*		getRelativeSourceFile(const WString& sourcePath) const;
	Connection*			createConnection(const wchar_t* networkPath, uint connectionIndex, ClientStats& stats, bool& failedToConnect, bool doProtocolCheck);
	bool				isIgnoredDirectory(const wchar_t* directory);
	bool				isValid(Connection* connection);


	// Settings
	const ClientSettings& m_settings;

	// Work state. Will initialize at beginning of each process call.
	Log*				m_log;
	bool				m_useSourceServerFailed;
	bool				m_useDestServerFailed;
	Event				m_workDone;
	bool				m_tryCopyFirst;
	NetworkCopyContext	m_copyContext;
	Connection*			m_sourceConnection;
	Connection*			m_destConnection;
	CriticalSection		m_copyEntriesCs;
	CopyEntries			m_copyEntries;
	CriticalSection		m_dirEntriesCs;
	DirEntries			m_dirEntries;
	FilesSet			m_handledFiles;
	CriticalSection		m_handledFilesCs;
	FilesSet			m_createdDirs;
	CriticalSection		m_createdDirsCs;
	FilesSet			m_purgeDirs;
	CriticalSection		m_networkInitCs;
	bool				m_networkWsaInitDone;
	bool				m_networkInitDone;
	WString				m_networkServerName;
	WString				m_networkServerNetDirectory;
	AddrInfo*			m_serverAddrInfo;
	Guid				m_secretGuid;
	CriticalSection		m_secretGuidCs;
	FileDatabase		m_fileDatabase;

	CompressionStats	m_compressionStats;

						Client(const Client&) = delete;
	void				operator=(const Client&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Client::Connection
{
public:
						Connection(const ClientSettings& settings, ClientStats& stats, Socket s, CompressionStats& compressionStats);
						~Connection();
	bool				sendCommand(const Command& cmd);
	bool				sendTextCommand(const wchar_t* text);
	bool				sendWriteFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, u64& outSize, u64& outWritten, bool& outLinked, CopyContext& copyContext);

	enum				ReadFileResult { ReadFileResult_Error, ReadFileResult_Success, ReadFileResult_ServerBusy };
	ReadFileResult		sendReadFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, u64& outSize, u64& outRead, NetworkCopyContext& copyContext);


	bool				sendCreateDirectoryCommand(const wchar_t* directory, FilesSet& outCreatedDirs);
	bool				sendDeleteAllFiles(const wchar_t* dir);
	bool				sendFindFiles(const wchar_t* dirAndWildcard, Vector<NameAndFileInfo>& outFiles, CopyContext& copyContext);
	bool				sendGetFileAttributes(const wchar_t* file, FileInfo& outInfo, uint& outAttributes, uint& outError);

	bool				destroy();

	const ClientSettings& m_settings;
	ClientStats&		m_stats;

	Socket				m_socket;
	bool				m_compressionEnabled;
	CompressionData		m_compressionData;

						Connection(const Connection&) = delete;
	void				operator=(const Connection&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
