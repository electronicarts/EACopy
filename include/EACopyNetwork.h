// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyShared.h"
#include <winsock2.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { ProtocolVersion = 7 };	// Network protocol version.. must match EACopy and EACopyService otherwise it will fallback to non-server copy behavior
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
	CommandType_Done,			// Tell server that connection is done copying and can close
	CommandType_RequestReport	// Ask server for a status report
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
	ReadResponse_BadSource
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

struct DoneCommand : Command
{
};


struct RequestReportCommand : Command
{
};


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Network utils

const wchar_t*	optimizeUncPath(const wchar_t* uncPath, WString& temp, bool allowLocal = true);
bool			sendData(SOCKET socket, const void* buffer, uint size);
bool			receiveData(SOCKET socket, void* buffer, uint size);
bool			setBlocking(SOCKET socket, bool blocking);
bool			disableNagle(SOCKET socket);
bool			setSendBufferSize(SOCKET socket, uint sendBufferSize);
bool			setRecvBufferSize(SOCKET socket, uint recvBufferSize);

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

bool sendFile(SOCKET socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, CopyContext& copyContext, CompressionData& compressionData, bool useBufferedIO, CopyStats& copyStats, SendFileStats& sendStats);

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

bool receiveFile(bool& outSuccess, SOCKET socket, const wchar_t* fullPath, size_t fileSize, FILETIME lastWriteTime, WriteFileType writeType, bool useUnbufferedIO, NetworkCopyContext& copyContext, char* recvBuffer, uint recvPos, uint& commandSize, CopyStats& copyStats, RecvFileStats& recvStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}
