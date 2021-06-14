// (c) Electronic Arts. All Rights Reserved.

#pragma once

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)

#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool sendDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* newFileName, u64 newFileSize, CopyContext& copyContext, IOStats& ioStats);


struct RecvDeltaStats
{
	u64			recvTime = 0;
	u64			recvSize = 0;
};


bool receiveDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* destFileName, u64 destFileSize, FileTime lastWriteTime, CopyContext& copyContext, IOStats& ioStats, RecvDeltaStats& recvStats);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
