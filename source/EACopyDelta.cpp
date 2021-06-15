// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY)

#include <EACopyDelta.h>
#include "EACopyDeltaZstd.h"
#include "EACopyDeltaXDelta.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using CodecFunc = bool(bool encode, Socket& socket, const wchar_t* referenceFileName, FileHandle referenceFile, u64 referenceFileSize, const wchar_t* newFileName, FileHandle newFile, u64 newFileSize, NetworkCopyContext& copyContext, IOStats& ioStats, u64& socketTime, u64& socketSize, u64& codeTime);


CodecFunc* g_codecFuncs[] =
{
	zstdCode,
	xDeltaCode,
};

bool
sendDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* newFileName, u64 newFileSize, NetworkCopyContext& copyContext, IOStats& ioStats)
{
	FileHandle newFile;
	if (!openFileRead(newFileName, newFile, ioStats, true))
		return false;
	ScopeGuard _([&]() { closeFile(newFileName, newFile, AccessType_Read, ioStats); });

	FileHandle referenceFile;
	if (!openFileRead(referenceFileName, referenceFile, ioStats, true, nullptr, false))
		return false;
	ScopeGuard _2([&]() { closeFile(referenceFileName, referenceFile, AccessType_Read, ioStats); });


	u8 codecIndex = 0;
	if (!sendData(socket, &codecIndex, sizeof(codecIndex)))
		return false;

	CodecFunc* codecFunc = g_codecFuncs[codecIndex];

	u64 socketTime = 0;
	u64 socketBytes = 0;
	u64 codeTime = 0;
	return codecFunc(true, socket, referenceFileName, referenceFile, referenceFileSize, newFileName, newFile, newFileSize, copyContext, ioStats, socketTime, socketBytes, codeTime);
}

bool receiveDelta(Socket& socket, const wchar_t* referenceFileName, u64 referenceFileSize, const wchar_t* destFileName, u64 destFileSize, FileTime lastWriteTime, NetworkCopyContext& copyContext, IOStats& ioStats, RecvDeltaStats& recvStats)
{
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

	FileHandle referenceFile;
	if (!openFileRead(referenceFileName, referenceFile, ioStats, true, nullptr, false))
		return false;
	ScopeGuard closeRef([&]() { closeFile(referenceFileName, referenceFile, AccessType_Read, ioStats); });

	FileHandle tempFile;
	if (!openFileWrite(tempFileName.c_str(), tempFile, ioStats, true, nullptr, true)) // Create file hidden, and unhide it once we've moved it in place
		return false;
	ScopeGuard delTemp([&]() { deleteFile(tempFileName.c_str(), ioStats, false); });
	ScopeGuard closeTemp([&]() { closeFile(tempFileName.c_str(), tempFile, AccessType_Write, ioStats); });

	u8 codecIndex;
	if (!receiveData(socket, &codecIndex, sizeof(codecIndex)))
		return false;
	CodecFunc* codecFunc = g_codecFuncs[codecIndex];

	if (!codecFunc(false, socket, referenceFileName, referenceFile, referenceFileSize, tempFileName.c_str(), tempFile, destFileSize, copyContext, ioStats, recvStats.recvTime, recvStats.recvSize, recvStats.decompressTime))
		return false;

	if (!setFileLastWriteTime(tempFileName.c_str(), tempFile, lastWriteTime, ioStats))
		return false;

	closeRef.execute();
	closeTemp.execute();

	if (!moveFile(tempFileName.c_str(), destFileName, ioStats))
		return false;

	if (!setFileHidden(destFileName, false))
		return false;

	delTemp.cancel();

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
