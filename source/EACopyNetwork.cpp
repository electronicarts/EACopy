// (c) Electronic Arts. All Rights Reserved.

#include "EACopyNetwork.h"
#include <ws2tcpip.h>
#include <Lm.h>
#include <LmDfs.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Netapi32.lib")
#pragma comment (lib, "Mpr.lib")

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

} // namespace eacopy
