// (c) Electronic Arts. All Rights Reserved.

#if defined(EACOPY_ALLOW_DELTA_COPY)

#include <EACopyDelta.h>
#include <xdelta3.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define XD3_INVALID_OFFSET (~uintptr_t(0))
#define MAX_LRU_SIZE 32u
#define SOURCEWINSIZE (128 * 1024 * 1024)

struct xDeltaLruBlock
{
	u8* blk;
	xoff_t blkno;
	usize_t size;

	xDeltaLruBlock* next;
	xDeltaLruBlock* prev;
};

struct xDeltaGetBlockData
{
	IOStats& ioStats;
	const wchar_t* referenceFileName;
	FileHandle& referenceFile;
	u64 referenceFileSize;
	u64 referenceFilePos = 0;

	// LRU work data
	xDeltaLruBlock* lru;
	uint lruSize = 0u;
	u8* lruData;
	xDeltaLruBlock list;
	bool fifo;

	void* fileData = nullptr;
};

void remove(xDeltaLruBlock& list, xDeltaLruBlock* block)
{
	block->next->prev = block->prev;
	block->prev->next = block->next;
	block->next = nullptr;
	block->prev = nullptr;
}

void pushBack(xDeltaLruBlock& list, xDeltaLruBlock* block)
{
	block->prev = list.prev;
	block->next = &list;
	list.prev->next = block;
	list.prev = block;
}

xDeltaLruBlock* popFront(xDeltaLruBlock& list)
{
	xDeltaLruBlock* front = list.next;
	front->next->prev = &list;
	list.next = front->next;
	front->next = nullptr;
	front->prev = nullptr;
	return front;
}

bool empty(xDeltaLruBlock& list)
{
	return list.next != &list;
}

int xDeltaGetLruBlock(xDeltaGetBlockData& data, xoff_t blkno, xDeltaLruBlock*& blrup, bool& isNew)
{
	xDeltaLruBlock* blru = NULL;

	isNew = false;

	if (data.fifo)
	{
		int idx = blkno % data.lruSize;
		blru = &data.lru[idx];
		if (blru->blkno == blkno)
		{
			blrup = blru;
			return 0;
		}
		if (blru->blkno != XD3_INVALID_OFFSET && blru->blkno > blkno)
			return XD3_TOOFARBACK;
	}
	else
	{
		for (uint i = 0; i < data.lruSize; i += 1)
		{
			blru = &data.lru[i];
			if (blru->blkno == blkno)
			{
				remove(data.list, blru);
				pushBack(data.list, blru);
				blrup = blru;
				return 0;
			}
		}
	}

	if (data.fifo)
	{
		int idx = blkno % data.lruSize;
		blru = &data.lru[idx];
	}
	else
	{
		XD3_ASSERT(!empty(data.list));
		blru = popFront(data.list);
		pushBack(data.list, blru);
	}

	isNew = true;
	blrup = blru;
	blru->blkno = XD3_INVALID_OFFSET;
	return 0;
}


int xDeltaGetBlock(xd3_stream* stream, xd3_source* source, xoff_t blkno)
{
	auto& data = *(xDeltaGetBlockData*)stream->opaque;

	int ret = 0;
	xDeltaLruBlock* blru;
	bool isNew;

	if ((ret = xDeltaGetLruBlock(data, blkno, blru, isNew)))
		return ret;

	if (!isNew)
	{
		source->curblkno = blkno;
		source->onblk = blru->size;
		source->curblk = blru->blk;
		return 0;
	}

	xoff_t pos = blkno * source->blksize;

	if (pos != data.referenceFilePos)
	{
		if (!setFilePosition(data.referenceFileName, data.referenceFile, pos, data.ioStats))
			return 1;
		data.referenceFilePos = pos;
	}

	u64 nread;
	if (!readFile(data.referenceFileName, data.referenceFile, (void*)blru->blk, source->blksize, nread, data.ioStats))
		return 1;
	data.referenceFilePos += nread;

	source->curblk = blru->blk;
	source->curblkno = blkno;
	source->onblk = nread;
	blru->size = nread;
	blru->blkno = blkno;

	return 0;
}

bool xDeltaInitBlockData(xDeltaGetBlockData& data, xd3_stream& stream, xd3_source& source, xoff_t blkno)
{
	data.list.prev = &data.list;
	data.list.next = &data.list;

	u64 lruSize = MAX_LRU_SIZE * sizeof(xDeltaLruBlock);
	data.lru = new xDeltaLruBlock[lruSize];
	memset(data.lru, 0, lruSize);
	data.lru[0].blk = data.lruData = new u8[SOURCEWINSIZE];
	data.lru[0].blkno = XD3_INVALID_OFFSET;
	data.lruSize = 1;
	pushBack(data.list, &data.lru[0]);
	usize_t blksize = SOURCEWINSIZE;

	source.blksize = blksize;
	source.curblkno = XD3_INVALID_OFFSET;
	source.curblk = NULL;
	source.max_winsize = SOURCEWINSIZE;

	if (xDeltaGetBlock(&stream, &source, blkno) != 0)
		return false;

	if (source.onblk < blksize)
		source.onlastblk = data.referenceFileSize;

	if (data.referenceFileSize > SOURCEWINSIZE)
	{
		blksize = SOURCEWINSIZE / MAX_LRU_SIZE;
		source.blksize = blksize;
		source.onblk = blksize;
		source.onlastblk = blksize;
		source.max_blkno = MAX_LRU_SIZE - 1;

		data.lru[0].size = blksize;
		data.lruSize = MAX_LRU_SIZE;

		for (uint i = 1; i < data.lruSize; i += 1)
		{
			data.lru[i].blk = data.lru[0].blk + (blksize * i);
			data.lru[i].blkno = i;
			data.lru[i].size = blksize;
			pushBack(data.list, &data.lru[i]);
		}
	}

	return true;
}

void xDeltaDestroyBlockData(xDeltaGetBlockData& data)
{
	delete[] data.lruData;
	delete[] data.lru;
}

bool xDeltaCode(bool encode, Socket& socket, const wchar_t* referenceFileName, FileHandle referenceFile, u64 referenceFileSize, const wchar_t* newFileName, FileHandle newFile, u64 newFileSize, NetworkCopyContext& copyContext, IOStats& ioStats, u64& socketTime, u64& socketSize, u64& codeTime)
{
	uint flags = 0;
	//flags |= XD3_ADLER32;
	//flags |= XD3_COMPLEVEL_1;
	flags |= XD3_COMPLEVEL_3;
	//flags |= XD3_COMPLEVEL_9;
	flags |= XD3_SEC_LZMA;

	xDeltaGetBlockData blockData{ ioStats, referenceFileName, referenceFile, referenceFileSize };
	blockData.fifo = encode;

	xd3_config config;
	xd3_init_config(&config, flags);
	config.winsize = min(referenceFileSize, XD3_DEFAULT_WINSIZE);
	config.getblk = xDeltaGetBlock;
	config.opaque = &blockData;
	config.iopt_size = XD3_DEFAULT_IOPT_SIZE;
	config.sprevsz = XD3_DEFAULT_SPREVSZ;
	config.smatch_cfg = XD3_SMATCH_FAST;

	xd3_stream stream;
	memset(&stream, 0, sizeof(stream));
	xd3_config_stream(&stream, &config);

	xd3_source source;
	memset(&source, 0, sizeof(source));
	if (!xDeltaInitBlockData(blockData, stream, source, 0))
		return false;
	ScopeGuard _bd([&]() { xDeltaDestroyBlockData(blockData); });
	xd3_set_source_and_size(&stream, &source, referenceFileSize);

	static_assert(XD3_ALLOCSIZE <= CopyContextBufferSize, "");
	u64 bufSize = XD3_ALLOCSIZE;// CopyContextBufferSize;

	u8* inputBuf = bufSize <= CopyContextBufferSize ? copyContext.buffers[1] : (u8*)malloc(bufSize);
	u64 inputBufRead = 0;
	u8* outputBuf = bufSize <= CopyContextBufferSize ? copyContext.buffers[2] : (u8*)malloc(bufSize);
	u64 outputBufWritten = 0;
	ScopeGuard _([&]()
		{
			xd3_close_stream(&stream);
			xd3_free_stream(&stream);

			if (bufSize > CopyContextBufferSize)
			{
				free(inputBuf);
				free(outputBuf);
			}
		});

	bool outSuccess = true;
	do
	{
		if (encode)
		{
			if (!readFile(newFileName, newFile, (void*)inputBuf, bufSize, inputBufRead, ioStats))
				return false;
		}
		else
		{
			TimerScope _(socketTime);
			if (!receiveData(socket, &inputBufRead, sizeof(inputBufRead)))
				return false;
			if (inputBufRead)
				if (!receiveData(socket, (void*)inputBuf, inputBufRead))
					return false;
			socketSize += sizeof(inputBufRead) + inputBufRead;
		}

		if (!outSuccess)
			continue;

		if (inputBufRead < bufSize)
			xd3_set_flags(&stream, XD3_FLUSH | stream.flags);
		xd3_avail_input(&stream, inputBuf, inputBufRead);

	process:

		int ret = 0;
		{
			TimerScope _(codeTime);
			if (encode)
				ret = xd3_encode_input(&stream);
			else
				ret = xd3_decode_input(&stream);
		}

		switch (ret)
		{
		case XD3_INPUT:
			continue;

		case XD3_OUTPUT:
			if (encode)
			{
				uint leftInBuffer = bufSize - outputBufWritten;
				uint writeToBufferSize = stream.avail_out < leftInBuffer ? stream.avail_out : leftInBuffer;
				memcpy(outputBuf + outputBufWritten, stream.next_out, writeToBufferSize);
				outputBufWritten += writeToBufferSize;
				if (outputBufWritten == bufSize)
				{
					if (!sendData(socket, &outputBufWritten, sizeof(outputBufWritten)))
						return false;
					if (!sendData(socket, outputBuf, outputBufWritten))
						return false;
					uint left = stream.avail_out - writeToBufferSize;
					memcpy(outputBuf, stream.next_out + writeToBufferSize, left);
					outputBufWritten = left;
				}

			}
			else
			{
				outSuccess = outSuccess && writeFile(newFileName, newFile, stream.next_out, stream.avail_out, ioStats);
			}
			xd3_consume_output(&stream);
			goto process;

		case XD3_GOTHEADER:
			goto process;

		case XD3_WINSTART:
			goto process;

		case XD3_WINFINISH:
			goto process;

		default:
			logErrorf(L"Xdelta failed %ls file %ls: %hs %d !!!\n", (encode ? L"compressing" : L"decompressing"), newFileName, stream.msg, ret);
			outSuccess = false;
		}

	} while (inputBufRead == bufSize);

	if (encode)
	{
		if (!sendData(socket, &outputBufWritten, sizeof(outputBufWritten)))
			return false;
		if (outputBufWritten)
			if (!sendData(socket, outputBuf, outputBufWritten))
				return false;
	}

	return outSuccess;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}

#endif
