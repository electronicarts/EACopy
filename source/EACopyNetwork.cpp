// (c) Electronic Arts. All Rights Reserved.

#include "EACopyNetwork.h"
#include <utility>
#include <assert.h>
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <Mswsock.h>
#include <Lm.h>
#include <LmDfs.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Netapi32.lib")
#pragma comment (lib, "Mpr.lib")
#pragma comment (lib, "Mswsock.lib") // TransmitFile
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include "zstd/zstd.h"

#if defined(_WIN32)
#else
typedef void* HANDLE;
struct _OVERLAPPED { uint Offset; uint OffsetHigh; HANDLE hEvent; };
namespace eacopy {
#define WSAECONNABORTED                  10053L
#define ioctlsocket ioctl
#define closesocket close
#define ERROR_IO_PENDING                 997L    // dderror
HANDLE CreateEvent(void*, bool, bool, void*) { EACOPY_NOT_IMPLEMENTED return nullptr; }
void CloseHandle(HANDLE h) { EACOPY_NOT_IMPLEMENTED }
struct WSABUF { uint len; char* buf; };
int WSARecv(SOCKET s, WSABUF* lpBuffers, uint dwBufferCount, uint* lpNumberOfBytesRecvd, uint* lpFlags, void* lpOverlapped, void* lpCompletionRoutine) { EACOPY_NOT_IMPLEMENTED return 0; }
#define INFINITE            0xFFFFFFFF  // Infinite timeout
#define WAIT_OBJECT_0 0
uint WaitForSingleObject(HANDLE hHandle, uint dwMilliseconds) { EACOPY_NOT_IMPLEMENTED return 0; }
}
#endif

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool isLocalHost(const wchar_t* hostname)
{
	#if defined(_WIN32)
	WSADATA wsaData;
	int res = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (res != 0)
		return false;
	#endif

	struct AddrInfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; /*either IPV4 or IPV6*/
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	struct AddrInfo* hostaddr = NULL;
	struct AddrInfo* localaddr = NULL;
	int res1 = getAddrInfoW(hostname, NULL, &hints, &hostaddr);
	int res2 = getAddrInfoW(L"localhost", NULL, &hints, &localaddr);
	bool isLocal = res1 == 0 && res2 == 0 && stringEquals(hostaddr->ai_canonname, localaddr->ai_canonname);

	freeAddrInfo(hostaddr);
	freeAddrInfo(localaddr);
	
	#if defined(_WIN32)
	WSACleanup();
	#endif
	
	return isLocal;
}

const wchar_t* optimizeUncPath(const wchar_t* uncPath, WString& temp, bool allowLocal)
{
#if defined(_WIN32)
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
		if (!stringCopy(wserverName, eacopy_sizeof_array(wserverName), serverName.c_str()))
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
#endif

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
			int lastError = getLastNetworkError();
			if (lastError == WSAECONNABORTED)
				closeSocket(socket);
			logErrorf(L"send failed with error: %ls", getErrorText(lastError).c_str());
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
		int res = ::recv(socket.socket, pos, left, 0);
		if (res < 0)
		{
			int lastError = getLastNetworkError();
			if (lastError == WSAECONNABORTED)
				closeSocket(socket);
			logErrorf(L"recv failed with error: %ls", getErrorText(lastError).c_str());
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
	uint value = 1;
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

bool sendFile(Socket& socket, const wchar_t* src, size_t fileSize, WriteFileType writeType, CopyContext& copyContext, CompressionData& compressionData, bool useBufferedIO, IOStats& ioStats, SendFileStats& sendStats)
{
	WString tempBuffer1;
	const wchar_t* validSrc = convertToShortPath(src, tempBuffer1);

	FileHandle sourceFile;
	if (!openFileRead(validSrc, sourceFile, ioStats, useBufferedIO))
		return false;

	ScopeGuard closeSourceFile([&]() { closeFile(validSrc, sourceFile, AccessType_Read, ioStats); });


	if (writeType == WriteFileType_TransmitFile)
	{
		#if defined(_WIN32)
		_OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = WSACreateEvent();
		ScopeGuard closeEvent([&]() { WSACloseEvent(overlapped.hEvent); });

		u64 pos = 0;
		while (pos != fileSize)
		{
			u64 left = fileSize - pos;
			uint toWrite = (uint)std::min(left, u64(INT_MAX-1));

			u64 startSendTime = getTime();
			if (!TransmitFile(socket.socket, sourceFile, toWrite, 0, &overlapped, NULL, TF_USE_KERNEL_APC))
			{
				int error = getLastNetworkError();
				if (error == ERROR_IO_PENDING)// or WSA_IO_PENDING
				{
					if (WSAWaitForMultipleEvents(1, &overlapped.hEvent, TRUE, WSA_INFINITE, FALSE) == WSA_WAIT_FAILED)
					{
						int error = getLastNetworkError();
						logErrorf(L"Error while waiting on transmit of %ls: %ls", validSrc, getErrorText(error).c_str());
						return false;
					}
				}
				else
				{
					logErrorf(L"Error while transmitting %ls: %ls", validSrc, getErrorText(error).c_str());
					return false;
				}
			}
			sendStats.sendTime += getTime() - startSendTime;
			sendStats.sendSize += toWrite;

			pos += toWrite;
			overlapped.Offset = static_cast<uint>(pos);
			overlapped.OffsetHigh = static_cast<uint>(pos >> 32);
		}
		#else
		EACOPY_NOT_IMPLEMENTED
		return false;
		#endif
	}
	else if (writeType == WriteFileType_Send)
	{
		u64 left = fileSize;
		while (left)
		{
			uint toRead = (uint)std::min(left, u64(NetworkTransferChunkSize));
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);

			u64 read;
			if (!readFile(validSrc, sourceFile, copyContext.buffers[0], toReadAligned, read, ioStats))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %ls: %ls", validSrc, getLastErrorText().c_str());
					return false;
				}
			}

			u64 startSendTime = getTime();
			if (!sendData(socket, copyContext.buffers[0], read))
				return false;
			sendStats.sendTime += getTime() - startSendTime;
			sendStats.sendSize += read;

			left -= read;
		}
	}
	else if (writeType == WriteFileType_Compressed)
	{
		enum { CompressedNetworkTransferChunkSize = NetworkTransferChunkSize };

		u64 left = fileSize;
		while (left)
		{
			enum { CompressBoundReservation = 32 * 1024 };
			// Make sure the amount of data we've read fit in the destination compressed buffer
			static_assert(ZSTD_COMPRESSBOUND(CompressedNetworkTransferChunkSize - CompressBoundReservation) <= CompressedNetworkTransferChunkSize - 4, "");

			uint toRead = std::min(left, u64(CompressedNetworkTransferChunkSize - CompressBoundReservation));
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);
			u64 read;
			if (!readFile(validSrc, sourceFile, copyContext.buffers[0], toReadAligned, read, ioStats))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %ls: %ls", validSrc, getLastErrorText().c_str());
					return false;
				}
			}

			if (!compressionData.context)
				compressionData.context = ZSTD_createCCtx();

			// Use the first 4 bytes to write size of buffer.. can probably be replaced with zstd header instead
			u8* destBuf = copyContext.buffers[1];
			u64 startCompressTime = getTime();
			size_t compressedSize = ZSTD_compressCCtx((ZSTD_CCtx*)compressionData.context, destBuf + 4, CompressedNetworkTransferChunkSize - 4, copyContext.buffers[0], read, compressionData.level);
			if (ZSTD_isError(compressedSize))
			{
				logErrorf(L"Fail compressing file %ls: %ls", validSrc, ZSTD_getErrorName(compressedSize));
				return false;
			}

			u64 compressTime = getTime() - startCompressTime;

			*(uint*)destBuf =  uint(compressedSize);

			u64 startSendTime = getTime();
			if (!sendData(socket, destBuf, compressedSize + 4))
				return false;
			u64 sendTime = getTime() - startSendTime;

			sendStats.compressionLevelSum += read * compressionData.level;

			if (!compressionData.fixedLevel)
			{
				// This is a bit fuzzy logic. Essentially we compared how much time per byte we spent last loop (compress + send)
				// We then look at if we compressed more or less last time and if time went up or down.
				// If time went up when we increased compression, we decrease it again. If it went down we increase compression even more.

				u64 compressWeight = ((compressTime + sendTime) * 1000) / read;

				u64 lastCompressWeight = compressionData.lastWeight;
				compressionData.lastWeight = compressWeight;

				int lastCompressionLevel = compressionData.lastLevel;
				compressionData.lastLevel = compressionData.level;

				bool increaseCompression = compressWeight < lastCompressWeight && lastCompressionLevel <= compressionData.level || 
											compressWeight >= lastCompressWeight && lastCompressionLevel > compressionData.level;

				if (increaseCompression)
					compressionData.level = std::min(22, compressionData.level + 1);
				else
					compressionData.level = std::max(1, compressionData.level - 1);
			}

			sendStats.compressTime += compressTime;
			sendStats.sendTime += sendTime;
			sendStats.sendSize += compressedSize + 4;

			left -= read;
		}
	}

	return true;
}

NetworkCopyContext::~NetworkCopyContext()
{
	ZSTD_freeDCtx((ZSTD_DCtx*)compContext);
}

bool receiveFile(bool& outSuccess, Socket& socket, const wchar_t* fullPath, size_t fileSize, FileTime lastWriteTime, WriteFileType writeType, bool useBufferedIO, NetworkCopyContext& copyContext, char* recvBuffer, uint recvPos, uint& commandSize, IOStats& ioStats, RecvFileStats& recvStats)
{
	WString tempBuffer1;
	const wchar_t* validFullPath = convertToShortPath(fullPath, tempBuffer1);

	u64 totalReceivedSize = 0;

	if (writeType == WriteFileType_TransmitFile || writeType == WriteFileType_Send)
	{
		FileHandle file;
		_OVERLAPPED osWrite;
		memset(&osWrite, 0, sizeof(osWrite));
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;
		osWrite.hEvent = CreateEvent(nullptr, false, true, nullptr);

		outSuccess = openFileWrite(validFullPath, file, ioStats, useBufferedIO);
		ScopeGuard fileGuard([&]() { CloseHandle(osWrite.hEvent); if (!closeFile(validFullPath, file, AccessType_Write, ioStats)) outSuccess = false; });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 toCopy = std::min(u64(recvPos - commandSize), u64(fileSize));
			outSuccess = outSuccess & writeFile(validFullPath, file, recvBuffer + commandSize, toCopy, ioStats, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			u64 startRecvTime = getTime();
			u64 left = fileSize - read;
			uint toRead = (uint)std::min(left, u64(NetworkTransferChunkSize));
			WSABUF wsabuf;
			wsabuf.len = toRead;
			wsabuf.buf = (char*)copyContext.buffers[fileBufIndex];
			uint recvBytes = 0;
			uint flags = 0;
			int fileRes = WSARecv(socket.socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
			if (fileRes != 0)
			{
				logErrorf(L"recv failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
				return false;
			}
			if (recvBytes == 0)
			{
				logErrorf(L"Socket closed before full file has been received (%ls)", fullPath);
				return false;
			}

			outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
			recvStats.recvTime += getTime() - startRecvTime;
			recvStats.recvSize += recvBytes;

			outSuccess = outSuccess && writeFile(validFullPath, file, copyContext.buffers[fileBufIndex], recvBytes, ioStats, &osWrite);

			read += recvBytes;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		totalReceivedSize += read;

		u64 startWriteTime = getTime();
		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		ioStats.writeTime = getTime() - startWriteTime;
		outSuccess = outSuccess && setFileLastWriteTime(validFullPath, file, lastWriteTime, ioStats);
	}
	else if (writeType == WriteFileType_Compressed)
	{
		FileHandle file;
		_OVERLAPPED osWrite;
		memset(&osWrite, 0, sizeof(osWrite));
		osWrite.Offset = 0xFFFFFFFF;
		osWrite.OffsetHigh = 0xFFFFFFFF;
		osWrite.hEvent = CreateEvent(nullptr, false, true, nullptr);

		outSuccess = openFileWrite(validFullPath, file, ioStats, useBufferedIO);
		ScopeGuard fileGuard([&]() { CloseHandle(osWrite.hEvent); if (!closeFile(validFullPath, file, AccessType_Write, ioStats)) outSuccess = false; });

		u64 read = 0;

		// Copy the stuff already in the buffer
		if (recvPos > commandSize)
		{
			u64 toCopy = std::min(u64(recvPos - commandSize), u64(fileSize));
			outSuccess = outSuccess & writeFile(validFullPath, file, recvBuffer + commandSize, toCopy, ioStats, &osWrite);
			read = toCopy;
			commandSize += (uint)toCopy;
		}

		int fileBufIndex = 0;

		while (read != fileSize)
		{
			u64 startRecvTime = getTime();

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

			while (toRead > 0)
			{
				WSABUF wsabuf;
				wsabuf.len = toRead;
				wsabuf.buf = (char*)copyContext.buffers[2];
				uint recvBytes = 0;
				uint flags = 0;
				int fileRes = WSARecv(socket.socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
				if (fileRes != 0)
				{
					logErrorf(L"recv failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
					return false;
				}
				if (recvBytes == 0)
				{
					logErrorf(L"Socket closed before full file has been received (%ls)", validFullPath);
					return false;
				}

				recvStats.recvTime += getTime() - startRecvTime;
				recvStats.recvSize += recvBytes;
				toRead -= recvBytes;
			}

			if (!copyContext.compContext)
				copyContext.compContext = ZSTD_createDCtx();

			u64 startDecompressTime = getTime();
			size_t decompressedSize = ZSTD_decompressDCtx((ZSTD_DCtx*)copyContext.compContext, copyContext.buffers[fileBufIndex], NetworkTransferChunkSize, copyContext.buffers[2], compressedSize);
			if (outSuccess)
			{
				outSuccess &= ZSTD_isError(decompressedSize) == 0;
				if (!outSuccess)
				{
					logErrorf(L"Decompression error while decompressing %u bytes after reading %llu, for file %ls: %hs", compressedSize, read, validFullPath, ZSTD_getErrorName(decompressedSize));
					// Don't return false since we can still continue copying other files after getting this error
				}
			}

			recvStats.decompressTime = getTime() - startDecompressTime;

			outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;

			outSuccess = outSuccess && writeFile(validFullPath, file, copyContext.buffers[fileBufIndex], decompressedSize, ioStats, &osWrite);

			read += decompressedSize;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}

		u64 startWriteTime = getTime();
		outSuccess = outSuccess && WaitForSingleObject(osWrite.hEvent, INFINITE) == WAIT_OBJECT_0;
		ioStats.writeTime = getTime() - startWriteTime;
		outSuccess = outSuccess && setFileLastWriteTime(validFullPath, file, lastWriteTime, ioStats);
	}

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy

int getAddrInfoW(const wchar_t* name, const wchar_t* service, const AddrInfo* hints, AddrInfo** result)
{
	#if defined(_WIN32)
	return GetAddrInfoW(name, service, hints, result);
	#else
	EACOPY_NOT_IMPLEMENTED
	return 0;
	#endif
}

int getLastNetworkError()
{
	#if defined(_WIN32)
	return WSAGetLastError();
	#else
	EACOPY_NOT_IMPLEMENTED
	return 0;
	#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
