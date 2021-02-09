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
	int retryCount = 0;
	while (retryCount++ < 3)
	{
		WString uncPathStr(uncPath);
		DFS_INFO_3* info;
		int result = NetDfsGetClientInfo((LPWSTR)uncPathStr.c_str(), NULL, NULL, 3, (LPBYTE*)&info);
		if (result != NERR_Success)
			break;
		ScopeGuard freeInfo([&]() { NetApiBufferFree(info); });

		if (info->NumberOfStorages == 0)
			break;

		const wchar_t* localPath = uncPathStr.c_str() + wcslen(info->EntryPath) + 2; // +1 for single slash in beginning of EntryPath plus the slash after shareName
		wchar_t* serverName = info->Storage->ServerName;
		wchar_t* shareName = info->Storage->ShareName;

		bool hasLocalPath = *localPath != '\0';

		WString newPath = L"\\\\" + WString(serverName, serverName + wcslen(serverName)) + L'\\';
		newPath += WString(shareName, shareName + wcslen(shareName));
		if (hasLocalPath)
		{
			newPath += L"\\";
			newPath += localPath;
		}
		if (newPath == uncPath)
			break;
		temp = std::move(newPath);
		uncPath = temp.c_str();
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

		wchar_t wnetName[MaxPath] = { 0 };
		if (wcsncpy_s(wnetName, eacopy_sizeof_array(wnetName), netDirectory, size_t(localPath - netDirectory)))
			return uncPath;

		wchar_t wserverName[MaxPath] = { 0 };
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

bool sendData(Socket& socket, const void* buffer, uint size)
{
	uint left = size;
	char* pos = (char*)buffer;

	while (left)
	{
		int res = ::send(socket.socket, (const char*)pos, left, 0);
		if (res == SOCKET_ERROR)
		{
			int lastError = WSAGetLastError();
			if (lastError == WSAECONNABORTED)
				closeSocket(socket);
			logErrorf(L"send failed with error: %s", getErrorText(lastError).c_str());
			return false;
		}
		pos += res;
		left -= res;
	}
	return true;
}

bool receiveData(Socket& socket, void* buffer, uint size)
{
	uint left = size;
	char* pos = (char*)buffer;

	while (left)
	{
		int res = ::recv(socket.socket, pos, left, MSG_WAITALL);
		if (res < 0)
		{
			int lastError = WSAGetLastError();
			if (lastError == WSAECONNABORTED)
				closeSocket(socket);
			logErrorf(L"recv failed with error: %s", getErrorText(lastError).c_str());
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

bool setBlocking(Socket& socket, bool blocking)
{
	u_long value = blocking ? 0 : 1;
	if (ioctlsocket(socket.socket, FIONBIO, &value) != SOCKET_ERROR)
		return true;

	logErrorf(L"Setting non blocking failed");
	return false;
}

bool disableNagle(Socket& socket)
{
	DWORD value = 1;
	if (setsockopt(socket.socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&value, sizeof(value)) != SOCKET_ERROR)
		return true;

	logErrorf(L"setsockopt TCP_NODELAY error");
	return false;
}

bool setSendBufferSize(Socket& socket, uint sendBufferSize)
{
	if (setsockopt(socket.socket, SOL_SOCKET, SO_SNDBUF, (const char*)&sendBufferSize, sizeof(sendBufferSize)) != SOCKET_ERROR)
		return true;

	logDebugf(L"setsockopt SO_SNDBUF error");
	return false;
}

bool setRecvBufferSize(Socket& socket, uint recvBufferSize)
{
	if (setsockopt(socket.socket, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBufferSize, sizeof(recvBufferSize)) != SOCKET_ERROR)
		return true;

	logDebugf(L"setsockopt SO_RCVBUF error");
	return false;
}

void closeSocket(Socket& socket)
{
	closesocket(socket.socket);
	socket.socket = INVALID_SOCKET;
}

bool isValidSocket(Socket& socket)
{
	return socket.socket != INVALID_SOCKET;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CompressionData::~CompressionData()
{
	if (context)
		ZSTD_freeCCtx((ZSTD_CCtx*)context);
}

bool sendFile(Socket& socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, CopyContext& copyContext, CompressionData& compressionData, bool useBufferedIO, CopyStats& copyStats, SendFileStats& sendStats)
{
	WString tempBuffer1;
	const wchar_t* validSrc = convertToShortPath(src, tempBuffer1);

	BOOL success = TRUE;
	u64 startCreateReadTimeMs = getTimeMs();
	HANDLE sourceFile;
	if (!openFileRead(validSrc, sourceFile, useBufferedIO))
		return false;

	ScopeGuard closeSourceFile([&]() { closeFile(validSrc, sourceFile); });
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
			if (!TransmitFile(socket.socket, sourceFile, toWrite, 0, &overlapped, NULL, TF_USE_KERNEL_APC))
			{
				int error = WSAGetLastError();
				if (error == ERROR_IO_PENDING)// or WSA_IO_PENDING
				{
					if (WSAWaitForMultipleEvents(1, &overlapped.hEvent, TRUE, WSA_INFINITE, FALSE) == WSA_WAIT_FAILED)
					{
						int error = WSAGetLastError();
						logErrorf(L"Error while waiting on transmit of %s: %s", validSrc, getErrorText(error).c_str());
						return false;
					}
				}
				else
				{
					logErrorf(L"Error while transmitting %s: %s", validSrc, getErrorText(error).c_str());
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
			DWORD toRead = (DWORD)min(left, NetworkTransferChunkSize);
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

			if (!ReadFile(sourceFile, copyContext.buffers[0], toReadAligned, NULL, NULL))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %s: %s", validSrc, getLastErrorText().c_str());
					return false;
				}
			}
			copyStats.readTimeMs += getTimeMs() - startReadMs;

			u64 startSendMs = getTimeMs();
			if (!sendData(socket, copyContext.buffers[0], toRead))
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
			enum { CompressBoundReservation = 32 * 1024 };
			// Make sure the amount of data we've read fit in the destination compressed buffer
			static_assert(ZSTD_COMPRESSBOUND(NetworkTransferChunkSize - CompressBoundReservation) <= NetworkTransferChunkSize - 4, "");

			u64 startReadMs = getTimeMs();
			DWORD toRead = (DWORD)min(left, NetworkTransferChunkSize - CompressBoundReservation);
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

			if (!ReadFile(sourceFile, copyContext.buffers[0], toReadAligned, NULL, NULL))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %s: %s", validSrc, getLastErrorText().c_str());
					return false;
				}
			}
			copyStats.readTimeMs += getTimeMs() - startReadMs;

			if (!compressionData.context)
				compressionData.context = ZSTD_createCCtx();

			// Use the first 4 bytes to write size of buffer.. can probably be replaced with zstd header instead
			u8* destBuf = copyContext.buffers[1];
			u64 startCompressMs = getTimeMs();
			size_t compressedSize = ZSTD_compressCCtx((ZSTD_CCtx*)compressionData.context, destBuf + 4, NetworkTransferChunkSize - 4, copyContext.buffers[0], toRead, compressionData.level);
			if (ZSTD_isError(compressedSize))
			{
				logErrorf(L"Fail compressing file %s: %s", validSrc, ZSTD_getErrorName(compressedSize));
				return false;
			}

			u64 compressTimeMs = getTimeMs() - startCompressMs;

			*(uint*)destBuf =  uint(compressedSize);

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

NetworkCopyContext::~NetworkCopyContext()
{
	ZSTD_freeDCtx((ZSTD_DCtx*)compContext);
}

bool receiveFile(bool& outSuccess, Socket& socket, const wchar_t* fullPath, size_t fileSize, FILETIME lastWriteTime, WriteFileType writeType, bool useBufferedIO, NetworkCopyContext& copyContext, char* recvBuffer, uint recvPos, uint& commandSize, CopyStats& copyStats, RecvFileStats& recvStats)
{
	WString tempBuffer1;
	const wchar_t* validFullPath = convertToShortPath(fullPath, tempBuffer1);

	u64 totalReceivedSize = 0;

	if (writeType == WriteFileType_TransmitFile || writeType == WriteFileType_Send)
	{
		HANDLE file;
		OVERLAPPED osWrite      = { 0, 0 };
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;

		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
		//copyStats.createWriteTimeMs 
		u64 startCreateWriteTimeMs = getTimeMs();
		outSuccess = openFileWrite(validFullPath, file, useBufferedIO);
		copyStats.createWriteTimeMs = getTimeMs() - startCreateWriteTimeMs;
		ScopeGuard fileGuard([&]() { closeFile(validFullPath, file); CloseHandle(osWrite.hEvent); });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 startWriteTimeMs = getTimeMs();
			u64 toCopy = min(u64(recvPos - commandSize), fileSize);
			outSuccess = outSuccess & writeFile(validFullPath, file, recvBuffer + commandSize, toCopy, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
			copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			u64 startRecvTimeMs = getTimeMs();
			u64 left = fileSize - read;
			uint toRead = (uint)min(left, NetworkTransferChunkSize);
			WSABUF wsabuf;
			wsabuf.len = toRead;
			wsabuf.buf = (char*)copyContext.buffers[fileBufIndex];
			DWORD recvBytes = 0;
			DWORD flags = MSG_WAITALL;
			int fileRes = WSARecv(socket.socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
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
			recvStats.recvTimeMs += getTimeMs() - startRecvTimeMs;
			recvStats.recvSize += recvBytes;

			u64 startWriteTimeMs = getTimeMs();
			outSuccess = outSuccess && writeFile(validFullPath, file, copyContext.buffers[fileBufIndex], recvBytes, &osWrite);
			copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;

			read += recvBytes;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		totalReceivedSize += read;

		u64 startWriteTimeMs = getTimeMs();
		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		outSuccess = outSuccess && setFileLastWriteTime(validFullPath, file, lastWriteTime);
		outSuccess = outSuccess && closeFile(validFullPath, file);
		copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;
	}
	else if (writeType == WriteFileType_Compressed)
	{
		HANDLE file;
		OVERLAPPED osWrite      = { 0, 0 };
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;

		osWrite.hEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
		outSuccess = openFileWrite(validFullPath, file, useBufferedIO);
		ScopeGuard fileGuard([&]() { closeFile(validFullPath, file); CloseHandle(osWrite.hEvent); });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 startWriteTimeMs = getTimeMs();
			u64 toCopy = min(u64(recvPos - commandSize), fileSize);
			outSuccess = outSuccess & writeFile(validFullPath, file, recvBuffer + commandSize, toCopy, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
			copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			u64 startRecvTimeMs = getTimeMs();

			uint compressedSize;
			if (!receiveData(socket, &compressedSize, sizeof(uint)))
				return false;

			totalReceivedSize += compressedSize + sizeof(uint);

			if (compressedSize > NetworkTransferChunkSize)
			{
				logErrorf(L"Compressed size is bigger than compression buffer capacity");
				return false;
			}

			uint toRead = compressedSize;

			WSABUF wsabuf;
			wsabuf.len = toRead;
			wsabuf.buf = (char*)copyContext.buffers[2];
			DWORD recvBytes = 0;
			DWORD flags = MSG_WAITALL;
			int fileRes = WSARecv(socket.socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
			if (fileRes != 0)
			{
				logErrorf(L"recv failed with error: %s", getErrorText(WSAGetLastError()).c_str());
				return false;
			}
			if (recvBytes == 0)
			{
				logErrorf(L"Socket closed before full file has been received (%s)", validFullPath);
				return false;
			}

			recvStats.recvTimeMs += getTimeMs() - startRecvTimeMs;
			recvStats.recvSize += recvBytes;

			if (!copyContext.compContext)
				copyContext.compContext = ZSTD_createDCtx();

			u64 startDecompressTimeMs = getTimeMs();
			size_t decompressedSize = ZSTD_decompressDCtx((ZSTD_DCtx*)copyContext.compContext, copyContext.buffers[fileBufIndex], NetworkTransferChunkSize, copyContext.buffers[2], recvBytes);
			if (outSuccess)
			{
				outSuccess &= ZSTD_isError(decompressedSize) == 0;
				if (!outSuccess)
				{
					logErrorf(L"Decompression error: %s", ZSTD_getErrorName(decompressedSize));
					// Don't return false since we can still continue copying other files after getting this error
				}
			}

			recvStats.decompressTimeMs = getTimeMs() - startDecompressTimeMs;

			outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;

			u64 startWriteTimeMs = getTimeMs();
			outSuccess = outSuccess && writeFile(validFullPath, file, copyContext.buffers[fileBufIndex], decompressedSize, &osWrite);
			copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;

			read += decompressedSize;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		u64 startWriteTimeMs = getTimeMs();
		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		outSuccess = outSuccess && setFileLastWriteTime(validFullPath, file, lastWriteTime);
		outSuccess = outSuccess && closeFile(validFullPath, file);
		copyStats.writeTimeMs = getTimeMs() - startWriteTimeMs;
	}

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
