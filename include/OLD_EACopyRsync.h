// (c) Electronic Arts. All Rights Reserved.

#pragma once

#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct RsyncStats
{
	u64	readTimeMs = 0;
	u64 readSize = 0;

	u64 receiveTimeMs = 0;
	u64	writeTimeMs = 0;
	u64 sendTimeMs = 0;
	u64 sendSize = 0;

	u64 rsyncTimeMs = 0;
};

bool serverHandleRsync(SOCKET socket, const wchar_t* fileName, const wchar_t* newFileName, FILETIME lastWriteTime, RsyncStats& outStats);
bool clientHandleRsync(SOCKET socket, const wchar_t* fileName, RsyncStats& outStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}
