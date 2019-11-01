// (c) Electronic Arts. All Rights Reserved.

#include "EACopyNetwork.h"
#include "EACopyServer.h"
#include <ws2tcpip.h>
#include <Lm.h>
#include <assert.h>
#include <conio.h>
#include <strsafe.h>
#include <psapi.h>

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
#include "EACopyZdelta.h"
#endif

#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
#include <EACopyRsync.h>
#endif

#pragma comment (lib, "Netapi32.lib")

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void
Server::start(const ServerSettings& settings, Log& log, bool isConsole, ReportServerStatus reportStatus)
{
	m_startTime = getTimeMs();
	m_isConsole = isConsole;

	LogContext logContext(log);

	if (!reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000))
		return;

	// Initialize Winsock
	WSADATA wsaData;
	int res = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (res != 0)
	{
		logErrorf(L"WSAStartup failed with error: %d", res);
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	ScopeGuard cleanup([]() { WSACleanup(); });

	struct addrinfo hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	struct addrinfo* result = NULL;
	char portBuf[256];
	_itoa_s(settings.listenPort, portBuf, sizeof(portBuf), 10);
	res = getaddrinfo(NULL, portBuf, &hints, &result);
	if (res != 0)
	{
		logErrorf(L"getaddrinfo failed with error: %d", res);
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	ScopeGuard addrGuard([result]() { freeaddrinfo(result); });

	// Create a socket for listening to connections
	m_listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (m_listenSocket == INVALID_SOCKET)
	{
		logErrorf(L"socket failed with error: %s", getErrorText(WSAGetLastError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	ScopeGuard listenSocketCleanup([&]() { closesocket(m_listenSocket); m_listenSocket = INVALID_SOCKET; });

	// Setup the TCP listening socket
	res = bind(m_listenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (res == SOCKET_ERROR)
	{
		logErrorf(L"bind failed with error: %s", getErrorText(WSAGetLastError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	addrGuard.execute();

	res = listen(m_listenSocket, SOMAXCONN);
	if (res == SOCKET_ERROR)
	{
		logErrorf(L"listen failed with error: %s", getErrorText(WSAGetLastError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	logInfoLinef(L"Server started. Listening on port %i (Press Esc to quit)", settings.listenPort);

	List<ConnectionInfo> connections;

	reportStatus(SERVICE_RUNNING, NO_ERROR, 0);

	TIMEVAL timeval;
	timeval.tv_sec = 0;
	timeval.tv_usec = isConsole ? 1000 : 5*1000*1000; // 1ms timeout if console application.. otherwise 5 seconds timeout

	while (m_loopServer || !connections.empty())
	{
		// If console we check input
		if (isConsole && _kbhit())
		{
			char ch = _getch();
			if (ch == 27 || ch == 'q')
				m_loopServer = false;
		}

		int selectRes = 0;
		
		if (m_listenSocket != INVALID_SOCKET)
		{
			// check if the socket is ready
			fd_set read, err;
			FD_ZERO(&read);
			FD_ZERO(&err);
			FD_SET(m_listenSocket, &read);
			FD_SET(m_listenSocket, &err);
			selectRes = select(0,&read,nullptr,&err,&timeval);

			if (selectRes == SOCKET_ERROR)
			{
				m_loopServer = false;
				logErrorf(L"Select failed with error: %s", getErrorText(WSAGetLastError()).c_str());
				reportStatus(SERVICE_RUNNING, -1, 3000);
			}
		}

		// If nothing was set we got a timeout and take the chance to do some cleanup of state
		if(!selectRes)
		{
			for (auto i=connections.begin(); i!=connections.end();)
			{
				if (!m_loopServer) // If server is shutting down we need to close the connection sockets to prevent potential deadlocks
					closesocket(i->socket);

				DWORD exitCode;
				if (!i->thread->getExitCode(exitCode))
					return;
				if (exitCode == STILL_ACTIVE)
				{
					++i;
					continue;
				}
				assert(i->socket == INVALID_SOCKET);
				delete i->thread;
				i = connections.erase(i);
				--m_activeConnectionCount;
			}

			// When there are no connections active we take the opportunity to shrink history if overflowed
			if (connections.empty())
			{
				m_localFilesCs.enter();
				if (m_localFilesHistory.size() > settings.maxHistory)
				{
					uint removeCount = (uint)m_localFilesHistory.size() - settings.maxHistory;
					uint it = removeCount;
					while (it--)
					{
						m_localFiles.erase(m_localFilesHistory.front());
						m_localFilesHistory.pop_front();
					}
					logDebugLinef(L"History overflow. Removed %u entries", removeCount);

				}
				m_localFilesCs.leave();
			}
			continue;
		}

		for (uint i=0; i!=selectRes; ++i)
		{
			// Accept a client socket (this is blocking but select should have checked so there is one knocking on the door)
			SOCKET clientSocket = accept(m_listenSocket, NULL, NULL);

			if (clientSocket == INVALID_SOCKET)
			{
				if (m_loopServer)
					logErrorf(L"accept failed with WSA error: %s", getErrorText(WSAGetLastError()).c_str());
				reportStatus(SERVICE_RUNNING, -1, 3000);
				m_loopServer = false;
				break;
			}

			++m_handledConnectionCount;

			logDebugLinef(L"Connection accepted");
			connections.emplace_back(log, settings, clientSocket);
			ConnectionInfo& info = connections.back();

			info.thread = new Thread([this, &info]() { return connectionThread(info); });

			++m_activeConnectionCount;
		}
	}
}

void
Server::stop()
{
	m_loopServer = false;
	SOCKET listenSocket = m_listenSocket;
	m_listenSocket = INVALID_SOCKET;
	closesocket(listenSocket);
}

bool
Server::primeDirectory(const wchar_t* directory)
{
	WString localDir;
	if (directory[1] == ':')
		localDir = directory;
	else if (!getLocalFromNet(localDir, directory))
		return false;
	if (*localDir.rbegin() != '\\')
		localDir += '\\';
	return primeDirectoryRecursive(localDir);
}


bool
Server::primeDirectoryRecursive(const WString& directory)
{
    WIN32_FIND_DATAW fd; 
    WString searchStr = directory + L"*.*";
    HANDLE hFind = ::FindFirstFileW(searchStr.c_str(), &fd); 
    if(hFind == INVALID_HANDLE_VALUE)
	{
		logErrorf(L"FindFirstFile failed with search string %s", searchStr.c_str());
		return false;
	}
	ScopeGuard _([&]() { FindClose(hFind); });
    do
	{
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			if ((wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0))
				continue;
			if (!primeDirectoryRecursive(directory + fd.cFileName + L'\\'))
				return false;
		}
		else
		{
			addToLocalFilesHistory({ fd.cFileName, fd.ftLastWriteTime, ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow }, directory + fd.cFileName);
		}
	} 
	while(FindNextFileW(hFind, &fd)); 

	return true;
}

DWORD
Server::connectionThread(ConnectionInfo& info)
{
	LogContext logContext(info.log);
	uint bufferSize = 512*1024;
	char* recvBuffer1 = new char[bufferSize];
	char* recvBuffer2 = new char[bufferSize];
	char* recvBuffer = recvBuffer1;

	ScopeGuard closeSocket([&]() { closesocket(info.socket); info.socket = INVALID_SOCKET; delete[] recvBuffer1; delete[] recvBuffer2; logDebugLinef(L"Connection closed..."); });

	// Experimenting with speeding up network performance. This didn't make any difference
	// setRecvBufferSize(info.socket, 16*1024*1024);

	// Disable Nagle's algorithm (makes a big difference!)
	if (!disableNagle(info.socket))
		return -1;


	// Sending protocol version to connection
	{
		VersionCommand cmd;
		cmd.commandType = CommandType_Version;
		cmd.commandSize = sizeof(cmd);
		cmd.protocolVersion = ProtocolVersion;
		if (!sendData(info.socket, &cmd, sizeof(VersionCommand)))
			return -1;
	}

	uint recvPos = 0;
	WString localPath;

	// This is using writeReport
	int entryCount[] = { 0, 0, 0, 0 };
	ScopeGuard logDebugReport([&]()
	{ 
		logScopeEnter();
		logDebugLinef(L"--------- Socket report ---------");
		logDebugLinef(L"          Copy   CopyDelta Link   Skip");
		logDebugLinef(L"Files   %6i %6i %6i %6i", entryCount[0], entryCount[1], entryCount[2], entryCount[2]);
		logDebugLinef(L"---------------------------------");
		logScopeLeave();
	});

	NetworkCopyContext copyContext;
	CompressionData	compressionData;

	bool isValidEnvironment = false;
	bool isDone = false;
	u64 deltaCompressionThreshold = ~0u;

	// Receive until the peer shuts down the connection
	while (!isDone && m_loopServer)
	{
		int res = recv(info.socket, recvBuffer + recvPos, bufferSize - recvPos, 0);
		if (res == 0)
		{
			logDebugLinef(L"Connection closing...");
			break;
		}
		if (res < 0)
		{
			int error = WSAGetLastError();
			if (error == WSAECONNRESET) // An existing connection was forcibly closed by the remote host.
				logInfoLinef(L"An existing connection was forcibly closed by the remote host");
			else
				logErrorf(L"recv failed with error: %s", getErrorText(error).c_str());
			return -1;
		}

		//logDebugLinef("Bytes received: %d", res);

		recvPos += res;
		
		while (true)
		{
			if (recvPos < sizeof(Command))
				break;
			auto& header = *(Command*)recvBuffer;
			if (recvPos < header.commandSize)
				break;

			switch (header.commandType)
			{
			case CommandType_Environment:
				{
					auto& cmd = *(const EnvironmentCommand*)recvBuffer;

					deltaCompressionThreshold = cmd.deltaCompressionThreshold;

					if (!getLocalFromNet(localPath, cmd.netDirectory))
						break;
					isValidEnvironment  = true;
				}
				break;
			case CommandType_Text:
				{
					auto& cmd = *(const TextCommand*)recvBuffer;
					logInfoLinef(L"%s", cmd.string);
				}
				break;
			case CommandType_WriteFile:
				{
					if (!isValidEnvironment)
					{
						WriteResponse writeResponse = WriteResponse_BadDestination;
						if (!sendData(info.socket, &writeResponse, sizeof(writeResponse)))
							return -1;
						break;
					}

					auto& cmd = *(const WriteFileCommand*)recvBuffer;
					WString fullPath = localPath + cmd.path;

					//logDebugLinef("%s", fullPath.c_str());

					const wchar_t* fileName = cmd.path;
					if (const wchar_t* lastSlash = wcsrchr(fileName, '\\'))
						fileName = lastSlash + 1;

					// Robocopy style key for uniqueness of file
					FileKey key { fileName, cmd.info.lastWriteTime, cmd.info.fileSize };

					// Check if a file with the same key has already been copied at some point
					m_localFilesCs.enter();
					FileRec localFile;
					auto findIt = m_localFiles.find(key);
					if (findIt != m_localFiles.end())
						localFile = findIt->second;
					m_localFilesCs.leave();

					WriteResponse writeResponse = WriteResponse_Copy;

					if (!localFile.name.empty())
					{
						// File has already been copied, if the old copied file still has the same attributes as when it was copied we create a link to it
						FileInfo other;
						DWORD attributes = getFileInfo(other, localFile.name.c_str());
						if (attributes && equals(cmd.info, other))
						{
							bool skip;
							if (createFileLink(fullPath.c_str(), cmd.info, localFile.name.c_str(), skip))
								writeResponse = skip ? WriteResponse_Skip : WriteResponse_Link;
						}
					}
					else
					{
						// Check if file already exists at destination and has same attributes, in that case, skip copy
						FileInfo other;
						DWORD attributes = getFileInfo(other, fullPath.c_str());
						if (attributes && equals(cmd.info, other))
							writeResponse = WriteResponse_Skip;
					}

					// If CopyDelta is enabled we should look for a file that we believe is a very similar file and use that to send delta
					#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
					WString fileForCopyDelta;
					if (cmd.info.fileSize >= deltaCompressionThreshold && writeResponse == WriteResponse_Copy)
						if (findFileForDeltaCopy(fileForCopyDelta, key))
						{
							// TODO: Right now we don't support copy delta to same destination as the file we use for delta
							if (fileForCopyDelta != fullPath)
							{
								FileInfo fi;
								if (getFileInfo(fi, fileForCopyDelta.c_str()))
									writeResponse = WriteResponse_CopyDelta;
							}
						}
					#endif

					++entryCount[writeResponse];

					// Send response of action
					if (!sendData(info.socket, &writeResponse, sizeof(writeResponse)))
						return -1;

					// Skip or Link means that we are done, just add to history and move on (history will kick out oldest entry if full)
					if (writeResponse == WriteResponse_Link || writeResponse == WriteResponse_Skip)
					{
						InterlockedAdd64((LONG64*)(writeResponse != WriteResponse_Skip ? &m_bytesLinked : &m_bytesSkipped), cmd.info.fileSize);
						addToLocalFilesHistory(key, fullPath);
						break;
					}

					bool success = false;
					u64 totalReceivedSize = 0;

					if (writeResponse == WriteResponse_CopyDelta)
					{
						#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
						RsyncStats stats;
						if (!serverHandleRsync(info.socket, fileForCopyDelta.c_str(), fullPath.c_str(), cmd.info.lastWriteTime, stats))
							return -1;
						success = true;
						#endif
					}
					else
					{
						bool useBufferedIO = getUseBufferedIO(info.settings.useBufferedIO, cmd.info.fileSize);
						CopyStats copyStats;
						RecvFileStats recvStats;
						if (!receiveFile(success, info.socket, fullPath.c_str(), cmd.info.fileSize, cmd.info.lastWriteTime, cmd.writeType, useBufferedIO, copyContext, recvBuffer, recvPos, header.commandSize, copyStats, recvStats))
							return -1;
					}

					if (success)
					{
						addToLocalFilesHistory(key, fullPath); // Add newly written file to local file lookup.. if it existed before, make sure to move it to latest history to prevent it from being thrown out
						InterlockedAdd64((LONG64*)&m_bytesCopied, cmd.info.fileSize);
						InterlockedAdd64((LONG64*)&m_bytesReceived, totalReceivedSize);
					}
					u8 copyResult = success ? 1 : 0;
					if (!sendData(info.socket, &copyResult, sizeof(copyResult)))
						return -1;
				}
				break;

			case CommandType_ReadFile:
				{
					if (!isValidEnvironment)
					{
						ReadResponse readResponse = ReadResponse_BadSource;
						if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
							return -1;
						break;
					}

					auto& cmd = *(const ReadFileCommand*)recvBuffer;
					WString fullPath = localPath + cmd.path;

					FileInfo fi;
					DWORD attributes = getFileInfo(fi, fullPath.c_str());
					if (!attributes || attributes & FILE_ATTRIBUTE_DIRECTORY)
					{
						ReadResponse readResponse = ReadResponse_BadSource;
						if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
							return -1;
						break;
					}

					ReadResponse readResponse = ReadResponse_Copy;
					if (equals(fi, cmd.info))
						readResponse = ReadResponse_Skip;

					// Check if the version that the client has exist and in that case send as delta
					#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
					const wchar_t* referenceFileName = nullptr;
					FileKey key { cmd.path, cmd.info.lastWriteTime, cmd.info.fileSize };
					m_localFilesCs.enter();
					auto findIt = m_localFiles.find(key);
					if (findIt != m_localFiles.end())
						referenceFileName = findIt->second.name.c_str();
					m_localFilesCs.leave();

					if (referenceFileName != nullptr)
						readResponse = ReadResponse_CopyDelta;
					#endif

					if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
						return -1;

					if (readResponse == ReadResponse_Skip)
						break;

					if (!sendData(info.socket, &fi.lastWriteTime, sizeof(fi.lastWriteTime)))
						return -1;

					if (readResponse == ReadResponse_Copy)
					{
						if (!sendData(info.socket, &fi.fileSize, sizeof(fi.fileSize)))
							return -1;


						WriteFileType writeType = cmd.compressionEnabled ? WriteFileType_Compressed : WriteFileType_Send;

						bool useBufferedIO = getUseBufferedIO(info.settings.useBufferedIO, fi.fileSize);
						CopyStats copyStats;
						SendFileStats sendStats;
						if (!sendFile(info.socket, fullPath.c_str(), fi.fileSize, writeType, copyContext, compressionData, useBufferedIO, copyStats, sendStats))
							return -1;
					}
					else // ReadResponse_CopyDelta
					{
						#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
						if (!sendZdelta(info.socket, referenceFileName, fullPath.c_str()))
							return -1;
						#endif
						return -1;
					}

				}
				break;
			
			case CommandType_CreateDir:
				{
					CreateDirResponse createDirResponse = CreateDirResponse_Success;

					if (isValidEnvironment)
					{
						auto& cmd = *(const CreateDirCommand*)recvBuffer;
						WString fullPath = localPath + cmd.path;
						if (!ensureDirectory(fullPath.c_str()))
							createDirResponse = CreateDirResponse_Error;
					}
					else
						createDirResponse = CreateDirResponse_BadDestination;


					if (!sendData(info.socket, &createDirResponse, sizeof(createDirResponse)))
						return -1;
				}
				break;

			case CommandType_DeleteFiles:
				{
					DeleteFilesResponse deleteFilesResponse = DeleteFilesResponse_Success;

					if (isValidEnvironment)
					{
						auto& cmd = *(const CreateDirCommand*)recvBuffer;
						WString fullPath = localPath + cmd.path;
						if (!deleteAllFiles(fullPath.c_str()))
							deleteFilesResponse = DeleteFilesResponse_Error;
					}
					else
						deleteFilesResponse = DeleteFilesResponse_BadDestination;

					if (!sendData(info.socket, &deleteFilesResponse, sizeof(deleteFilesResponse)))
						return -1;
				}
				break;

			case CommandType_FindFiles:
				{
					auto& cmd = *(const FindFilesCommand*)recvBuffer;
					WIN32_FIND_DATAW fd; 
					WString searchStr = localPath + cmd.pathAndWildcard;
					HANDLE hFind = ::FindFirstFileW(searchStr.c_str(), &fd); 
					if(hFind == INVALID_HANDLE_VALUE)
					{
						uint blockSize = ~0u;
						if (!sendData(info.socket, &blockSize, sizeof(blockSize)))
							return -1;
						break;
					}
					ScopeGuard _([&]() { FindClose(hFind); });

					u8* bufferPos = copyContext.buffers[0];

					auto writeBlock = [&]()
					{
						uint blockSize = bufferPos - copyContext.buffers[0];
						if (!sendData(info.socket, &blockSize, sizeof(blockSize)))
							return false;
						bufferPos = copyContext.buffers[0];
						if (!sendData(info.socket, bufferPos, blockSize))
							return false;
						return true;
					};

					do
					{ 
						if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0))
							continue;

						uint fileNameBytes = (wcslen(fd.cFileName)+1)*2;

						if (fileNameBytes + 20 >= CopyContextBufferSize)
							if (!writeBlock())
								return -1;

						*(uint*)bufferPos = fd.dwFileAttributes;
						bufferPos += sizeof(uint);
						*(FILETIME*)bufferPos = fd.ftLastWriteTime;
						bufferPos += sizeof(u64);
						*(u64*)bufferPos = ((u64)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
						bufferPos += sizeof(u64);

						memcpy(bufferPos, fd.cFileName, fileNameBytes);
						bufferPos += fileNameBytes;
					}
					while(FindNextFileW(hFind, &fd)); 

					if (!writeBlock()) // Flush block
						return -1;

					if (!writeBlock()) // Write empty block to tell client we're done
						return -1;
				}
				break;

			case CommandType_RequestReport:
				{
					u64 uptimeMs = getTimeMs() - m_startTime;
					m_localFilesCs.enter();
					uint historySize = (uint)m_localFiles.size();
					m_localFilesCs.leave();
					
					PROCESS_MEMORY_COUNTERS memCounters;
					memCounters.cb = sizeof(memCounters);
					GetProcessMemoryInfo(GetCurrentProcess(), &memCounters, sizeof(memCounters));

					u64 freeVolumeSpace = 0;
					ULARGE_INTEGER freeBytesAvailable;
					if (GetDiskFreeSpaceExW(localPath.c_str(), nullptr, nullptr, &freeBytesAvailable))
						freeVolumeSpace = freeBytesAvailable.QuadPart;

					uint activeConnectionCount = m_activeConnectionCount - 1; // Skip the connection that is asking for this info

					wchar_t buffer[4096];
					StringCbPrintfW(buffer, sizeof(buffer),
						L"   Server v%S  (c) Electronic Arts.  All Rights Reserved.\n"
						L"\n"
						L"   Protocol: v%u\n"
						L"   Running as: %s\n"
						L"   Uptime: %s\n"
						L"   Connections active: %u (handled: %u)\n"
						L"   Local file history size: %u\n"
						L"   Memory working set: %s (Peak: %s)\n"
						L"   Free space on volume: %s\n"
						L"\n"
						L"   %s copied (%s received)\n"
						L"   %s linked\n"
						L"   %s skipped\n"
						, ServerVersion, ProtocolVersion, m_isConsole ? L"Console" : L"Service"
						, toHourMinSec(uptimeMs).c_str()
						, activeConnectionCount, m_handledConnectionCount, historySize, toPretty(memCounters.WorkingSetSize).c_str(), toPretty(memCounters.PeakWorkingSetSize).c_str()
						, toPretty(freeVolumeSpace).c_str(), toPretty(m_bytesCopied).c_str(), toPretty(m_bytesReceived).c_str(), toPretty(m_bytesLinked).c_str(), toPretty(m_bytesSkipped).c_str());

					bool isFirst = true;
					info.log.traverseRecentErrors([&](const WString& error)
						{
							if (isFirst)
							{
								wcscat_s(buffer, eacopy_sizeof_array(buffer), L"\n   Recent errors:\n");
								isFirst = false;
							}
							wcscat_s(buffer, L"      ");
							wcscat_s(buffer, eacopy_sizeof_array(buffer), error.c_str());
							wcscat_s(buffer, L"\n");
							return true;
						});

					uint bufferLen = (uint)wcslen(buffer);
					if (!sendData(info.socket, &bufferLen, sizeof(bufferLen)))
						return -1;
					if (!sendData(info.socket, buffer, bufferLen*2))
						return -1;
				}
				break;
			case CommandType_Done:
				isDone = true;
				break;
			}

			recvPos -= header.commandSize;
			if (recvPos == 0)
				break;

			char* oldRecv = recvBuffer;
			char* newRecv = oldRecv == recvBuffer1 ? recvBuffer2 : recvBuffer1;
			memcpy(newRecv, oldRecv + header.commandSize, recvPos);
			recvBuffer = newRecv;
		}
	}

	// shutdown the connection since we're done
	if (shutdown(info.socket, SD_BOTH) == SOCKET_ERROR)
	{
		logErrorf(L"shutdown failed with error: %s", getErrorText(WSAGetLastError()).c_str());
		return -1;
	}

	return 0;
}

void
Server::addToLocalFilesHistory(const FileKey& key, const WString& fullFileName)
{
	m_localFilesCs.enter();
	auto insres = m_localFiles.insert({key, FileRec()});
	if (!insres.second)
		m_localFilesHistory.erase(insres.first->second.historyIt);
	m_localFilesHistory.push_back(key);
	insres.first->second.name = fullFileName;
	insres.first->second.historyIt = --m_localFilesHistory.end();
	m_localFilesCs.leave();
}

bool
Server::getLocalFromNet(WString& localDirectory, const wchar_t* netDirectory)
{
	localDirectory = netDirectory;
	const wchar_t* localPath = wcschr(netDirectory, '\\');
	if (!localPath)
		localPath = netDirectory + wcslen(netDirectory);

	WString netname(netDirectory, localPath);

	PSHARE_INFO_502 shareInfo;

	NET_API_STATUS res = NetShareGetInfo(NULL, const_cast<LPWSTR>(netname.c_str()), 502, (LPBYTE*) &shareInfo);
	if (res != NERR_Success)
	{
		logErrorf(L"Failed to find netshare '%s'", netname.c_str());
		return false;
	}

	WString wpath(shareInfo->shi502_path);
	localDirectory = WString(wpath.begin(), wpath.end());
	if (*localPath)
		localDirectory += localPath;
	localDirectory += '\\';

	NetApiBufferFree(shareInfo);
	return true;
}

bool
Server::findFileForDeltaCopy(WString& outFile, const FileKey& key)
{
	FileKey searchKey { key.name, 0, 0 };
	auto searchIt = m_localFiles.lower_bound(searchKey);
	while (searchIt != m_localFiles.end())
	{
		if (searchIt->first.name != key.name)
			return false;
		outFile = searchIt->second.name;
		return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
