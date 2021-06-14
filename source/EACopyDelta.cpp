// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)

#include <EACopyDelta.h>
#include "EACopyDeltaZstd.h"
#include "EACopyDeltaXDelta.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using CodeFunc = bool(bool encode, Socket& socket, const wchar_t* referenceFileName, FileHandle referenceFile, u64 referenceFileSize, const wchar_t* newFileName, FileHandle newFile, u64 newFileSize, CopyContext& copyContext, IOStats& ioStats, u64& socketTime, u64& socketSize);

CodeFunc* codeFunc = zstdCode;

bool
sendDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* newFileName, u64 newFileSize, CopyContext& copyContext, IOStats& ioStats)
{
	FileHandle newFile;
	if (!openFileRead(newFileName, newFile, ioStats, true))
		return false;
	ScopeGuard _([&]() { closeFile(newFileName, newFile, AccessType_Read, ioStats); });

	FileHandle referenceFile;
	if (!openFileRead(referenceFileName, referenceFile, ioStats, true, nullptr, false))
		return false;
	ScopeGuard _2([&]() { closeFile(referenceFileName, referenceFile, AccessType_Read, ioStats); });

	u64 socketTime = 0;
	u64 socketBytes = 0;
	return codeFunc(true, socket, referenceFileName, referenceFile, referenceFileSize, newFileName, newFile, newFileSize, copyContext, ioStats, socketTime, socketBytes);
}

bool receiveDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* destFileName, u64 destFileSize, FileTime lastWriteTime, CopyContext& copyContext, IOStats& ioStats, RecvDeltaStats& recvStats)
{
	u8* deltaBuffer = copyContext.buffers[0];
	uint deltaBufferSize;

	WString tempFileName;
	const wchar_t* lastSlash = wcsrchr(destFileName, L'\\');
	if (lastSlash)
	{
		tempFileName.append(destFileName, lastSlash + 1);
		tempFileName += L'.';
		tempFileName += lastSlash + 1;
	}
	else
	{
		tempFileName += L'.';
		tempFileName += destFileName;
	}

	{
		FileHandle referenceFile;
		if (!openFileRead(referenceFileName, referenceFile, ioStats, true, nullptr, false))
			return false;
		ScopeGuard _([&]() { closeFile(referenceFileName, referenceFile, AccessType_Read, ioStats); });

		FileHandle tempFile;
		if (!openFileWrite(tempFileName.c_str(), tempFile, ioStats, true))
			return false;
		ScopeGuard _2([&]() { closeFile(tempFileName.c_str(), tempFile, AccessType_Write, ioStats); });

		if (!codeFunc(false, socket, referenceFileName, referenceFile, referenceFileSize, tempFileName.c_str(), tempFile, destFileSize, copyContext, ioStats, recvStats.recvTime, recvStats.recvSize))
			return false;

		if (!setFileLastWriteTime(tempFileName.c_str(), tempFile, lastWriteTime, ioStats))
			return false;
	}

	wprintf(L"Received: %ls (time: %ls)\r\n", toPretty(recvStats.recvSize, 7).c_str(), toHourMinSec(recvStats.recvTime, 7).c_str());

	if (!moveFile(tempFileName.c_str(), destFileName, ioStats))
		return false;

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
