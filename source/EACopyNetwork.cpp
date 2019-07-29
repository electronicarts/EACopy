// (c) Electronic Arts. All Rights Reserved.

#include "EACopyNetwork.h"
#include <Mswsock.h>
#include <Lm.h>
#include <LmDfs.h>
#include <ws2tcpip.h>
#include "zstd/zstd.h"
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Netapi32.lib")
#pragma comment (lib, "Mpr.lib")
#pragma comment (lib, "Mswsock.lib") // TransmitFile

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool isLocalHost(const wchar_t* hostname)
{
	WSADATA wsaData;
	int res = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (res != 0)
		return false;

	struct addrinfoW hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; /*either IPV4 or IPV6*/
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	struct addrinfoW* hostaddr = NULL;
	struct addrinfoW* localaddr = NULL;
	int res1 = GetAddrInfoW(hostname, NULL, &hints, &hostaddr);
	int res2 = GetAddrInfoW(L"localhost", NULL, &hints, &localaddr);
	bool isLocal = res1 == 0 && res2 == 0 && wcscmp(hostaddr->ai_canonname, localaddr->ai_canonname) == 0;

	FreeAddrInfoW(hostaddr);
	FreeAddrInfoW(localaddr);
	WSACleanup();
	
	return isLocal;
}

const wchar_t* optimizeUncPath(const wchar_t* uncPath, WString& temp, bool allowLocal)
{
	if (uncPath[0] != '\\' || uncPath[1] != '\\')
		return uncPath;

	// Try to resolve Dfs to real server path
	{
		WString uncPathStr(uncPath);
		DFS_INFO_3* info;
		int result = NetDfsGetClientInfo((LPWSTR)uncPathStr.c_str(), NULL, NULL, 3, (LPBYTE*)&info);
		if (result == NERR_Success)
		{
			const wchar_t* localPath = uncPath + wcslen(info->EntryPath) + 2; // +1 for single slash in beginning of EntryPath plus the slash after shareName
			wchar_t* serverName = info->Storage->ServerName;
			wchar_t* shareName = info->Storage->ShareName;

			bool hasLocalPath = *localPath != '\0';

			temp = L"\\\\" + WString(serverName, serverName + wcslen(serverName)) + L'\\';
			temp += WString(shareName, shareName + wcslen(shareName));
			if (hasLocalPath)
			{
				temp += L"\\";
				temp += localPath;
			}
			NetApiBufferFree(info);

			uncPath = temp.c_str();
		}
	}

	// Try SMB to see if path is local
	if (allowLocal)
	{
		const wchar_t* serverNameStart = uncPath + 2;
		const wchar_t* serverNameEnd = wcschr(serverNameStart, '\\');
		if (!serverNameEnd)
			return uncPath;

		WString serverName(serverNameStart, serverNameEnd);
		if (!isLocalHost(serverName.c_str()))
			return uncPath;

		const wchar_t* netDirectory = serverNameEnd + 1;

		const wchar_t* localPath = wcschr(netDirectory, '\\');
		if (!localPath)
			localPath = netDirectory + wcslen(netDirectory);

		wchar_t wnetName[MAX_PATH] = { 0 };
		if (wcsncpy_s(wnetName, eacopy_sizeof_array(wnetName), netDirectory, size_t(localPath - netDirectory)))
			return uncPath;

		wchar_t wserverName[MAX_PATH] = { 0 };
		if (wcscpy_s(wserverName, eacopy_sizeof_array(wserverName), serverName.c_str()))
			return uncPath;

		PSHARE_INFO_502 shareInfo;

		NET_API_STATUS res = NetShareGetInfo(wserverName, wnetName, 502, (LPBYTE*) &shareInfo);
		if (res != NERR_Success)
			return uncPath;

		WString wpath(shareInfo->shi502_path);
		NetApiBufferFree(shareInfo);

		temp = WString(wpath.begin(), wpath.end());
		if (*localPath)
			temp += localPath;
		uncPath = temp.c_str();
	}

	return uncPath;
}

bool sendData(SOCKET socket, const void* buffer, uint size)
{
	uint left = size;
	char* pos = (char*)buffer;

	while (left)
	{
		int res = ::send(socket, (const char*)pos, left, 0);
		if (res == SOCKET_ERROR)
		{
			logErrorf(L"send failed with error: %d", WSAGetLastError());
			return false;
		}
		pos += res;
		left -= res;
	}
	return true;
}

bool receiveData(SOCKET socket, void* buffer, uint size)
{
	uint left = size;
	char* pos = (char*)buffer;

	while (left)
	{
		int res = ::recv(socket, pos, left, MSG_WAITALL);
		if (res < 0)
		{
			logErrorf(L"recv failed with error: %d", WSAGetLastError());
			return false;
		}
		else if (res == 0)
		{
			logDebugLinef(L"Connection closed");
			return false;
		}

		pos += res;
		left -=res;
	}
	return true;
}

bool setBlocking(SOCKET socket, bool blocking)
{
	u_long value = blocking ? 0 : 1;
	if (ioctlsocket(socket, FIONBIO, &value) != SOCKET_ERROR)
		return true;

	logErrorf(L"Setting non blocking failed");
	return false;
}

bool disableNagle(SOCKET socket)
{
	DWORD value = 1;
	if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&value, sizeof(value)) != SOCKET_ERROR)
		return true;

	logErrorf(L"setsockopt TCP_NODELAY error");
	return false;
}

bool setSendBufferSize(SOCKET socket, uint sendBufferSize)
{
	if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&sendBufferSize, sizeof(sendBufferSize)) != SOCKET_ERROR)
		return true;

	logDebugf(L"setsockopt SO_SNDBUF error");
	return false;
}

bool setRecvBufferSize(SOCKET socket, uint recvBufferSize)
{
	if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBufferSize, sizeof(recvBufferSize)) != SOCKET_ERROR)
		return true;

	logDebugf(L"setsockopt SO_RCVBUF error");
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CompressionData::~CompressionData()
{
	if (context)
		ZSTD_freeCCtx((ZSTD_CCtx*)context);
}

bool sendFile(SOCKET socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, CopyBuffer& copyBuffer, CompressionData& compressionData, bool useBufferedIO, CopyStats& copyStats, SendFileStats& sendStats)
{
	BOOL success = TRUE;
	u64 startCreateReadTimeMs = getTimeMs();
	HANDLE sourceFile;
	if (!openFileRead(src, sourceFile, useBufferedIO))
		return false;

	ScopeGuard closeFile([&]() { CloseHandle(sourceFile); });
	copyStats.createReadTimeMs += getTimeMs() - startCreateReadTimeMs;


	if (writeType == WriteFileType_TransmitFile)
	{
		_OVERLAPPED overlapped;
		ZeroMemory(&overlapped, sizeof(overlapped));
		overlapped.hEvent = WSACreateEvent();
		ScopeGuard closeEvent([&]() { WSACloseEvent(overlapped.hEvent); });

		u64 pos = 0;
		while (pos != fileSize)
		{
			u64 left = fileSize - pos;
			uint toWrite = (uint)min(left, u64(INT_MAX-1));

			u64 startSendMs = getTimeMs();
			if (!TransmitFile(socket, sourceFile, toWrite, 0, &overlapped, NULL, TF_USE_KERNEL_APC))
			{
				int error = WSAGetLastError();
				if (error == ERROR_IO_PENDING)// or WSA_IO_PENDING
				{
					if (WSAWaitForMultipleEvents(1, &overlapped.hEvent, TRUE, WSA_INFINITE, FALSE) == WSA_WAIT_FAILED)
					{
						int error = WSAGetLastError();
						logErrorf(L"Error while waiting on transmit of %s: %s", src, getErrorText(error).c_str());
						return false;
					}
				}
				else
				{
					logErrorf(L"Error while transmitting %s: %s", src, getErrorText(error).c_str());
					return false;
				}
			}
			sendStats.sendTimeMs += getTimeMs() - startSendMs;
			sendStats.sendSize += toWrite;

			pos += toWrite;
			overlapped.Offset = static_cast<DWORD>(pos);
			overlapped.OffsetHigh = static_cast<DWORD>(pos >> 32);
		}
	}
	else if (writeType == WriteFileType_Send)
	{
		u64 left = fileSize;
		while (left)
		{
			u64 startReadMs = getTimeMs();
			DWORD toRead = (DWORD)min(left, CopyBufferSize);
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

			if (!ReadFile(sourceFile, copyBuffer.buffers[0], toReadAligned, NULL, NULL))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %s: %s", src, getLastErrorText().c_str());
					return false;
				}
			}
			copyStats.readTimeMs += getTimeMs() - startReadMs;

			u64 startSendMs = getTimeMs();
			if (!sendData(socket, copyBuffer.buffers[0], toRead))
				return false;
			sendStats.sendTimeMs += getTimeMs() - startSendMs;
			sendStats.sendSize += toRead;

			left -= toRead;
		}
	}
	else if (writeType == WriteFileType_Compressed)
	{
		u64 left = fileSize;
		while (left)
		{
			u64 startReadMs = getTimeMs();
			DWORD toRead = (DWORD)min(left, CopyBufferSize);
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

			if (!ReadFile(sourceFile, copyBuffer.buffers[0], toReadAligned, NULL, NULL))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %s: %s", src, getLastErrorText().c_str());
					return false;
				}
			}
			copyStats.readTimeMs += getTimeMs() - startReadMs;

			if (!compressionData.context)
				compressionData.context = ZSTD_createCCtx();

			// Use the first 4 bytes to write size of buffer.. can probably be replaced with zstd header instead
			u8* destBuf = copyBuffer.buffers[1];
			u64 startCompressMs = getTimeMs();
			uint compressedSize = (uint)ZSTD_compressCCtx((ZSTD_CCtx*)compressionData.context, destBuf + 4, CopyBufferSize - 4, copyBuffer.buffers[0], toRead, compressionData.level);
			u64 compressTimeMs = getTimeMs() - startCompressMs;

			*(uint*)destBuf =  compressedSize;

			u64 startSendMs = getTimeMs();
			if (!sendData(socket, destBuf, compressedSize + 4))
				return false;
			u64 sendTimeMs = getTimeMs() - startSendMs;

			sendStats.compressionLevelSum += toRead * compressionData.level;

			if (!compressionData.fixedLevel)
			{
				// This is a bit fuzzy logic. Essentially we compared how much time per byte we spent last loop (compress + send)
				// We then look at if we compressed more or less last time and if time went up or down.
				// If time went up when we increased compression, we decrease it again. If it went down we increase compression even more.

				u64 compressWeight = ((compressTimeMs + sendTimeMs) * 10000000) / toRead;

				u64 lastCompressWeight = compressionData.lastWeight;
				compressionData.lastWeight = compressWeight;

				int lastCompressionLevel = compressionData.lastLevel;
				compressionData.lastLevel = compressionData.level;

				bool increaseCompression = compressWeight < lastCompressWeight && lastCompressionLevel <= compressionData.level || 
											compressWeight >= lastCompressWeight && lastCompressionLevel > compressionData.level;

				if (increaseCompression)
					compressionData.level = min(22, compressionData.level + 1);
				else
					compressionData.level = max(1, compressionData.level - 1);
			}

			sendStats.compressTimeMs += compressTimeMs;
			sendStats.sendTimeMs += sendTimeMs;
			sendStats.sendSize += compressedSize + 4;

			left -= toRead;
		}
	}

	return true;
}

FileReceiveBuffers::FileReceiveBuffers()
{
	data[0] = new char[capacity];
	data[1] = new char[capacity];
	compData = new char[compCapacity];
}

FileReceiveBuffers::~FileReceiveBuffers()
{
	ZSTD_freeDCtx((ZSTD_DCtx*)compContext);
	delete[] compData;
	delete[] data[0];
	delete[] data[1];
}

bool receiveFile(bool& outSuccess, SOCKET socket, const wchar_t* fullPath, size_t fileSize, FILETIME lastWriteTime, WriteFileType writeType, bool useBufferedIO, FileReceiveBuffers& fileBuf, char* recvBuffer, uint recvPos, uint& commandSize)
{
	u64 totalReceivedSize = 0;

	if (writeType == WriteFileType_TransmitFile || writeType == WriteFileType_Send)
	{
		HANDLE file;
		OVERLAPPED osWrite      = { 0, 0 };
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;

		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
		outSuccess = openFileWrite(fullPath, file, useBufferedIO);
		ScopeGuard fileGuard([&]() { closeFile(fullPath, file); CloseHandle(osWrite.hEvent); });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 toCopy = min(u64(recvPos - commandSize), fileSize);
			outSuccess = outSuccess & writeFile(fullPath, file, recvBuffer + commandSize, toCopy, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			u64 left = fileSize - read;
			uint toRead = (uint)min(left, fileBuf.capacity);
			WSABUF wsabuf;
			wsabuf.len = toRead;
			wsabuf.buf = fileBuf.data[fileBufIndex];
			DWORD recvBytes = 0;
			DWORD flags = MSG_WAITALL;
			int fileRes = WSARecv(socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
			if (fileRes != 0)
			{
				logErrorf(L"recv failed with error: %s", getErrorText(WSAGetLastError()).c_str());
				return false;
			}
			if (recvBytes == 0)
			{
				logErrorf(L"Socket closed before full file has been received (%s)", fullPath);
				return false;
			}

			outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;

			outSuccess = outSuccess && writeFile(fullPath, file, fileBuf.data[fileBufIndex], recvBytes, &osWrite);

			read += recvBytes;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		totalReceivedSize += read;

		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		outSuccess = outSuccess && setFileLastWriteTime(fullPath, file, lastWriteTime);
		outSuccess = outSuccess && closeFile(fullPath, file);
	}
	else if (writeType == WriteFileType_Compressed)
	{
		HANDLE file;
		OVERLAPPED osWrite      = { 0, 0 };
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;

		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
		outSuccess = openFileWrite(fullPath, file, useBufferedIO);
		ScopeGuard fileGuard([&]() { closeFile(fullPath, file); CloseHandle(osWrite.hEvent); });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 toCopy = min(u64(recvPos - commandSize), fileSize);
			outSuccess = outSuccess & writeFile(fullPath, file, recvBuffer + commandSize, toCopy, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			uint compressedSize;
			if (!receiveData(socket, &compressedSize, sizeof(uint)))
				return false;

			totalReceivedSize += compressedSize + sizeof(uint);

			if (compressedSize > fileBuf.compCapacity)
			{
				logErrorf(L"Compressed size is bigger than compression buffer capacity");
				return false;
			}

			uint toRead = compressedSize;

			WSABUF wsabuf;
			wsabuf.len = toRead;
			wsabuf.buf = fileBuf.compData;
			DWORD recvBytes = 0;
			DWORD flags = MSG_WAITALL;
			int fileRes = WSARecv(socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
			if (fileRes != 0)
			{
				logErrorf(L"recv failed with error: %s", getErrorText(WSAGetLastError()).c_str());
				return false;
			}
			if (recvBytes == 0)
			{
				logErrorf(L"Socket closed before full file has been received (%s)", fullPath);
				return false;
			}

			if (!fileBuf.compContext)
				fileBuf.compContext = ZSTD_createDCtx();


			size_t decompressedSize = ZSTD_decompressDCtx((ZSTD_DCtx*)fileBuf.compContext, fileBuf.data[fileBufIndex], fileBuf.capacity, fileBuf.compData, recvBytes);
			outSuccess = outSuccess && !ZSTD_isError(decompressedSize);


			outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
			outSuccess = outSuccess && writeFile(fullPath, file, fileBuf.data[fileBufIndex], decompressedSize, &osWrite);

			read += decompressedSize;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		outSuccess = outSuccess && setFileLastWriteTime(fullPath, file, lastWriteTime);
		outSuccess = outSuccess && closeFile(fullPath, file);
	}

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
