// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyShared.h"
#include <winsock2.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum : uint { ProtocolVersion = 4 };	// Network protocol version.. must match EACopy and EACopyService otherwise it will fallback to non-server copy behavior
enum : uint { DefaultPort = 18099 };	// Default port for client and server to connect. Can be overridden with command line



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Protocol for communication between EACopy and Server

enum CommandType : u8
{
	CommandType_Version,		// Version
	CommandType_Text,			// Text message.. Currently not used but can be used to communicate message to server from client
	CommandType_WriteFile,		// Write file from client to server
	CommandType_CreateDir,		// Create directory on server
	CommandType_Environment,	// Sends information to server about where all relative file paths should be stored
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
	WriteResponse_Link,
	WriteResponse_Skip,
	WriteResponse_BadDestination
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

}
