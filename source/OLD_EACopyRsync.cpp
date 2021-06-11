// (c) Electronic Arts. All Rights Reserved.

#include <EACopyRsync.h>
#include "librsync/src/librsync.h"
#include "librsync/src/fileutil.h"
#include "librsync/src/util.h"
#include "librsync/src/buf.h"
#include "librsync/src/job.h"
#include "librsync/src/sumset.h"
#include <assert.h>

struct rs_filebuf {
    FILE *f;
    char *buf;
    size_t buf_len;
};

namespace eacopy
{

enum : uint { DefaultRSyncBlockLen =  4*1024 };//RS_DEFAULT_BLOCK_LEN };
enum : uint { SocketBlockSize = 512 * 1024 };

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ReadInfo
{
	const wchar_t* ident;
	RsyncStats& outStats;

	HANDLE file = INVALID_HANDLE_VALUE;
	SOCKET socket = INVALID_SOCKET;
	size_t readPosition = 0;
	uint bufferSize = 0;
	bool lastBlock = false;

	bool readFromSocket(DWORD& bytesRead, int& eof, char* buf, size_t buf_len)
	{
		u64 startReceiveMs = getTimeMs();
		while (true)
		{
			if (!bufferSize)
			{
				if (lastBlock)
				{
					eof = 1;
					break;
				}

				if (!receiveData(socket, &bufferSize, sizeof(bufferSize)))
					return false;

				if (!bufferSize)
				{
					eof = 1;
					break;
				}
				else if (bufferSize < SocketBlockSize)
					lastBlock = true;
			}

			uint toRead = min(buf_len - bytesRead, bufferSize);
			if (!receiveData(socket, buf + bytesRead, toRead))
				return false;
			bufferSize -= toRead;
			readPosition += toRead;
			bytesRead += toRead;

			if (bytesRead == buf_len)
				break;
		}
		outStats.receiveTimeMs += getTimeMs() - startReceiveMs;
		return true;
	}
};

struct WriteInfo
{
	const wchar_t* ident;
	RsyncStats& outStats;

	HANDLE file = INVALID_HANDLE_VALUE;

	SOCKET socket = INVALID_SOCKET;
	char* buffer = nullptr;
	uint bufferSize = 0;

	bool flush()
	{
		u64 startSendMs = getTimeMs();
		if (!sendData(socket, &bufferSize, sizeof(bufferSize)))
			return false;
		if (!sendData(socket, buffer, bufferSize))
			return false;
		outStats.sendSize += sizeof(bufferSize) + bufferSize;
		outStats.sendTimeMs += getTimeMs() - startSendMs;
		return true;
	}

};

/* If the stream has no more data available, read some from F into BUF, and let
   the stream use that. On return, SEEN_EOF is true if the end of file has
   passed into the stream. */
rs_result rs_infilebuf_fill2(rs_job_t *job, rs_buffers_t *buf, void *opaque)
{
    rs_filebuf_t *fb = (rs_filebuf_t *)opaque;

    /* This is only allowed if either the buf has no input buffer yet, or that
       buffer could possibly be BUF. */
    if (buf->next_in != NULL) {
        assert(buf->avail_in <= fb->buf_len);
        assert(buf->next_in >= fb->buf);
        assert(buf->next_in <= fb->buf + fb->buf_len);
    } else {
        assert(buf->avail_in == 0);
    }

	if (buf->eof_in == 1)
		return RS_DONE;

	if (buf->avail_in != 0)
		return RS_DONE;

	ReadInfo& readInfo = *(ReadInfo*)fb->f;

	DWORD bytesRead = 0;

	if (readInfo.file != INVALID_HANDLE_VALUE)
	{
		u64 startReadMs = getTimeMs();
		if (!ReadFile(readInfo.file, fb->buf, fb->buf_len, &bytesRead, NULL))
		{
			DWORD error = GetLastError();
			if (error == ERROR_HANDLE_EOF)
			{
				buf->eof_in = 1;
				return RS_DONE;
			}
			logErrorf(L"Fail reading file %s: %s", readInfo.ident, getErrorText(error).c_str());
			return RS_IO_ERROR;
		}
		readInfo.outStats.readTimeMs += getTimeMs() - startReadMs;
		readInfo.outStats.readSize += bytesRead;

		if (bytesRead == 0)
		{
			buf->eof_in = 1;
			return RS_DONE;
		}

		readInfo.readPosition += bytesRead;
	}
	else if (readInfo.socket != INVALID_SOCKET)
	{
		if (!readInfo.readFromSocket(bytesRead, buf->eof_in, fb->buf, fb->buf_len))
			return RS_IO_ERROR;
	}

	buf->avail_in = bytesRead;
    buf->next_in = fb->buf;

    job->stats.in_bytes += bytesRead;

    return RS_DONE;
}

/* The buf is already using BUF for an output buffer, and probably contains
   some buffered output now. Write this out to F, and reset the buffer cursor. */
rs_result rs_outfilebuf_drain2(rs_job_t *job, rs_buffers_t *buf, void *opaque)
{
    int present;
    rs_filebuf_t *fb = (rs_filebuf_t *)opaque;

	WriteInfo& writeInfo = *(WriteInfo*)fb->f;

    // This is only allowed if either the buf has no output buffer yet, or that
    //   buffer could possibly be BUF.
    if (buf->next_out == NULL) {
        assert(buf->avail_out == 0);

        buf->next_out = fb->buf;
        buf->avail_out = fb->buf_len;

        return RS_DONE;
    }

    assert(buf->avail_out <= fb->buf_len);
    assert(buf->next_out >= fb->buf);
    assert(buf->next_out <= fb->buf + fb->buf_len);

    present = buf->next_out - fb->buf;
    if (present > 0) {

        assert(present > 0);
		if (writeInfo.file != INVALID_HANDLE_VALUE)
		{
			u64 startWriteMs = getTimeMs();
			DWORD written;
			if (!WriteFile(writeInfo.file, fb->buf, present, &written, NULL))
			{
				logErrorf(L"Fail writing file %s: %s", writeInfo.ident, getLastErrorText().c_str());
				return RS_IO_ERROR;
			}
			present = written;
			writeInfo.outStats.writeTimeMs += getTimeMs() - startWriteMs;
		}
		else
		{
			u64 startSendMs = getTimeMs();
			size_t left = present;
			size_t readPos = 0;
			if (writeInfo.bufferSize && writeInfo.bufferSize + left >= SocketBlockSize)
			{
				size_t toCopy = SocketBlockSize - writeInfo.bufferSize;
				memcpy(writeInfo.buffer + writeInfo.bufferSize, fb->buf, toCopy);
				uint blockSize = SocketBlockSize;
				if (!sendData(writeInfo.socket, &blockSize, sizeof(blockSize)))
					return RS_IO_ERROR;
				if (!sendData(writeInfo.socket, writeInfo.buffer, blockSize))
					return RS_IO_ERROR;
				writeInfo.bufferSize = 0;
				left = present - toCopy;
				readPos += toCopy;
			}

			while (left >= SocketBlockSize)
			{
				uint blockSize = SocketBlockSize;
				if (!sendData(writeInfo.socket, &blockSize, sizeof(blockSize)))
					return RS_IO_ERROR;
				if (!sendData(writeInfo.socket, fb->buf + readPos, blockSize))
					return RS_IO_ERROR;
				left -= SocketBlockSize;
				readPos += SocketBlockSize;
			}

			memcpy(writeInfo.buffer + writeInfo.bufferSize, fb->buf + readPos, left);
			writeInfo.bufferSize += left;

			writeInfo.outStats.sendTimeMs += getTimeMs() - startSendMs;
			writeInfo.outStats.sendSize += readPos; // Not fully accurate but good enough
		}

        buf->next_out = fb->buf;
        buf->avail_out = fb->buf_len;

        job->stats.out_bytes += present;
    }

    return RS_DONE;
}

rs_result rs_file_copy_cb2(void *arg, rs_long_t pos, size_t *len, void **buf)
{
	ReadInfo& readInfo = *(ReadInfo*)arg;
	assert(readInfo.file != INVALID_HANDLE_VALUE);
	
	u64 startReadMs = getTimeMs();

	if (!setFilePosition(readInfo.ident, readInfo.file, pos))
		return RS_IO_ERROR;

	readInfo.readPosition = pos;

	DWORD bytesRead;
	if (!ReadFile(readInfo.file, *buf, *len, &bytesRead, NULL))
	{
		logErrorf(L"Fail reading file %s: %s", readInfo.ident, getLastErrorText().c_str());
		return RS_IO_ERROR;
	}
	readInfo.outStats.readTimeMs += getTimeMs() - startReadMs;
	readInfo.outStats.readSize += bytesRead;

	readInfo.readPosition += bytesRead;

	*len = bytesRead;

	return RS_DONE;
}

rs_result rs_whole_run2(rs_job_t *job, FILE *in_file, FILE *out_file,
                       int inbuflen, int outbuflen)
{
    rs_buffers_t buf;
    rs_result result;
    rs_filebuf_t *in_fb = NULL, *out_fb = NULL;

    /* Override buffer sizes if rs_inbuflen or rs_outbuflen are set. */
    inbuflen = rs_inbuflen ? rs_inbuflen : inbuflen;
    outbuflen = rs_outbuflen ? rs_outbuflen : outbuflen;
    if (in_file)
        in_fb = rs_filebuf_new(in_file, inbuflen);
    if (out_file)
        out_fb = rs_filebuf_new(out_file, outbuflen);
    result =
        rs_job_drive(job, &buf, in_fb ? rs_infilebuf_fill2 : NULL, in_fb,
                     out_fb ? rs_outfilebuf_drain2 : NULL, out_fb);
    if (in_fb)
        rs_filebuf_free(in_fb);
    if (out_fb)
        rs_filebuf_free(out_fb);
    return result;
}

rs_result rs_sig_file2(FILE *old_file, FILE *sig_file, size_t new_block_len,
                      size_t strong_len, rs_magic_number sig_magic,
                      rs_stats_t *stats)
{
    rs_job_t *job;
    rs_result r;

    job = rs_sig_begin(new_block_len, strong_len, sig_magic);
    /* Size inbuf for 4 blocks, outbuf for header + 4 blocksums. */
    r = rs_whole_run2(job, old_file, sig_file, 4 * new_block_len,
                     12 + 4 * (4 + strong_len));
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);
    rs_job_free(job);

    return r;
}

rs_result rs_delta_file2(rs_signature_t *sig, FILE *new_file, FILE *delta_file,
                        rs_stats_t *stats)
{
    rs_job_t *job;
    rs_result r;

    job = rs_delta_begin(sig);
    /* Size inbuf for 1 block, outbuf for literal cmd + 4 blocks. */
    r = rs_whole_run2(job, new_file, delta_file, sig->block_len,
                     10 + 4 * sig->block_len);
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);
    rs_job_free(job);
    return r;
}

rs_result rs_patch_file2(FILE *basis_file, FILE *delta_file, FILE *new_file,
                        rs_stats_t *stats)
{
    rs_job_t *job;
    rs_result r;

    job = rs_patch_begin(rs_file_copy_cb2, basis_file);
    /* Default size inbuf and outbuf 64K. */
    r = rs_whole_run2(job, delta_file, new_file, 64 * 1024, 64 * 1024);
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);
    rs_job_free(job);
    return r;
}

bool serverHandleRsync(SOCKET socket, const wchar_t* baseFileName, const wchar_t* newFileName, FILETIME lastWriteTime, RsyncStats& outStats)
{
    //rs_inbuflen = 512*1024;
    //rs_outbuflen = 512*1024;

	u64 startMs = getTimeMs();

	HANDLE baseFile;
	if (!openFileRead(baseFileName, baseFile, true))
		return false;
	ScopeGuard fileGuard([&] { CloseHandle(baseFile); });

	// Send signature data to client
	{

		WriteInfo writeInfo { L"RsyncSignatureMem", outStats };
		writeInfo.socket = socket;
		char buffer[SocketBlockSize];
		writeInfo.buffer = buffer;

		size_t block_len = DefaultRSyncBlockLen;
		size_t strong_len = 0;

		ReadInfo readInfo { baseFileName, outStats };
		readInfo.file = baseFile;
		FILE* basis_file = (FILE*)&readInfo;
		FILE* sig_file = (FILE*)&writeInfo;

		rs_result result = rs_sig_file2(basis_file, sig_file, block_len, strong_len, RS_BLAKE2_SIG_MAGIC, nullptr);
		if (result != RS_DONE)
			return false;

		if (!writeInfo.flush())
			return false;
	}

	// Receive delta data from client and combine with base file to create new file
	{
		if (!setFilePosition(baseFileName, baseFile, 0))
			return false;

		HANDLE newFile;
		if (!openFileWrite(newFileName, newFile, true))
			return false;
		ScopeGuard newFileGuard([&] { CloseHandle(newFile); });

		ReadInfo basisReadInfo { baseFileName, outStats };
		basisReadInfo.file = baseFile;
		ReadInfo deltaReadInfo { L"RSyncDeltaPatch", outStats };
		deltaReadInfo.socket = socket;

		WriteInfo writeInfo { newFileName, outStats };
		writeInfo.file = newFile;
		
		rs_result result = rs_patch_file2((FILE*)&basisReadInfo, (FILE*)&deltaReadInfo, (FILE*)&writeInfo, nullptr);
		if (result != RS_DONE)
			return false;

		if (!setFileLastWriteTime(newFileName, newFile, lastWriteTime))
			return false;
	}

	outStats.rsyncTimeMs = getTimeMs() - startMs - outStats.readTimeMs - outStats.receiveTimeMs - outStats.sendTimeMs - outStats.writeTimeMs;

	return true;
}

bool clientHandleRsync(SOCKET socket, const wchar_t* fileName, RsyncStats& outStats)
{
	u64 startMs = getTimeMs();

    rs_signature_t* sumset = nullptr;
	ScopeGuard cleanSumset([&]() { rs_free_sumset(sumset); });

	{
		ReadInfo readInfo { L"RsyncSignature", outStats };
		readInfo.socket = socket;

		rs_job_t* job = rs_loadsig_begin(&sumset);

		// We read 0 bytes which will read first blocks size (which is very likely that it is the entire file)
		DWORD bytesRead = 0;
		int eof;
		if (!readInfo.readFromSocket(bytesRead, eof, nullptr, 0))
			return false;

		job->sig_fsize = readInfo.bufferSize < SocketBlockSize ? readInfo.bufferSize : 0; // If bufferSize is more than SocketBlockSize we don't know how big the signature data is

		// Size inbuf for 1024x 16 byte blocksums.
		rs_result result = rs_whole_run2(job, (FILE*)&readInfo, NULL, 1024 * 16, 0);
		rs_job_free(job);

		if (result != RS_DONE)
		{
			logErrorf(L"Rsync failed while receiving signature data");
			return false;
		}
	}
	
	{
		rs_result result = rs_build_hash_table(sumset);
		if (result != RS_DONE)
		{
			logErrorf(L"Rsync failed while building hashtable");
			return false;
		}
	}

	{
		HANDLE newFileHandle;
		if (!openFileRead(fileName, newFileHandle, true))
			return false;
		ScopeGuard fileGuard([&] { CloseHandle(newFileHandle); });
		ReadInfo readInfo = { fileName, outStats };
		readInfo.file = newFileHandle;
		WriteInfo writeInfo = { L"RSyncDeltaPatchMemory", outStats };
		writeInfo.socket = socket;
		char buffer[SocketBlockSize];
		writeInfo.buffer = buffer;

		u64 startMs = getTimeMs();
		rs_result result = rs_delta_file2(sumset, (FILE*)&readInfo, (FILE*)&writeInfo, nullptr);
		logInfof(L"BUILD DELTA: %s\r\n", toHourMinSec(getTimeMs() - startMs).c_str());

		if (!writeInfo.flush())
			return false;
	}

	outStats.rsyncTimeMs = getTimeMs() - startMs - outStats.readTimeMs - outStats.receiveTimeMs - outStats.sendTimeMs - outStats.writeTimeMs;

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

}
