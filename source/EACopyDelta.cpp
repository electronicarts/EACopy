// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)

#include <EACopyDelta.h>
#include "EACopyDeltaXDelta.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool
sendDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* newFileName, CopyContext& copyContext, IOStats& ioStats)
{
	FileHandle newFile;
	if (!openFileRead(newFileName, newFile, ioStats, true))
		return false;
	ScopeGuard _([&]() { closeFile(newFileName, newFile, AccessType_Read, ioStats); });

	FileHandle referenceFile;
	if (!openFileRead(referenceFileName, referenceFile, ioStats, true, nullptr, false))
		return false;
	ScopeGuard _2([&]() { closeFile(referenceFileName, referenceFile, AccessType_Read, ioStats); });

	u64 written = 0;
	u64 socketTime = 0;
	u64 socketBytes = 0;
	return xDeltaCode(true, socket, referenceFileName, referenceFile, referenceFileSize, newFileName, newFile, written, copyContext, ioStats, socketTime, socketBytes);
}

bool receiveDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* destFileName, FileTime lastWriteTime, CopyContext& copyContext, u64& written, IOStats& ioStats, RecvDeltaStats& recvStats)
{
	written = 0;

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

		if (!xDeltaCode(false, socket, referenceFileName, referenceFile, referenceFileSize, tempFileName.c_str(), tempFile, written, copyContext, ioStats, recvStats.recvTime, recvStats.recvSize))
			return false;

		if (!setFileLastWriteTime(tempFileName.c_str(), tempFile, lastWriteTime, ioStats))
			return false;
	}

	if (!moveFile(tempFileName.c_str(), destFileName, ioStats))
		return false;

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
