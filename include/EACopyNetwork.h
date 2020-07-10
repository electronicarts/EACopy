// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyShared.h"
#include <winsock2.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { ProtocolVersion = 11 };	// Network protocol version.. must match EACopy and EACopyService otherwise it will fallback to non-server copy behavior
enum : uint { DefaultPort = 18099 };	// Default port for client and server to connect. Can be overridden with command line
enum : uint { DefaultDeltaCompressionThreshold = 1024 * 1024 }; // Default threshold for filesize to use delta compression


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Protocol for communication between EACopy and Server

enum CommandType : u8
{
	CommandType_Version,		// Version
	CommandType_Text,			// Text message.. Currently not used but can be used to communicate message to server from client
	CommandType_WriteFile,		// Write file from client to server
	CommandType_ReadFile,		// Read file from server to client
	CommandType_CreateDir,		// Create directory on server
	CommandType_Environment,	// Sends information to server about where all relative file paths should be stored
	CommandType_DeleteFiles,	// Tell server to delete files
	CommandType_FindFiles,		// Return list of files/directories
	CommandType_Done,			// Tell server that connection is done copying and can close
	CommandType_RequestReport,	// Ask server for a status report
	CommandType_GetFileInfo,	// Get file info for file/directory on server side
};


struct Command
{
	uint commandSize;
	CommandType commandType;
};


struct VersionCommand : Command
{
	uint protocolVersion;
};


struct EnvironmentCommand : Command
{
	u64 deltaCompressionThreshold;
	wchar_t netDirectory[1];
	uint connectionIndex;
};


struct TextCommand : Command
{
	int stringLength;
	wchar_t string[1];
};


enum WriteFileType : u8
{
	WriteFileType_TransmitFile,
	WriteFileType_Send,
	WriteFileType_Compressed
};

struct WriteFileCommand : Command
{
	WriteFileType writeType;
	FileInfo info;
	wchar_t path[1];
};

enum WriteResponse : u8
{
	WriteResponse_Copy,
	WriteResponse_CopyDelta,
	WriteResponse_Link,
	WriteResponse_Skip,
	WriteResponse_BadDestination
};

struct ReadFileCommand : Command
{
	u8 compressionEnabled;
	FileInfo info;
	wchar_t path[1];
};

enum ReadResponse : u8
{
	ReadResponse_Copy,
	ReadResponse_CopyDelta,
	ReadResponse_Skip,
	ReadResponse_BadSource,
	ReadResponse_ServerBusy,
};

struct CreateDirCommand : Command
{
	wchar_t path[1];
};

enum CreateDirResponse : u8
{
	CreateDirResponse_Success,
	CreateDirResponse_Error,
	CreateDirResponse_BadDestination,
};

struct DeleteFilesCommand : Command
{
	wchar_t path[1];
};

enum DeleteFilesResponse : u8
{
	DeleteFilesResponse_Success,
	DeleteFilesResponse_Error,
	DeleteFilesResponse_BadDestination,
};

struct FindFilesCommand : Command
{
	wchar_t pathAndWildcard[1];
};

struct DoneCommand : Command
{
};


struct RequestReportCommand : Command
{
};

struct GetFileInfoCommand : Command
{
	wchar_t path[1];
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Network utils

struct Socket
{
	SOCKET socket;
	bool valid;
};

const wchar_t*	optimizeUncPath(const wchar_t* uncPath, WString& temp, bool allowLocal = true);
bool			sendData(Socket& socket, const void* buffer, uint size);
bool			receiveData(Socket& socket, void* buffer, uint size);
bool			setBlocking(Socket& socket, bool blocking);
bool			disableNagle(Socket& socket);
bool			setSendBufferSize(Socket& socket, uint sendBufferSize);
bool			setRecvBufferSize(Socket& socket, uint recvBufferSize);
void			closeSocket(Socket& socket);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct CompressionData
{
	bool		fixedLevel = false;
	int			level = 0;
	void*		context = nullptr;

	int			lastLevel = 0;
	u64			lastWeight = 0;

	~CompressionData();
};

struct SendFileStats
{
	u64			sendTimeMs = 0;
	u64			sendSize = 0;
	u64			compressTimeMs = 0;
	u64			compressionLevelSum = 0;
};

bool sendFile(Socket& socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, CopyContext& copyContext, CompressionData& compressionData, bool useBufferedIO, CopyStats& copyStats, SendFileStats& sendStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum { NetworkTransferChunkSize = CopyContextBufferSize };

struct NetworkCopyContext : CopyContext
{
	void* compContext = nullptr;

	~NetworkCopyContext();
};

struct RecvFileStats
{
	u64			recvTimeMs = 0;
	u64			recvSize = 0;
	u64			decompressTimeMs = 0;
};

bool receiveFile(bool& outSuccess, Socket& socket, const wchar_t* fullPath, size_t fileSize, FILETIME lastWriteTime, WriteFileType writeType, bool useUnbufferedIO, NetworkCopyContext& copyContext, char* recvBuffer, uint recvPos, uint& commandSize, CopyStats& copyStats, RecvFileStats& recvStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}
