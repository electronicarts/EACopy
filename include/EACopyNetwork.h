// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyShared.h"
#include <cstring>

typedef eacopy::u64 SOCKET;
#define INVALID_SOCKET  (SOCKET)(~0)

struct Guid {
    unsigned long  Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char  Data4[8];
	bool operator==(const Guid& other) const { return memcmp(this, &other, sizeof(Guid)) == 0; }
	bool operator!=(const Guid& other) const { return memcmp(this, &other, sizeof(Guid)) != 0; }
};

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { ProtocolVersion = 20 };	// Network protocol version.. must match EACopy and EACopyService otherwise it will fallback to non-server copy behavior
enum : uint { DefaultPort = 18099 };	// Default port for client and server to connect. Can be overridden with command line


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Protocol for communication between EACopy and Server

//#define EACOPY_COMMAND
#define EACOPY_COMMANDS \
	EACOPY_COMMAND(Version) 		/* Version */ \
	EACOPY_COMMAND(Text) 			/* Text message..Currently not used but can be used to communicate message to server from client */ \
	EACOPY_COMMAND(WriteFile) 		/* Write file from client to server */ \
	EACOPY_COMMAND(ReadFile) 		/* Read file from server to client */ \
	EACOPY_COMMAND(CreateDir) 		/* Create directory on server */ \
	EACOPY_COMMAND(Environment) 	/* Sends information to server about where all relative file paths should be stored */ \
	EACOPY_COMMAND(DeleteFiles) 	/* Tell server to delete files */ \
	EACOPY_COMMAND(FindFiles) 		/* Return list of files/directories */ \
	EACOPY_COMMAND(Done) 			/* Tell server that connection is done copying and can close */ \
	EACOPY_COMMAND(RequestReport) 	/* Ask server for a status report */ \
	EACOPY_COMMAND(GetFileInfo) 	/* Get file info for file/directory on server side */ \

#define EACOPY_COMMAND(x) CommandType_##x,

enum CommandType : u8
{
	EACOPY_COMMANDS
	CommandType_Bad,
};

#undef EACOPY_COMMAND

struct Command
{
	uint commandSize;
	CommandType commandType;
};

enum ProtocolFlags : u8
{
	UseSecurityFile = 1,
};

struct VersionCommand : Command
{
	uint protocolVersion;
	uint protocolFlags;
	wchar_t info[1];
};


struct EnvironmentCommand : Command
{
	uint connectionIndex;
	uint majorVersion;
	uint minorVersion;
	Guid secretGuid;
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
	WriteResponse_CopyUsingSmb,
	WriteResponse_Link,
	WriteResponse_Odx,
	WriteResponse_Skip,
	WriteResponse_Hash,
	WriteResponse_BadDestination // Must be last!
};

struct ReadFileCommand : Command
{
	u8 compressionLevel; // 0 means no compression, 255 means dynamic compression
	FileInfo info;
	wchar_t path[1];
};

enum ReadResponse : u8
{
	ReadResponse_Copy,
	ReadResponse_CopyDelta,
	ReadResponse_CopyUsingSmb,
	ReadResponse_Skip,
	ReadResponse_Hash,
	ReadResponse_ServerBusy,
	ReadResponse_BadSource, // Must be last!
};

struct CreateDirCommand : Command
{
	wchar_t path[1];
};

enum CreateDirResponse : u8
{
	CreateDirResponse_Error,
	CreateDirResponse_BadDestination,
	CreateDirResponse_SuccessExisted,
	CreateDirResponse_SuccessCreated,
	// DO NOT ADD HERE, add above SuccessCreated.. "value - CreateDirResponse_SuccessExisted" is used to figure out how many directories up created
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
	uint index;
};

const wchar_t*	optimizeUncPath(const wchar_t* uncPath, WString& temp, bool allowLocal = true);
bool			sendData(Socket& socket, const void* buffer, uint size);
bool			receiveData(Socket& socket, void* buffer, uint size);
bool			setBlocking(Socket& socket, bool blocking);
bool			disableNagle(Socket& socket);
bool			setSendBufferSize(Socket& socket, uint sendBufferSize);
bool			setRecvBufferSize(Socket& socket, uint recvBufferSize);
void			closeSocket(Socket& socket);
bool			isValidSocket(Socket& socket);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct CompressionStats
{
	CriticalSection		lock;
	u64					sendTime[8] = { 0 };
	uint				sendBytes[8] = { 0 };
	u64					activeSendTime = 0;
	u64					activeSendBytes = 0;

	u64					currentSendTime = 0;
	u64					currentSendBytes = 0;
	uint				currentIndex = 0;

	u64					lastTimeUnitPerBytes = 0;

	bool				fixedLevel = false;
	int					currentLevel = 0;
};

enum { NetworkTransferChunkSize = CopyContextBufferSize };

struct NetworkCopyContext : CopyContext
{
	void* compContext = nullptr;
	void* decompContext = nullptr;

	~NetworkCopyContext();
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct SendFileStats
{
	u64			sendTime = 0;
	u64			sendSize = 0;
	u64			compressTime = 0;
	u64			compressionLevelSum = 0;
};

bool sendFile(Socket& socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, NetworkCopyContext& copyContext, CompressionStats& compressionStats, bool useBufferedIO, IOStats& ioStats, SendFileStats& sendStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct RecvFileStats
{
	u64			recvTime = 0;
	u64			recvSize = 0;
	u64			decompressTime = 0;
};

bool receiveFile(bool& outSuccess, Socket& socket, const wchar_t* fullPath, size_t fileSize, FileTime lastWriteTime, WriteFileType writeType, bool useUnbufferedIO, NetworkCopyContext& copyContext, char* recvBuffer, uint recvPos, uint& commandSize, IOStats& ioStats, RecvFileStats& recvStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(_WIN32)
struct addrinfoW;
#define AddrInfo addrinfoW
#define freeAddrInfo FreeAddrInfoW
#else
struct addrinfo;
#define AddrInfo addrinfo
#define freeAddrInfo freeaddrinfo
#define SOCKET_ERROR (-1)
#endif

int getAddrInfoW(const wchar_t* name, const wchar_t* service, const AddrInfo* hints, AddrInfo** result);
int getLastNetworkError();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
