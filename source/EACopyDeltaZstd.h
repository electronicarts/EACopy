// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY)

#include <EACopyDelta.h>
#define ZSTD_STATIC_LINKING_ONLY
#include "../external/zstd/lib/zstd.h"
#include <winsock2.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool zstdCode(bool encode, Socket& socket, const wchar_t* referenceFileName, FileHandle referenceFile, u64 referenceFileSize, const wchar_t* newFileName, FileHandle newFile, u64 newFileSize, NetworkCopyContext& copyContext, IOStats& ioStats, u64& socketTime, u64& socketSize, u64& codeTime)
{
	enum { CompressedNetworkTransferChunkSize = NetworkTransferChunkSize / 4 };
	enum { CompressBoundReservation = 32 * 1024 };


	u8* copyContextData = copyContext.buffers[0]; // All buffers are allocated in the same allocation.. so if we use less we can just linearly use them

	u8* inBuffer;
	u8* outBuffer;
	u8* referenceBuffer;

	if (encode)
	{
		inBuffer = copyContextData; // In-buffer for file
		outBuffer = inBuffer + CompressedNetworkTransferChunkSize - CompressBoundReservation; // Out-buffer to socket
		referenceBuffer = outBuffer + CompressedNetworkTransferChunkSize; // Buffer for reference file
	}
	else
	{
		inBuffer = copyContextData; // In-buffer for socket
		outBuffer = inBuffer + CompressedNetworkTransferChunkSize; // Out-buffer for destination file
		referenceBuffer = outBuffer + CopyContextBufferSize; // Buffer for reference file
	}

	bool freeReferenceMemory = false;
	void* referenceMemory = referenceBuffer;

	size_t referenceBufferSize = CopyContextBufferSize * 3 - (referenceBuffer - copyContextData);
	if (referenceFileSize > referenceBufferSize)
	{
		referenceMemory = new u8[referenceFileSize];
		freeReferenceMemory = true;
	}
	ScopeGuard _([&]() { if (freeReferenceMemory) delete[] referenceMemory; });

	u64 referenceFileRead;
	if (!readFile(referenceFileName, referenceFile, referenceMemory, referenceFileSize, referenceFileRead, ioStats))
		return false;

	if (referenceFileRead != referenceFileSize)
	{
		logErrorf(L"Failed to read entire ref file %ls", referenceFileName);
		return false;
	}

	ZSTD_outBuffer buffOut;
	ZSTD_inBuffer buffIn;

	if (encode)
	{
		if (!copyContext.compContext)
			copyContext.compContext = ZSTD_createCCtx();
		auto cctx = (ZSTD_CCtx*)copyContext.compContext;
		ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);

		bool useBufferedIO = true;
		size_t res = ZSTD_CCtx_refPrefix(cctx, referenceMemory, referenceFileSize);
		if (ZSTD_isError(res))
		{
			logErrorf(L"Fail setting up ref file %ls when compressing %ls: %hs", referenceFileName, newFileName, ZSTD_getErrorName(res));
			return false;
		}

		ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);  /* always enable content size when available (note: supposed to be default) */
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, ZSTD_WINDOWLOG_MAX);
		ZSTD_CCtx_setPledgedSrcSize(cctx, newFileSize);
		ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableDedicatedDictSearch, 1);
		//ZSTD_CCtx_setParameter(cctx, ZSTD_c_literalCompressionMode, ZSTD_lcm_auto);

		u64 left = newFileSize;
		while (left)
		{
			// Make sure the amount of data we've read fit in the destination compressed buffer
			static_assert(ZSTD_COMPRESSBOUND(CompressedNetworkTransferChunkSize - CompressBoundReservation) <= CompressedNetworkTransferChunkSize - 4, "");

			uint toRead = min(left, u64(CompressedNetworkTransferChunkSize - CompressBoundReservation));
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);
			u64 read;
			if (!readFile(newFileName, newFile, inBuffer, toReadAligned, read, ioStats))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %ls: %ls", newFileName, getLastErrorText().c_str());
					return false;
				}
			}
			left -= read;

			// Use the first 4 bytes to write size of buffer.. can probably be replaced with zstd header instead
			u8* destBuf = outBuffer;
			u64 startCompressTime = getTime();
			buffOut.dst = destBuf + 4;
			buffOut.size = CompressedNetworkTransferChunkSize - 4;
			buffOut.pos = 0;
			buffIn.src = inBuffer;
			buffIn.size = read;
			buffIn.pos = 0;

			ZSTD_EndDirective directive = ZSTD_e_continue;
			if (left == 0)
				directive = ZSTD_e_end;

			res = ZSTD_compressStream2(cctx, &buffOut, &buffIn, directive);
			if (ZSTD_isError(res))
			{
				logErrorf(L"Fail compressing file %ls: %hs", newFileName, ZSTD_getErrorName(res));
				return false;
			}

			if (buffIn.pos != buffIn.size)
			{
				logErrorf(L"ERROR.. code path not implemented since I'm not sure if this can happen.. report this to if experienced!!");
				return false;
			}

			size_t compressedSize = buffOut.pos;
			if (compressedSize == 0)
				continue;

			u64 compressTime = getTime() - startCompressTime;

			*(uint*)destBuf = uint(compressedSize);

			uint sendBytes = compressedSize + 4;
			u64 startSendTime = getTime();
			if (!sendData(socket, destBuf, sendBytes))
				return false;
			u64 sendTime = getTime() - startSendTime;

		}

		// Send last size 0 entry to tell client we're done
		uint terminator = 0;
		if (!sendData(socket, &terminator, sizeof(terminator)))
			return false;
	}
	else
	{
		if (!copyContext.decompContext)
			copyContext.decompContext = ZSTD_createDCtx();
		auto dctx = (ZSTD_DCtx*)copyContext.decompContext;
		ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
		ZSTD_DCtx_setMaxWindowSize(dctx, newFileSize);
		size_t res = ZSTD_DCtx_refPrefix(dctx, referenceMemory, referenceFileSize);
		if (ZSTD_isError(res))
		{
			logErrorf(L"Zstd error %ls: %hs", referenceFileName, ZSTD_getErrorName(res));
			return false;
		}

		bool outSuccess = true;

		u64 read = 0;

		u64 totalReceivedSize = 0;

		while (true)
		{
			u64 startRecvTime = getTime();

			uint compressedSize;
			if (!receiveData(socket, &compressedSize, sizeof(uint)))
				return false;

			totalReceivedSize += compressedSize + sizeof(uint);

			if (compressedSize == 0) // We're done
				break;

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
				wsabuf.buf = (char*)inBuffer;
				uint recvBytes = 0;
				uint flags = MSG_WAITALL;
				int fileRes = WSARecv(socket.socket, &wsabuf, 1, &recvBytes, &flags, NULL, NULL);
				if (fileRes != 0)
				{
					logErrorf(L"recv failed with error: %ls", getErrorText(getLastNetworkError()).c_str());
					return false;
				}
				if (recvBytes == 0)
				{
					logErrorf(L"Socket closed before full file has been received (%ls)", newFileName);
					return false;
				}

				socketSize += recvBytes;
				toRead -= recvBytes;
			}
			socketTime += getTime() - startRecvTime;


			size_t decompressedSize = 0;

			if (outSuccess)
			{
				u64 startDecompressTime = getTime();

				buffOut.dst = outBuffer;
				buffOut.size = CopyContextBufferSize;
				buffOut.pos = 0;
				buffIn.src = inBuffer;
				buffIn.size = compressedSize;
				buffIn.pos = 0;
				res = ZSTD_decompressStream(dctx, &buffOut, &buffIn);
				if (ZSTD_isError(res))
				{
					logErrorf(L"Decompression error while decompressing %u bytes after reading %llu, for file %ls: %hs", compressedSize, read, newFileName, ZSTD_getErrorName(res));
					// Don't return false since we can still continue copying other files after getting this error
					outSuccess = false;
				}

				if (buffIn.pos != buffIn.size)
				{
					logErrorf(L"ERROR.. code path not implemented since I'm not sure if this can happen.. report this to if experienced!!");
					outSuccess = false;
				}
				decompressedSize = buffOut.pos;
				codeTime += getTime() - startDecompressTime;
			}


			outSuccess = outSuccess && writeFile(newFileName, newFile, outBuffer, decompressedSize, ioStats);

			read += decompressedSize;
		}
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
