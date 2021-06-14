// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)

#include <EACopyDelta.h>
#define ZSTD_STATIC_LINKING_ONLY
#include "../external/zstd/lib/zstd.h"
#include <winsock2.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool zstdCode(bool encode, Socket& socket, const wchar_t* referenceFileName, FileHandle referenceFile, u64 referenceFileSize, const wchar_t* newFileName, FileHandle newFile, u64 newFileSize, CopyContext& copyContext_, IOStats& ioStats, u64& socketTime, u64& socketSize)
{
	enum { CompressedNetworkTransferChunkSize = NetworkTransferChunkSize / 4 };

	void* referenceMemory = malloc(referenceFileSize);
	u64 nread;
	if (!readFile(referenceFileName, referenceFile, referenceMemory, referenceFileSize, nread, ioStats))
		return false;

	ZSTD_outBuffer buffOut;
	ZSTD_inBuffer buffIn;

	auto& copyContext = (NetworkCopyContext&)copyContext_;
	if (encode)
	{
		CompressionStats compressionStats;
		compressionStats.level = 3;
		CompressionData	compressionData{ compressionStats };
		if (!compressionData.context)
			compressionData.context = ZSTD_createCCtx();
		auto cctx = (ZSTD_CCtx*)compressionData.context;
		bool useBufferedIO = true;
		if (ZSTD_CCtx_refPrefix(cctx, referenceMemory, referenceFileSize) != 0)
			return false;
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
			enum { CompressBoundReservation = 32 * 1024 };
			// Make sure the amount of data we've read fit in the destination compressed buffer
			static_assert(ZSTD_COMPRESSBOUND(CompressedNetworkTransferChunkSize - CompressBoundReservation) <= CompressedNetworkTransferChunkSize - 4, "");

			uint toRead = min(left, u64(CompressedNetworkTransferChunkSize - CompressBoundReservation));
			uint toReadAligned = useBufferedIO ? toRead : (((toRead + 4095) / 4096) * 4096);
			u64 read;
			if (!readFile(newFileName, newFile, copyContext.buffers[0], toReadAligned, read, ioStats))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					logErrorf(L"Fail reading file %ls: %ls", newFileName, getLastErrorText().c_str());
					return false;
				}
			}
			left -= read;

			CompressionStats& cs = compressionData.compressionStats;

			// Use the first 4 bytes to write size of buffer.. can probably be replaced with zstd header instead
			u8* destBuf = copyContext.buffers[1];
			u64 startCompressTime = getTime();
			buffOut.dst = destBuf + 4;
			buffOut.size = CompressedNetworkTransferChunkSize - 4;
			buffOut.pos = 0;
			buffIn.src = copyContext.buffers[0];
			buffIn.size = read;
			buffIn.pos = 0;

			ZSTD_EndDirective directive = ZSTD_e_continue;
			if (left == 0)
				directive = ZSTD_e_end;

			size_t compressedSize = ZSTD_compressStream2(cctx, &buffOut, &buffIn, directive);

			if (buffIn.pos != buffIn.size)
			{
				logErrorf(L"ERROR!!");
				return false;
			}

			compressedSize = buffOut.pos;
			if (compressedSize == 0)
				continue;

			if (ZSTD_isError(compressedSize))
			{
				logErrorf(L"Fail compressing file %ls: %ls", newFileName, ZSTD_getErrorName(compressedSize));
				return false;
			}
			u64 compressTime = getTime() - startCompressTime;

			*(uint*)destBuf = uint(compressedSize);

			uint sendBytes = compressedSize + 4;
			u64 startSendTime = getTime();
			if (!sendData(socket, destBuf, sendBytes))
				return false;
			u64 sendTime = getTime() - startSendTime;

		}
	}
	else
	{
		if (!copyContext.compContext)
			copyContext.compContext = ZSTD_createDCtx();
		auto dctx = (ZSTD_DCtx*)copyContext.compContext;
		ZSTD_DCtx_setMaxWindowSize(dctx, newFileSize);
		if (ZSTD_DCtx_refPrefix(dctx, referenceMemory, referenceFileSize) != 0)
			return false;

		bool outSuccess = true;

		u64 read = 0;

		// Copy the stuff already in the buffer
		/*
		if (recvPos > commandSize)
		{
			u64 toCopy = std::min(u64(recvPos - commandSize), u64(fileSize));
			outSuccess = outSuccess & writeFile(newFileName, newFile, recvBuffer + commandSize, toCopy, ioStats);
			read = toCopy;
			commandSize += (uint)toCopy;
		}
		*/

		int fileBufIndex = 0;
		u64 totalReceivedSize = 0;

		while (read != newFileSize)
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

				socketTime += getTime() - startRecvTime; 
				socketSize += recvBytes;

				//recvStats.recvTime += getTime() - startRecvTime;
				//recvStats.recvSize += recvBytes;
				toRead -= recvBytes;
			}

			u64 startDecompressTime = getTime();

			buffOut.dst = copyContext.buffers[fileBufIndex];
			buffOut.size = CopyContextBufferSize;
			buffOut.pos = 0;
			buffIn.src = copyContext.buffers[2];
			buffIn.size = compressedSize;
			buffIn.pos = 0;
			size_t res = ZSTD_decompressStream(dctx, &buffOut, &buffIn);
			if (ZSTD_isError(res))
			{
				outSuccess = false;
				logErrorf(L"Decompression error while decompressing %u bytes after reading %llu, for file %ls: %hs", compressedSize, read, newFileName, ZSTD_getErrorName(res));
				// Don't return false since we can still continue copying other files after getting this error
			}

			if (buffIn.pos != buffIn.size)
			{
				logErrorf(L"ERROR!!");
				return false;
			}

			size_t decompressedSize = buffOut.pos;

			if (outSuccess)
			{
				outSuccess &= ZSTD_isError(decompressedSize) == 0;
				if (!outSuccess)
				{
					logErrorf(L"Decompression error while decompressing %u bytes after reading %llu, for file %ls: %hs", compressedSize, read, newFileName, ZSTD_getErrorName(decompressedSize));
					// Don't return false since we can still continue copying other files after getting this error
				}
			}

			//recvStats.decompressTime = getTime() - startDecompressTime;

			outSuccess = outSuccess && writeFile(newFileName, newFile, copyContext.buffers[fileBufIndex], decompressedSize, ioStats);

			read += decompressedSize;
			fileBufIndex = fileBufIndex == 0 ? 1 : 0;
		}
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
