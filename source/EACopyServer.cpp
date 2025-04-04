// (c) Electronic Arts. All Rights Reserved.

#include "EACopyNetwork.h"
#include "EACopyServer.h"
#include <ws2tcpip.h>
#include <Lm.h>
#include <assert.h>
#include <combaseapi.h>
#include <conio.h>
#include <strsafe.h>
#include <psapi.h>
#include <Rpc.h>

#if defined(EACOPY_ALLOW_DELTA_COPY)
#include "EACopyDelta.h"
#endif

#if defined(EACOPY_ALLOW_RSYNC)
#include <EACopyRsync.h>
#endif

#pragma comment (lib, "Netapi32.lib")

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void
Server::start(const ServerSettings& settings, Log& log, bool isConsole, ReportServerStatus reportStatus)
{
	m_startTime = getTime();
	m_isConsole = isConsole;

	LogContext logContext(log);

	if (!reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000))
		return;

	for (auto& primeDir : settings.additionalLinkDirectories)
		primeDirectory(primeDir.c_str(), settings.useLinksRelativePath);

	// Initialize Winsock
	WSADATA wsaData;
	int res = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (res != 0)
	{
		logErrorf(L"WSAStartup failed with error: %d", res);
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	char localipBuffer[256];
	char* localip = nullptr;
	
	if (!settings.listenIp.empty())
	{
		sprintf_s(localipBuffer, sizeof(localipBuffer), "%ls", settings.listenIp.c_str());
		localip = localipBuffer;
	}
	else
	{
		char ac[512];
		res = gethostname(ac, sizeof(ac));
		if (res != SOCKET_ERROR)
		{
			hostent* phe = gethostbyname(ac);
			if (phe != 0 && phe->h_addr_list[1]) // More than one ip addresses
			{
				logInfoLinef(L"More than one ip address found. Will listen on first ip in list (use /IP:<ipaddress> to use other)");
				for (int i = 0; phe->h_addr_list[i] != 0; ++i)
				{
					in_addr addr;
					memcpy(&addr, phe->h_addr_list[i], sizeof(in_addr));
					char* ip = inet_ntoa(addr);
					logInfoLinef(L"  %i: %hs", i, ip);
				}
			}
		}
	}

	ScopeGuard cleanup([]() { WSACleanup(); });

	struct addrinfo hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	WString networkServerName;
	struct addrinfo* result = NULL;
	char portBuf[256];
	_itoa_s(settings.listenPort, portBuf, sizeof(portBuf), 10);
	res = getaddrinfo(localip, portBuf, &hints, &result);
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
		logErrorf(L"socket failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	ScopeGuard listenSocketCleanup([&]() { closesocket(m_listenSocket); m_listenSocket = INVALID_SOCKET; });

	// Setup the TCP listening socket
	res = bind(m_listenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (res == SOCKET_ERROR)
	{
		logErrorf(L"bind failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	addrGuard.execute();

	res = listen(m_listenSocket, SOMAXCONN);
	if (res == SOCKET_ERROR)
	{
		logErrorf(L"listen failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
		reportStatus(SERVICE_START_PENDING, -1, 3000);
		return;
	}

	logInfoLinef(L"Server started. Listening on port %i (Press Esc to quit)", settings.listenPort);

	List<ConnectionInfo> connections;

	reportStatus(SERVICE_RUNNING, NO_ERROR, 0);

	// 1ms timeout if console application.. otherwise 5 seconds (and 1ms) timeout
	TIMEVAL timeval;
	timeval.tv_sec = isConsole ? 0 : 5;
	timeval.tv_usec = 1000;

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
				logErrorf(L"Select failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
				reportStatus(SERVICE_RUNNING, -1, 3000);
			}
		}

		// If nothing was set we got a timeout and take the chance to do some cleanup of state
		if(!selectRes)
		{
			for (auto i=connections.begin(); i!=connections.end();)
			{
				if (!m_loopServer) // If server is shutting down we need to close the connection sockets to prevent potential deadlocks
					closeSocket(i->socket);

				uint exitCode;
				if (!i->thread->getExitCode(exitCode))
					return;
				if (exitCode == STILL_ACTIVE)
				{
					++i;
					continue;
				}
				assert(i->socket.socket == INVALID_SOCKET);
				i = connections.erase(i);
				--m_activeConnectionCount;
			}

			// When there are no connections active we take the opportunity to shrink history if overflowed
			if (connections.empty())
			{
				if (uint removeCount = m_database.garbageCollect(settings.maxHistory))
					logDebugLinef(L"History overflow. Removed %u entries", removeCount);
				logFlush();
			}
			continue;
		}

		for (uint i=0; i!=selectRes; ++i)
		{
			// Accept a client socket (this is blocking but select should have checked so there is one knocking on the door)
			sockaddr_in remoteSockAddr = { 0 }; /* for TCP/IP */
			socklen_t remoteSockAddrLen = sizeof(remoteSockAddr);
			SOCKET clientSocket = accept(m_listenSocket, (sockaddr*)&remoteSockAddr, &remoteSockAddrLen);

			if (clientSocket == INVALID_SOCKET)
			{
				if (m_loopServer)
					logErrorf(L"accept failed with WSA error: %ls", getErrorText(getLastNetworkError()).c_str());
				reportStatus(SERVICE_RUNNING, -1, 3000);
				m_loopServer = false;
				break;
			}

			char* remoteIp = inet_ntoa(remoteSockAddr.sin_addr);
			uint socketIndex = m_handledConnectionCount++;

			//logDebugLinef(L"Connection %u accepted (%hs)", socketIndex, remoteIp);
			Socket sock = { clientSocket, socketIndex };
			connections.emplace_back(log, settings, sock);
			ConnectionInfo& info = connections.back();
			info.remoteIp = WString(remoteIp, remoteIp + strlen(remoteIp));

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
Server::primeDirectory(const wchar_t* directory, bool useLinksRelativePath)
{
	WString serverDir;
	bool isExternalDir;
	if (directory[1] == ':')
		serverDir = directory;
	else if (!getLocalFromNet(serverDir, isExternalDir, directory))
		return false;
	if (*serverDir.rbegin() != '\\')
		serverDir += '\\';
	IOStats ioStats;
	return m_database.primeDirectory(serverDir, ioStats, useLinksRelativePath, true);
}

#define EACOPY_COMMAND(x) L"CMD" L#x,

const wchar_t* commandNames[] =
{
	EACOPY_COMMANDS
};

uint
Server::connectionThread(ConnectionInfo& info)
{
	LogContext logContext(info.log);
	uint bufferSize = 512*1024;
	char* recvBuffer1 = new char[bufferSize];
	char* recvBuffer2 = new char[bufferSize];
	char* recvBuffer = recvBuffer1;

	uint commands[CommandType_Bad] = { 0 };
	u64 commandTimes[CommandType_Bad] = { 0 };
	uint readEntries[ReadResponse_BadSource] = { 0 };
	uint readEntryCount = 0;
	uint writeEntries[WriteResponse_BadDestination] = { 0 };
	uint writeEntryCount = 0;
	IOStats ioStats;

	ScopeGuard closeSocket([&]()
	{
			closeSocket(info.socket);
			delete[] recvBuffer1;
			delete[] recvBuffer2;
			logScopeEnter();
			logDebugLinef(L"--------- Connection %u closed ---------", info.socket.index);
			
			if (readEntryCount)
			{
				logDebugLinef(L"             Copy   CopyDelta CopySmb   Skip   Hash    ServerBusy");
				logDebugLinef(L"   Reads   %6i      %6i  %6i %6i %6i        %6i", readEntries[0], readEntries[1], readEntries[2], readEntries[3], readEntries[4], readEntries[5]);
			}
			if (writeEntryCount)
			{
				logDebugLinef(L"             Copy   CopyDelta CopySmb   Link    Odx   Skip   Hash");
				logDebugLinef(L"   Writes  %6i      %6i  %6i %6i %6i %6i %6i", writeEntries[0], writeEntries[1], writeEntries[2], writeEntries[3], writeEntries[4], writeEntries[5], writeEntries[6]);
			}
			logDebugLinef(L"");

			Vector<WString> statsVec;
			for (uint i = 0; i != CommandType_Bad; ++i)
				populateStatsTime(statsVec, commandNames[i], commandTimes[i], commands[i]);
			//populateIOStats(statsVec, ioStats);
			logDebugStats(statsVec);
			logDebugLinef(L"");

			// This connection session is finished, flush the log.to avoid file buffer build up getting too large.
			logFlush();

			logScopeLeave();
	});

	// Experimenting with speeding up network performance. This didn't make any difference
	// setRecvBufferSize(info.socket, 16*1024*1024);

	// Disable Nagle's algorithm (makes a big difference!)
	if (!disableNagle(info.socket))
		return -1;


	// Sending protocol version to connection
	{
		enum { MaxStringLength = 1024 };
		char cmdBuf[MaxStringLength*2 + sizeof(VersionCommand)+1];
		auto& cmd = *(VersionCommand*)cmdBuf;

		swprintf(cmd.info, L"v%ls", getServerVersionString().c_str());

		cmd.commandType = CommandType_Version;
		cmd.commandSize = sizeof(cmd) + uint(wcslen(cmd.info))*2;
		cmd.protocolVersion = m_protocolVersion;
		cmd.protocolFlags = 0;

		if (info.settings.useSecurityFile)
			cmd.protocolFlags |= UseSecurityFile;
		if (!sendData(info.socket, &cmd, cmd.commandSize))
			return -1;
	}

	SendFileStats sendStats;
	NetworkCopyContext copyContext;
	CompressionStats compressionStats;
	compressionStats.currentLevel = 1;

	uint recvPos = 0;
	WString serverPath;
	bool isValidEnvironment = false;
	bool isServerPathExternal = false; // Tells whether "local" directory is external or not (it could be pointing to a network share)
	bool isDone = false;
	uint clientConnectionIndex = ~0u; // Note that this is the connection index from the same client where 0 is the controlling connection and the rest are worker connections

	ScopeGuard queueRemoveGuard([&]()
		{
			if (clientConnectionIndex == ~0u)
				return;
			ScopedCriticalSection cs(m_queuesCs);
			Queue& q = m_queues[clientConnectionIndex];
			q.erase(std::find(q.begin(), q.end(), &info));
		});

	ActiveSession* activeSession = nullptr;

	Guid zeroGuid = {0};
	Guid secretGuid = {0};
	ScopeGuard removeSecretGuid([&]()
		{
			ScopedCriticalSection cs(m_activeSessionsCs);
			if (activeSession && --activeSession->connectionCount == 0)
				m_activeSessions.erase(secretGuid);
		});

	// Receive until the peer shuts down the connection
	while (!isDone && m_loopServer)
	{
		int res = recv(info.socket.socket, recvBuffer + recvPos, bufferSize - recvPos, 0);
		if (res == 0)
		{
			logDebugLinef(L"Connection %u closing...", info.socket.index);
			break;
		}
		if (res < 0)
		{
			int error = getLastNetworkError();
			if (error == WSAECONNRESET) // An existing connection was forcibly closed by the remote host.
				logInfoLinef(L"An existing connection was forcibly closed by the remote host");
			else
				logErrorf(L"recv failed with error: %ls", getErrorText(error).c_str());
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

			++commands[header.commandType];
			TimerScope commandTimer(commandTimes[header.commandType]);

			switch (header.commandType)
			{
			case CommandType_Environment:
				{
					auto& cmd = *(const EnvironmentCommand*)recvBuffer;
					secretGuid = cmd.secretGuid;

					logScopeEnter();
					logDebugLinef(L"--- Connection %u opened ---", info.socket.index);
					logDebugLinef(L"  RemoteIp              : %ls", info.remoteIp.c_str());
					logDebugLinef(L"  ClientVersion         : %ls", getVersionString(cmd.majorVersion, cmd.minorVersion, false).c_str());
					logDebugLinef(L"  RemoteConnectionIndex : %u", cmd.connectionIndex);
					logDebugLinef(L"  NetDirectory          : %ls", cmd.netDirectory);
					logDebugLinef(L"");
					logScopeLeave();

					clientConnectionIndex = min(cmd.connectionIndex, MaxPriorityQueueCount - 1);

					m_queuesCs.scoped([&]() { m_queues[clientConnectionIndex].push_back(&info); });

					if (!getLocalFromNet(serverPath, isServerPathExternal, cmd.netDirectory))
						return -1;

					if (info.settings.useSecurityFile)
					{
						if (cmd.secretGuid != zeroGuid)
						{
							// Connection provided secretGuid, check against table of valid secretGuids

							ScopedCriticalSection _(m_activeSessionsCs);
							auto findIt = m_activeSessions.find(cmd.secretGuid);
							if (findIt != m_activeSessions.end())
							{
								activeSession = &(findIt->second);
								++activeSession->connectionCount;
							}
							else
							{
								logInfoLinef(L"Connection is providing invalid secret guid.. disconnect");
								return -1;
							}
						}
						else
						{
							// No secretGuid provided, let's test the clients access to the network path
							// by putting a hidden file there with a guid in it, if client can return that guid it means that client has access

							static_assert(sizeof(GUID) == sizeof(Guid), "GUID and Guid are not matching");
							GUID filenameGuid;
							if (CoCreateGuid(&filenameGuid) != S_OK)
							{
								logErrorf(L"CoCreateGuid - Failed to create filename guid");
								return -1;
							}

							wchar_t filename[128];
							filename[0] = L'.';
							StringFromGUID2(filenameGuid, filename + 1, 40);
							filename[1] = L'f';
							filename[38] = 0;

							if (CoCreateGuid((GUID*)&secretGuid) != S_OK)
							{
								logErrorf(L"CoCreateGuid - Failed to create secret guid");
								return -1;
							}

							// Create hidden security file with code
							FileInfo fileInfo;
							fileInfo.fileSize = sizeof(secretGuid);
							WString securityFilePath = serverPath + filename;
							if (!ensureDirectory(serverPath.c_str(), 0, ioStats))
							{
								logErrorf(L"Failed to create directory '%ls' needed to create secret guid file for client. Server does not have access?", serverPath.c_str());
								return -1;
							}
							ScopeGuard deleteFileGuard([&]() { deleteFile(securityFilePath.c_str(), ioStats, false); });
							if (!createFile(securityFilePath.c_str(), fileInfo, &secretGuid, ioStats, true, true))
								return -1;

							{
								ScopedCriticalSection _(m_activeSessionsCs);
								auto insres = m_activeSessions.insert({secretGuid, {}});
								if (!insres.second)
								{
									logErrorf(L"Failed to start new session. Session has already been started by other connection. Something is wrong.");
									return -1;
								}
								activeSession = &insres.first->second;
								++activeSession->connectionCount;
							}

							if (!sendData(info.socket, &filenameGuid, sizeof(filenameGuid)))
								return -1;

							Guid returnedSecretGuid;
							if (!receiveData(info.socket, &returnedSecretGuid, sizeof(returnedSecretGuid))) // Let client do the copying while we want for a success or not
								return -1;

							if (secretGuid != returnedSecretGuid)
							{
								logInfoLinef(L"Connection is providing invalid secret guid.. disconnect");
								return -1;
							}
						}
					}
					else
					{
						ScopedCriticalSection _(m_activeSessionsCs);
						auto insres = m_activeSessions.insert({secretGuid, {}});
						activeSession = &insres.first->second;
						++activeSession->connectionCount;
					}

					isValidEnvironment  = true;
				}
				break;
			case CommandType_Text:
				{
					auto& cmd = *(const TextCommand*)recvBuffer;
					logInfoLinef(L"%ls", cmd.string);
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
					WString fullPath = serverPath + cmd.path;

					//logDebugLinef("%ls", fullPath.c_str());

					const wchar_t* fileName = cmd.path;
					if (!info.settings.useLinksRelativePath)
						if (const wchar_t* lastSlash = wcsrchr(fileName, '\\'))
							fileName = lastSlash + 1;

					Hash hash;

					// Robocopy style key for uniqueness of file
					FileKey key { fileName, cmd.info.lastWriteTime, cmd.info.fileSize };

					// Check if a file with the same key has already been copied at some point
					FileDatabase::FileRec localFile = m_database.getRecord(key);

					WriteResponse writeResponse = (isServerPathExternal && cmd.writeType != WriteFileType_Compressed) ? WriteResponse_CopyUsingSmb : WriteResponse_Copy;

					if (!localFile.name.empty() && cmd.info.fileSize >= info.settings.useLinksThreshold)
					{
						// File has already been copied, if the old copied file still has the same attributes as when it was copied we create a link to it
						FileInfo localFileInfo;
						uint attributes = getFileInfo(localFileInfo, localFile.name.c_str(), ioStats);
						if (attributes && equals(cmd.info, localFileInfo))
						{
							hash = localFile.hash;

							FileInfo destInfo;

							if (fullPath == localFile.name) // We are copying to the same place we've copied before and the file there is still up-to-date, skip
							{
								writeResponse = WriteResponse_Skip;
							}
							else
							// For external shares CreateHardLink might return true even though it is a skip..
							// So the correct thing would be to check the file first but it is too costly so
							/*
							else if (isServerPathExternal && getFileInfo(destInfo, fullPath.c_str()) && equals(cmd.info, destInfo))
							{
								writeResponse = WriteResponse_Skip;
							}
							else
							*/
							{
								bool skip;
								if (createFileLink(fullPath.c_str(), cmd.info, localFile.name.c_str(), skip, ioStats))
								{
									writeResponse = skip ? WriteResponse_Skip : WriteResponse_Link;
								}
								else
								{
									logContext.resetLastError(); // We want to handle failing links as non-error and fallback to normal copying

									if (info.settings.useOdx)
									{
										// if destination file is read-only then we will clear that flag so the copy can succeed
										if (attributes & FILE_ATTRIBUTE_READONLY)
										{
											if (!setFileWritable(localFile.name.c_str(), true))
												logErrorf(L"Could not copy over read-only destination file (%ls).  EACopy could not forcefully unset the destination file's read-only attribute.", localFile.name.c_str());
										}
										bool existed = false;
										u64 bytesCopied;
										if (copyFile(localFile.name.c_str(), localFileInfo, attributes, fullPath.c_str(), true, false, existed, bytesCopied, copyContext, ioStats, info.settings.useBufferedIO))
											writeResponse = WriteResponse_Odx;
									}
								}
							}
						}
					}
					else
					{
						// Check if file already exists at destination and has same attributes, in that case, skip copy
						// If directory was created by session we don't have to check because we know it doesnt exist (if it is because of someone else writing it is not the end of the world.
						const wchar_t* lastSlash = wcsrchr(fullPath.c_str(), '\\');
						WString directory(fullPath.c_str(), lastSlash + 1);
						bool dirCreatedBySession;
						activeSession->createdDirsCs.scoped([&]() { dirCreatedBySession = activeSession->createdDirs.find(directory) != activeSession->createdDirs.end(); });
						if (!dirCreatedBySession)
						{
							FileInfo other;
							uint attributes = getFileInfo(other, fullPath.c_str(), ioStats);
							if (attributes && equals(cmd.info, other))
							{
								hash = localFile.hash;
								writeResponse = WriteResponse_Skip;
							}
						}
					}

					// If CopyDelta is enabled we should look for a file that we believe is a very similar file and use that to send delta
					#if defined(EACOPY_ALLOW_RSYNC)
					WString fileForCopyDelta;
					if (writeResponse == WriteResponse_Copy)
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

					if (info.settings.useHash && (writeResponse == WriteResponse_Copy || writeResponse == WriteResponse_CopyUsingSmb))
					{
						// Ask for hash of file first, it might still exist on the server but with different time stamp... and if it doesnt we still need the hash
						WriteResponse hashResponse = WriteResponse_Hash;
						if (!sendData(info.socket, &hashResponse, sizeof(hashResponse)))
							return -1;
						if (!receiveData(info.socket, &hash, sizeof(hash)))
							return -1;
						localFile = m_database.getRecord(hash);

						if (!localFile.name.empty()) // File exists on server but with different time stamp.
						{
							// TODO: Maybe check that localFile still has the same timestamp so noone has tampered with it
							// 
							// Attempt to create link to other file
							bool skip;
							if (cmd.info.fileSize >= info.settings.useLinksThreshold && createFileLink(fullPath.c_str(), cmd.info, localFile.name.c_str(), skip, ioStats))
							{
								writeResponse = skip ? WriteResponse_Skip : WriteResponse_Link;
							}
							else if (info.settings.useOdx)
							{
								bool existed = false;
								u64 bytesCopied;
								FileInfo localFileInfo;
								if (uint attributes = getFileInfo(localFileInfo, localFile.name.c_str(), ioStats))
								{
									// if destination file is read-only then we will clear that flag so the copy can succeed
									if (attributes & FILE_ATTRIBUTE_READONLY)
									{
										if (!setFileWritable(localFile.name.c_str(), true))
											logErrorf(L"Could not copy over read-only destination file (%ls).  EACopy could not forcefully unset the destination file's read-only attribute.", localFile.name.c_str());
									}
									if (copyFile(localFile.name.c_str(), localFileInfo, attributes, fullPath.c_str(), true, false, existed, bytesCopied, copyContext, ioStats, info.settings.useBufferedIO))
										writeResponse = WriteResponse_Odx;
								}
							}
						}
					}

					++writeEntries[writeResponse];
					++writeEntryCount;

					// Send response of action
					if (!sendData(info.socket, &writeResponse, sizeof(writeResponse)))
						return -1;

					// Skip, Odx or Link means that we are done, just add to history and move on (history will kick out oldest entry if full)
					if (writeResponse == WriteResponse_Link || writeResponse == WriteResponse_Odx || writeResponse == WriteResponse_Skip)
					{
						u64& bytes = writeResponse == WriteResponse_Odx ? m_bytesCopied : (writeResponse != WriteResponse_Skip ? m_bytesLinked : m_bytesSkipped);
						InterlockedAdd64((LONG64*)&bytes, cmd.info.fileSize);
						m_database.addToFilesHistory(key, hash, fullPath);
						break;
					}

					bool success = false;
					bool sendSuccess = true;
					u64 totalReceivedSize = 0;

					if (writeResponse == WriteResponse_CopyDelta)
					{
						#if defined(EACOPY_ALLOW_RSYNC)
						RsyncStats stats;
						if (!serverHandleRsync(info.socket, fileForCopyDelta.c_str(), fullPath.c_str(), cmd.info.lastWriteTime, stats))
							return -1;
						success = true;
						#endif
					}
					else if (writeResponse == WriteResponse_CopyUsingSmb)
					{
						u8 copyResult;
						if (!receiveData(info.socket, &copyResult, sizeof(copyResult))) // Let client do the copying while we want for a success or not
							return -1;
						success = copyResult != 0;
						sendSuccess = false;
					}
					else // WriteResponse_Copy
					{
						bool useBufferedIO = getUseBufferedIO(info.settings.useBufferedIO, cmd.info.fileSize);
						RecvFileStats recvStats;
						if (!receiveFile(success, info.socket, fullPath.c_str(), cmd.info.fileSize, cmd.info.lastWriteTime, cmd.writeType, useBufferedIO, copyContext, recvBuffer, recvPos, header.commandSize, ioStats, recvStats))
							return -1;
					}

					if (success)
					{
						m_database.addToFilesHistory(key, hash, fullPath); // Add newly written file to local file lookup.. if it existed before, make sure to move it to latest history to prevent it from being thrown out
						InterlockedAdd64((LONG64*)&m_bytesCopied, cmd.info.fileSize);
						InterlockedAdd64((LONG64*)&m_bytesReceived, totalReceivedSize);
					}

					if (sendSuccess)
					{
						u8 copyResult = success ? 1 : 0;
						if (!sendData(info.socket, &copyResult, sizeof(copyResult)))
							return -1;
					}
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

					bool tooBusy = false;

					{
						ScopedCriticalSection cs(m_queuesCs);

						uint totalCountBeforeThis = 0;

						// Count all connections with lower connection index
						for (uint i=0, e=clientConnectionIndex; i!=e; ++i)
							totalCountBeforeThis += m_queues[i].size();

						// If there is still room after all connections with lower connection index we prioritize oldest connection
						if (totalCountBeforeThis < info.settings.maxConcurrentDownloadCount)
						{
							Queue& q = m_queues[clientConnectionIndex];
							uint spotsLeftForQueue = info.settings.maxConcurrentDownloadCount - totalCountBeforeThis;
							if (spotsLeftForQueue < q.size())
							{
								for (auto i : q)
								{
									if (!spotsLeftForQueue--)
									{
										tooBusy = true;
										break;
									}
									if (i == &info)
										break;
								}
							}
						}
						else
							tooBusy = true;
					}

					if (tooBusy)
					{
						ReadResponse readResponse = ReadResponse_ServerBusy;
						++readEntryCount;
						++readEntries[readResponse];
						if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
							return -1;
						break;
					}


					auto& cmd = *(const ReadFileCommand*)recvBuffer;
					WString fullPath = serverPath + cmd.path;

					FileInfo fi;
					uint attributes = getFileInfo(fi, fullPath.c_str(), ioStats);
					if (!attributes || attributes & FILE_ATTRIBUTE_DIRECTORY)
					{
						ReadResponse readResponse = ReadResponse_BadSource;
						if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
							return -1;
						break;
					}

					const wchar_t* fileName = cmd.path;
					if (!info.settings.useLinksRelativePath)
						if (const wchar_t* lastSlash = wcsrchr(fileName, '\\'))
							fileName = lastSlash + 1;

					ReadResponse readResponse = (isServerPathExternal && cmd.compressionLevel == 0) ? ReadResponse_CopyUsingSmb : ReadResponse_Copy;
					// Check if the version that the client has exist and in that case send as delta
					#if defined(EACOPY_ALLOW_DELTA_COPY)
					FileDatabase::FileRec referenceFile;
					if (cmd.compressionLevel != 0 && info.settings.useDeltaCompression)
					{
						FileKey key{ fileName, cmd.info.lastWriteTime, cmd.info.fileSize };
						referenceFile = m_database.getRecord(key);
						if (!referenceFile.name.empty())
						{
							readResponse = ReadResponse_CopyDelta;
						}
					}
					#endif

					if (equals(fi, cmd.info))
					{
						readResponse = ReadResponse_Skip;
					}
					else if (info.settings.useHash && fi.fileSize == cmd.info.fileSize)  // Check if client's file content actually matches server's.. might only differ in last write time if size is the same
					{
						FileKey serverKey{ fileName, fi.lastWriteTime, fi.fileSize };
						Hash serverHash = m_database.getRecord(serverKey).hash;
						if (!isValid(serverHash))
						{
							// If setting already tell us to use hash and the file entry is missing from database, calculate the hash and add this to database
							CopyContext	copyContext;
							IOStats ioStats;
							u64 hashtime;
							u64 hashcount;
							HashContext hashContext(hashtime, hashcount);
							getFileHash(serverHash, fullPath.c_str(), copyContext, ioStats, hashContext, hashtime);
						}
						if (isValid(serverHash))
						{
							FileKey clientKey{ fileName, cmd.info.lastWriteTime, cmd.info.fileSize };
							Hash hash = m_database.getRecord(clientKey).hash;
							if (!isValid(hash))
							{
								ReadResponse hashResponse = ReadResponse_Hash;
								if (!sendData(info.socket, &hashResponse, sizeof(hashResponse)))
									return -1;
								if (!receiveData(info.socket, &hash, sizeof(hash)))
									return -1;
							}
							if (isValid(hash) && hash == serverHash) // They are actually the same file.. just different time
							{
								readResponse = ReadResponse_Skip;
							}
						}
					}

					if (!sendData(info.socket, &readResponse, sizeof(readResponse)))
						return -1;
					++readEntryCount;
					++readEntries[readResponse];

					if (readResponse == ReadResponse_Skip)
						break;

					if (!sendData(info.socket, &fi.lastWriteTime, sizeof(fi.lastWriteTime)))
						return -1;

					if (readResponse == ReadResponse_Copy)
					{
						if (!sendData(info.socket, &fi.fileSize, sizeof(fi.fileSize)))
							return -1;

						WriteFileType writeType = WriteFileType_Send;

						if (cmd.compressionLevel != 0)
						{
							writeType = WriteFileType_Compressed;
							if (cmd.compressionLevel != 255)
							{
								compressionStats.currentLevel = cmd.compressionLevel;
								compressionStats.fixedLevel = true;
							}
							else
								compressionStats.fixedLevel = false;
						}

						bool useBufferedIO = getUseBufferedIO(info.settings.useBufferedIO, fi.fileSize);
						if (!sendFile(info.socket, fullPath.c_str(), fi.fileSize, writeType, copyContext, compressionStats, useBufferedIO, ioStats, sendStats))
							return -1;
					}
					else if (readResponse == ReadResponse_CopyUsingSmb)
					{
						// NOP, client got this
					}
					else // ReadResponse_CopyDelta
					{
						#if defined(EACOPY_ALLOW_DELTA_COPY)
						if (!sendData(info.socket, &fi.fileSize, sizeof(fi.fileSize)))
							return -1;
						if (!sendDelta(info.socket, referenceFile.name.c_str(), cmd.info.fileSize, fullPath.c_str(), fi.fileSize, copyContext, ioStats))
							return -1;
						#else
						return -1;
						#endif
					}
				}
				break;
			
			case CommandType_CreateDir:
				{
					u8 createDirResponse = CreateDirResponse_Error;

					if (isValidEnvironment)
					{
						auto& cmd = *(const CreateDirCommand*)recvBuffer;
						WString fullPath = serverPath + cmd.path;
						FilesSet createdDirs;
						if (ensureDirectory(fullPath.c_str(), 0, ioStats, false, true, &createdDirs))
						{
							createDirResponse = CreateDirResponse_SuccessExisted + (u8)min(createdDirs.size(), 200); // is not the end of the world if 201 was created but 200 was reported
							activeSession->createdDirsCs.scoped([&]() { activeSession->createdDirs.insert(createdDirs.begin(), createdDirs.end()); });
						}
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
						WString fullPath = serverPath + cmd.path;
						if (!deleteAllFiles(fullPath.c_str(), ioStats, false)) // No error on missing files
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
					FindFileData fd;
					WString searchStr = serverPath + cmd.pathAndWildcard;

					WString tempBuffer;
					FindFileHandle findHandle = findFirstFile(searchStr.c_str(), fd, ioStats);
					if(findHandle == InvalidFileHandle)
					{
						uint blockSize = ~0u;
						if (!sendData(info.socket, &blockSize, sizeof(blockSize)))
							return -1;
						break;
					}
					ScopeGuard _([&]() { findClose(findHandle, ioStats); });

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
						FileInfo fileInfo;
						uint attributes = getFileInfo(fileInfo, fd);

						const wchar_t* fileName = getFileName(fd);
						if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && isDotOrDotDot(fileName))
							continue;

						uint fileNameBytes = (wcslen(fileName)+1)*2;

						if (fileNameBytes + 20 >= CopyContextBufferSize)
							if (!writeBlock())
								return -1;

						*(uint*)bufferPos = attributes;
						bufferPos += sizeof(uint);
						*(FileTime*)bufferPos = fileInfo.lastWriteTime;
						bufferPos += sizeof(u64);
						*(u64*)bufferPos = fileInfo.fileSize;
						bufferPos += sizeof(u64);

						memcpy(bufferPos, fileName, fileNameBytes);
						bufferPos += fileNameBytes;
					}
					while(findNextFile(findHandle, fd, ioStats)); 

					uint error = GetLastError();
					if (error != ERROR_NO_MORE_FILES)
					{
						logErrorf(L"FindNextFile failed for %ls: %ls", searchStr.c_str(), getErrorText(error).c_str());
						return -1;
					}

					if (!writeBlock()) // Flush block
						return -1;

					if (!writeBlock()) // Write empty block to tell client we're done
						return -1;
				}
				break;

			case CommandType_GetFileInfo:
				{
					auto& cmd = *(const GetFileInfoCommand*)recvBuffer;
					WIN32_FIND_DATAW fd; 
					WString fullPath = serverPath + cmd.path;
					__declspec(align(8)) u8 sendBuffer[3*8+2*4];
					static_assert(sizeof(sendBuffer) == sizeof(FileInfo) + sizeof(uint) + sizeof(uint), "");
					uint attributes = getFileInfo(*(FileInfo*)sendBuffer, fullPath.c_str(), ioStats);
					*(uint*)(sendBuffer + sizeof(FileInfo)) = attributes;
					uint error = 0;
					if (!attributes)
						error = GetLastError();
					*(uint*)(sendBuffer + sizeof(FileInfo) + sizeof(uint)) = error;

					sendData(info.socket, sendBuffer, sizeof(sendBuffer));
				}
				break;

			case CommandType_RequestReport:
				{
					u64 upTime = getTime() - m_startTime;
					uint historySize = m_database.getHistorySize();
					
					PROCESS_MEMORY_COUNTERS memCounters;
					memCounters.cb = sizeof(memCounters);
					GetProcessMemoryInfo(GetCurrentProcess(), &memCounters, sizeof(memCounters));

					u64 freeVolumeSpace = 0;
					ULARGE_INTEGER freeBytesAvailable;
					if (GetDiskFreeSpaceExW(serverPath.c_str(), nullptr, nullptr, &freeBytesAvailable))
						freeVolumeSpace = freeBytesAvailable.QuadPart;

					uint activeConnectionCount = m_activeConnectionCount - 1; // Skip the connection that is asking for this info

					// Reuse the thread buffer for report
					auto buffer = (wchar_t*)copyContext.buffers[0];
					auto bufferSize = CopyContextBufferSize;
					auto elementCount = bufferSize/2;

					StringCbPrintfW(buffer, CopyContextBufferSize,
						L"   Server v%ls  (c) Electronic Arts.  All Rights Reserved.\n"
						L"\n"
						L"   Protocol: v%u\n"
						L"   Running as: %ls\n"
						L"   Uptime: %ls\n"
						L"   Connections active: %u (handled: %u)\n"
						L"   Local file history size: %u\n"
						L"   Memory working set: %ls (Peak: %ls)\n"
						L"   Free space on volume: %ls\n"
						L"\n"
						L"   %ls copied (%ls received)\n"
						L"   %ls linked\n"
						L"   %ls skipped\n"
						, getServerVersionString().c_str(), m_protocolVersion, m_isConsole ? L"Console" : L"Service"
						, toHourMinSec(upTime).c_str()
						, activeConnectionCount, m_handledConnectionCount, historySize, toPretty(memCounters.WorkingSetSize).c_str(), toPretty(memCounters.PeakWorkingSetSize).c_str()
						, toPretty(freeVolumeSpace).c_str(), toPretty(m_bytesCopied).c_str(), toPretty(m_bytesReceived).c_str(), toPretty(m_bytesLinked).c_str(), toPretty(m_bytesSkipped).c_str());

					bool isFirst = true;
					info.log.traverseRecentErrors([&](const WString& error)
						{
							if (isFirst)
							{
								wcscat_s(buffer, elementCount, L"\n   Recent errors:\n");
								isFirst = false;
							}
							wcscat_s(buffer, elementCount, L"      ");
							wcscat_s(buffer, elementCount, error.c_str());
							wcscat_s(buffer, elementCount, L"\n");
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
				sendData(info.socket, &sendStats.compressionLevelSum, sizeof(sendStats.compressionLevelSum));
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
	if (shutdown(info.socket.socket, SD_BOTH) == SOCKET_ERROR)
	{
		logErrorf(L"shutdown failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
		return -1;
	}

	return 0;
}

bool
Server::getLocalFromNet(WString& outServerDirectory, bool& outIsExternalDirectory, const wchar_t* netDirectory)
{
	outIsExternalDirectory = false;
	outServerDirectory = netDirectory;
	const wchar_t* serverPath = wcschr(netDirectory, '\\'); // Find first backslash.. 
	if (!serverPath)
		serverPath = netDirectory + wcslen(netDirectory);

	WString netname(netDirectory, serverPath);

	PSHARE_INFO_502 shareInfo;

	NET_API_STATUS res = NetShareGetInfo(NULL, const_cast<LPWSTR>(netname.c_str()), 502, (LPBYTE*) &shareInfo);
	if (res != NERR_Success)
	{
		if (netDirectory[0] == '\\' && netDirectory[1] == '\\') // This is another network share and EACopyServer is just a proxy to that place
		{
			outIsExternalDirectory = true;
			return true;
		}

		logErrorf(L"Failed to find netshare '%ls'", netname.c_str());
		return false;
	}

	WString wpath(shareInfo->shi502_path);
	outServerDirectory = WString(wpath.begin(), wpath.end());
	if (*serverPath)
		outServerDirectory += serverPath;
	if (outServerDirectory[outServerDirectory.size()-1] != '\\')
		outServerDirectory += '\\';

	NetApiBufferFree(shareInfo);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
