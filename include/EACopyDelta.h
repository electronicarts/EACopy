// (c) Electronic Arts. All Rights Reserved.

#pragma once

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)

#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool sendDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* newFileName, CopyContext& copyContext, IOStats& ioStats);


struct RecvDeltaStats
{
	u64			recvTime = 0;
	u64			recvSize = 0;
};


bool receiveDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* destFileName, FileTime lastWriteTime, CopyContext& copyContext, u64& written, IOStats& ioStats, RecvDeltaStats& recvStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
