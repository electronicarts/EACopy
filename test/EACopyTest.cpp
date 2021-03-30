// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"
#include <assert.h>
#include <utility>
#if defined(_WIN32)
#include "EACopyServer.h"
#define NOMINMAX
#include <shlwapi.h>
#include <strsafe.h>
#include <Shlobj.h>
#else
#include <algorithm>
#include <limits.h>
#include <unistd.h>
#endif

namespace eacopy
{

#if defined(NDEBUG)
#define EACOPY_ASSERT(x) if (!(x)) { logInfoLinef(L"ASSERT! " #x "\r\n"); Sleep(2000); fflush(stdout); *(int*)nullptr = 0xdeadbeef; }
#else
	#define EACOPY_ASSERT(x) if (!(x)) { Sleep(1000); fflush(stdout); assert(false); }
#endif

#if defined(_WIN32)
//Set these Variables to run EACopyTest without having to specify the source/dest input parameters.
//Examples are detailed:
// L"C:\\temp\\EACopyTest\\source" OR L"I:\\MyLocalDrive"
// This directory should exist.
#define DEFAULT_SOURCE_DIR  L"C:\\temp\\EACopyTest\\source"
// L"\\\\localhost\\EACopyTest\\dest" OR L"\\\\localhost\\MyShare"
// Locally configured share to C:\temp\EACopyTest\dest OR I:\MyShare
//The real directory should exist and the share setup to point to the directory.
#define DEFAULT_DEST_DIR  L"\\\\localhost\\EACopyTest\\dest"
// Some network share on another machine than where the EACopyService run
#define DEFAULT_EXTERNAL_DEST_DIR L""
#else
#define DEFAULT_SOURCE_DIR  L"EACopyTest/source"
#define DEFAULT_DEST_DIR  L"EACopyTest/dest"
#define DEFAULT_EXTERNAL_DEST_DIR L""
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Source directory used by unit tests.. Should be a absolute path on one of the local drives
WString g_testSourceDir = DEFAULT_SOURCE_DIR;

// Destination directory used by unit tests. Should be a network share on the local machine.
WString g_testDestDir = DEFAULT_DEST_DIR;

// Destination directory used by unit tests. Should be a network share on the local machine.
WString g_testExternalDestDir = DEFAULT_EXTERNAL_DEST_DIR;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TestServer
// Wrapper of the Server with execution happening on another thread to be able to run EAClient on main thread during testing

#if defined(_WIN32)
class TestServer
{
public:
	TestServer(const ServerSettings& settings, Log& log, uint protocolVersion = ProtocolVersion)
	:	m_settings(settings)
	,	m_log(log)
	,	m_server(protocolVersion)
	,	m_serverThread([this]() { threadFunc(); return 0; })
	{
	}

	~TestServer()
	{
		m_server.stop();
	}

	// Will wait until server thread is ready (internal threads etc are ready to go)
	void waitReady()
	{
		while (!m_isServerReady)
		{
			EACOPY_ASSERT(!m_threadExited);
			Sleep(1);
		}
	}

	bool primeDirectory(const wchar_t* directory)
	{
		return m_server.primeDirectory(directory);
	}

private:
	int threadFunc()
	{
		m_threadStarted = true;
		m_server.start(m_settings, m_log, false, [this](uint dwCurrentState, uint dwWin32ExitCode, uint dwWaitHint) -> BOOL { m_isServerReady = dwCurrentState == SERVICE_RUNNING; return TRUE; });
		m_threadExited = true;
		return 0;
	}

	const ServerSettings& m_settings;
	Log& m_log;
	Server m_server;
	bool m_threadStarted = false;
	bool m_threadExited = false;
	bool m_isServerReady = false;
	Thread m_serverThread;
};
#else
constexpr wchar_t ServerVersion[] = "NOSERVER";
#define MAX_PATH 260
#endif


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TestBase - base class for all tests created below. Sets up test environment before each test

struct TestBase
{
	TestBase(const wchar_t* testName, bool isTemp) : name(testName)
	{
		if (isTemp)
			return;

		if (!s_lastTest)
			s_firstTest = this;
		else
			s_lastTest->m_nextTest = this;
		s_lastTest = this;
	}

	void run(uint& testIndex, int count)
	{
		for (uint loopIndex=0; loopIndex!=runCount(); ++loopIndex)
		{
			++testIndex;
			testSourceDir = g_testSourceDir + L'\\' + name + L'\\';
			testDestDir = g_testDestDir + L'\\' + name + L'\\';

			bool log = false;
			if (log)
			{
				clientLog.init((name + L"_ClientLog.txt").c_str(), true, false);
				serverLog.init((name + L"_ServerLog.txt").c_str(), true, false);
			}
			wprintf(L"Running test %2u/%u '%ls'...", testIndex, count, name.c_str());
			fflush(stdout);
			IOStats stats;
			EACOPY_ASSERT(deleteDirectory(testSourceDir.c_str(), stats));
			EACOPY_ASSERT(deleteDirectory(testDestDir.c_str(), stats));
			EACOPY_ASSERT(ensureDirectory(testSourceDir.c_str()));
			EACOPY_ASSERT(ensureDirectory(testDestDir.c_str()));
			m_setupTime = 0;
			u64 startTime = getTime();
			runImpl(loopIndex);
			u64 endTime = getTime();
			if (!skipped)
			{
				wprintf(L"Done (%ls)\n", toHourMinSec(endTime - startTime - m_setupTime).c_str());
				++s_successCount;
			}
			else
			{
				++s_skipCount;
			}
			EACOPY_ASSERT(deleteDirectory(testSourceDir.c_str(), stats));
			EACOPY_ASSERT(deleteDirectory(testDestDir.c_str(), stats));
			if (log)
			{
				serverLog.deinit();
				clientLog.deinit();
			}
		}
	}

	static void runAll()
	{
		uint testCount = 0;
		for (TestBase* it=s_firstTest; it; it = it->m_nextTest)
			testCount += it->runCount();

		uint testIt = 0;
		for (TestBase* it=s_firstTest; it; it = it->m_nextTest)
			it->run(testIt, testCount);
	}

	virtual void runImpl(uint loopIndex) = 0;
	virtual uint runCount() = 0;

	ClientSettings getDefaultClientSettings(const wchar_t* wildcard = L"*.*")
	{
		ClientSettings settings;
		settings.sourceDirectory = testSourceDir;
		settings.destDirectory = testDestDir;
		settings.useServer = UseServer_Disabled;
		settings.retryCount = 0;
		if (wildcard)
			settings.filesOrWildcards.push_back(wildcard);
		return settings;
	}

	ServerSettings getDefaultServerSettings()
	{
		ServerSettings settings;
		settings.useSecurityFile = false;
		return settings;
	}

	void createTestFile(const wchar_t* name, u64 size, bool source = true)
	{
		const WString& dir = source ? testSourceDir : testDestDir;
		createTestFile(dir.c_str(), name, size);
	}
	void createTestFile(const wchar_t* dir_, const wchar_t* name, u64 size, bool source = true)
	{
		u64 startSetupTime = getTime();

		WString dir(dir_);
		const wchar_t* lastSlash = wcsrchr(name, L'\\');
		if (lastSlash)
		{
			dir.append(name, lastSlash - name);
			EACOPY_ASSERT(ensureDirectory(dir.c_str()));
			dir += L'\\';
			name = lastSlash + 1;
		}

		WString tempBuffer;
		WString fullFilename = dir + name;
		const wchar_t* fullFileName = convertToShortPath(fullFilename.c_str(), tempBuffer);

		u64 dataSize = std::min(size, 1024*1024*256ull);
		char* data = new char[size];
		for (u64 i=0; i!=dataSize; ++i)
			data[i] = 'a' + i % 26;

		FileInfo fileInfo;
		fileInfo.fileSize = size;
		EACOPY_ASSERT(createFile(fullFileName, fileInfo, data, ioStats, true));

		delete[] data;

		m_setupTime += getTime() - startSetupTime;
	}

	bool getGeneralFileExists(const wchar_t* fullFilePath)
	{
		#if defined(_WIN32)
		return PathFileExistsW(fullFilePath) == TRUE;
		#else
		auto str = toString(fullFilePath);
		std::replace(str.begin(), str.end(), '\\', '/');
		return access(str.c_str(), F_OK) == 0;
		#endif	
	}

	bool getTestFileExists(const wchar_t* name, bool source = false)
	{
		WString str = source ? testSourceDir : testDestDir;
		str.append(L"\\").append(name);
		#if defined(_WIN32)
		return PathFileExistsW(str.c_str()) == TRUE;
		#else
		auto str2 = toString(str.c_str());
		std::replace(str2.begin(), str2.end(), '\\', '/');
		return access(str2.c_str(), F_OK) == 0;
		#endif	
	}

	uint getFileInfo(FileInfo& outInfo, const wchar_t* fullFileName)
	{
		return eacopy::getFileInfo(outInfo, fullFileName, ioStats);
	}

	bool ensureDirectory(const wchar_t* directory)
	{
		return eacopy::ensureDirectory(directory, ioStats);
	}

	bool isEqual(const wchar_t* fileA, const wchar_t* fileB)
	{
		FileInfo a;
		FileInfo b;
		uint attrA = getFileInfo(a, fileA);
		uint attrB = getFileInfo(b, fileB);
		if (!attrA || attrA != attrB)
			return false;
		return equals(a, b);
	}

	bool isSourceEqualDest(const wchar_t* file)
	{
		return isEqual((testSourceDir + file).c_str(), (testDestDir + file).c_str());
	}

	void createFileList(const wchar_t* name, const char* fileOrWildcard, bool source = true)
	{
		WString dir = source ? testSourceDir : testDestDir;
		WString fileName = dir + L'\\' + name;
		FileHandle fileHandle;
		(void)fileHandle; // suppress unused variable warning

		EACOPY_ASSERT(openFileWrite(fileName.c_str(), fileHandle, ioStats, true));
		EACOPY_ASSERT(writeFile(fileName.c_str(), fileHandle, fileOrWildcard, strlen(fileOrWildcard), ioStats));
		EACOPY_ASSERT(closeFile(fileName.c_str(), fileHandle, AccessType_Write, ioStats));
	}

	void setReadOnly(const wchar_t* file, bool readonly, bool source = true)
	{
		const WString dir = source ? testSourceDir : testDestDir;
		EACOPY_ASSERT(setFileWritable((dir + file).c_str(), !readonly));
	}

	void writeRandomData(const wchar_t* sourceFile, u64 fileSize)
	{
		char* data = new char[fileSize];
		memset(data, 0, fileSize);
		for (uint i=0; i!=100; ++i)
		{
			char buffer[128];
			memset(buffer, 0, sizeof(buffer));
			uint writePos = std::max((uint)0, (uint)(((uint(rand()) << 16) + uint(rand())) % (fileSize - 128)));
			memcpy(data + writePos, buffer, sizeof(buffer));
		}

		FileInfo fileInfo;
		fileInfo.fileSize = fileSize;
		EACOPY_ASSERT(createFile(sourceFile, fileInfo, data, ioStats, true));

		delete[] data;
	}

	u64 m_setupTime;
	Log clientLog;
	Log serverLog;
	WString name;
	WString testSourceDir;
	WString testDestDir;
	bool skipped = false;
	IOStats ioStats;

	static TestBase* s_firstTest;
	static TestBase* s_lastTest;
	TestBase* m_nextTest = nullptr;

	static uint s_successCount;
	static uint s_skipCount;
};

TestBase* TestBase::s_firstTest;
TestBase* TestBase::s_lastTest;
uint TestBase::s_successCount;
uint TestBase::s_skipCount;


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define EACOPY_TEST_LOOP(name, loopCount)															\
	struct Test_##name : public TestBase															\
	{																								\
		Test_##name(bool isTemp) : TestBase(L## #name, isTemp) {}									\
		virtual void runImpl(uint loopIndex) override;												\
		virtual uint runCount() override { return loopCount; }										\
	} test_##name(false);																			\
	void test##name() { uint index = 0; Test_##name test(true); test.run(index, test.runCount()); }	\
	void Test_##name::runImpl(uint loopIndex)														\


#define EACOPY_TEST(name) EACOPY_TEST_LOOP(name, 1)


#define EACOPY_REQUIRE_EXTERNAL_SHARE																\
	if (g_testExternalDestDir.empty())																\
	{																								\
		skipped = true;																				\
		logInfoLinef(L"Skipped (No external dest dir provided)");									\
		return;																						\
	}																								\

#define EACOPY_REQUIRE_ADMIN																		\
	if (!IsUserAnAdmin())																			\
	{																								\
		skipped = true;																				\
		logInfoLinef(L"Skipped (Need admin rights for this test)");									\
		return;																						\
	}																								\

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(_WIN32)
EACOPY_TEST(UncPathOptimization)
{
	WString temp;
	const wchar_t* optimizedPath = optimizeUncPath(g_testDestDir.c_str(), temp);
	EACOPY_ASSERT(optimizedPath[0] && optimizedPath[1] == L':');
}
#endif

EACOPY_TEST(CopySmallFile)
{
	createTestFile(L"Foo.txt", 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopySmallFileDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createTestFile(L"Foo.txt", 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyMediumFile)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(SkipFile)
{
	createTestFile(L"Foo.txt", 100);
	bool existed;
	u64 bytesCopied;
	(void)existed; // suppress unused variable warning
	(void)bytesCopied; // suppress unused variable warning

	EACOPY_ASSERT(copyFile((testSourceDir + L"Foo.txt").c_str(), (testDestDir + L"Foo.txt").c_str(), false, true, existed, bytesCopied, ioStats, UseBufferedIO_Enabled));
	EACOPY_ASSERT(!existed);
	EACOPY_ASSERT(bytesCopied == 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	ClientStats stats;
	EACOPY_ASSERT(client.process(clientLog, stats) == 0);
	EACOPY_ASSERT(stats.skipCount == 1);
	EACOPY_ASSERT(stats.skipSize = 100);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(OverwriteFile)
{
	createTestFile(L"Foo.txt", 100);
	createTestFile(L"Foo.txt", 101, false);
	EACOPY_ASSERT(!isSourceEqualDest(L"Foo.txt"));

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	ClientStats stats;
	EACOPY_ASSERT(client.process(clientLog, stats) == 0);
	EACOPY_ASSERT(stats.copyCount == 1);
	EACOPY_ASSERT(stats.copySize = 100);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(OverwriteFirstCopySecondFile)
{
	createTestFile(L"Foo1.txt", 100);
	createTestFile(L"Foo2.txt", 100);
	createTestFile(L"Foo1.txt", 101, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	ClientStats stats;
	EACOPY_ASSERT(client.process(clientLog, stats) == 0);
	EACOPY_ASSERT(stats.copyCount == 2);
	EACOPY_ASSERT(stats.copySize = 200);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo1.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"Foo2.txt"));
}

EACOPY_TEST(CopyEmptyDir)
{
	ensureDirectory((testSourceDir + L"\\Directory").c_str());

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.copySubdirDepth = 100;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Directory").c_str()) != 0);
}

EACOPY_TEST(CopyFileToReadOnlyDest)
{
	// Create 3 files, 2 which exist in the destination and are different but Foo.txt is set to be readonly.
	// We expect that even though Foo.txt is readonly it will still get copied over the destination Foo.txt
	
	// source files:
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Bar1.txt", 10);
	createTestFile(L"Bar2.txt", 10);
	
	//dest files:
	createTestFile(L"Foo.txt", 100, false);
	setReadOnly(L"Foo.txt", true, false);
	createTestFile(L"Bar2.txt", 100, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 1;
	clientSettings.retryCount = 2;

	Client client(clientSettings);
	ClientStats clientStats;

	// We expect this to succeed because Foo.txt in dest should have its file attributes changed from read-only to normal
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 3);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"Bar1.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"Bar2.txt"));
}

#if defined(_WIN32)
EACOPY_TEST(CopyFileDestLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
	EACOPY_ASSERT(!isSourceEqualDest(L"Foo.txt"));

	FileHandle destFile;
	openFileRead((testDestDir + L"Foo.txt").c_str(), destFile, ioStats, true);
	EACOPY_ASSERT(destFile);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 100;
	clientSettings.retryCount = 100;

	Client client(clientSettings);
	ClientStats clientStats;

	Thread thread([&]()
	{
		while (clientStats.retryCount == 0)
			Sleep(10);
		closeFile(L"", destFile, AccessType_Read, ioStats);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}
#endif

#if defined(_WIN32)
EACOPY_TEST(CopyFileSourceSharedReadLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	FileHandle sourceFile;
	openFileRead((testSourceDir + L"Foo.txt").c_str(), sourceFile, ioStats, true, nullptr, true, false);
	EACOPY_ASSERT(sourceFile);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 100;
	clientSettings.retryCount = 100;

	Client client(clientSettings);
	ClientStats clientStats;

	Thread thread([&]()
	{
		while (clientStats.retryCount == 0)
			Sleep(10);
		closeFile(L"", sourceFile, AccessType_Read, ioStats);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}
#endif

#if defined(_WIN32)
EACOPY_TEST(CopyFileSourceReadLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	FileHandle sourceFile;
	openFileRead((testSourceDir + L"Foo.txt").c_str(), sourceFile, ioStats, true, nullptr, true, false);
	EACOPY_ASSERT(sourceFile);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 100;
	clientSettings.retryCount = 100;

	Client client(clientSettings);
	ClientStats clientStats;

	Thread thread([&]()
	{
		while (clientStats.retryCount == 0)
			Sleep(10);
		closeFile(L"", sourceFile, AccessType_Read, ioStats);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}
#endif

#if defined(_WIN32)
EACOPY_TEST(CopyFileSourceWriteLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	FileHandle sourceFile;
	openFileRead((testSourceDir + L"Foo.txt").c_str(), sourceFile, ioStats, true, nullptr, true, false);
	EACOPY_ASSERT(sourceFile);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 100;
	clientSettings.retryCount = 100;

	Client client(clientSettings);
	ClientStats clientStats;

	Thread thread([&]()
	{
		while (clientStats.retryCount == 0)
			Sleep(10);
		closeFile(L"", sourceFile, AccessType_Read, ioStats);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}
#endif

EACOPY_TEST(CopyWildcardMissing)
{
	ClientSettings clientSettings = getDefaultClientSettings(L"Test.txt");
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) != 0);
}

EACOPY_TEST(CopyFileList)
{
	createTestFile(L"Foo.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyFullPathFileList)
{
	String fooAbsPath(toString(testSourceDir.c_str()));
	fooAbsPath += "Foo.txt\n";
	String barAbsPath(toString(testSourceDir.c_str()));
	barAbsPath += "Dir\\Bar.txt";

	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Dir\\Bar.txt", 20);
	createFileList(L"FileList.txt", (fooAbsPath + barAbsPath).c_str());

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"Dir\\Bar.txt"));
}

EACOPY_TEST(CopyFileListWithFileWithDefinedDest)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createFileList(L"FileList.txt", "A\\B\\Foo.txt A");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}

EACOPY_TEST(CopyFileListWithFileWithDefinedDestBeingRoot)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createFileList(L"FileList.txt", "A\\B\\Foo.txt .");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}

EACOPY_TEST(CopyFileListWithDirWithDefinedDest)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createTestFile(L"A\\B\\C\\Bar.txt", 10);
	createFileList(L"FileList.txt", "A\\B A");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.copySubdirDepth = 2;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\C\\Bar.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}

EACOPY_TEST(CopyFileListWithDirWithDefinedDestBeingRoot)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createFileList(L"FileList.txt", "A\\B .");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}
/*
EACOPY_TEST(CopyFileListWithDirWithDefinedDestPurged)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createTestFile(L"A\\B\\Bar.txt", 10, false);
	createTestFile(L"A\\Hej.txt", 10, false);
	createFileList(L"FileList.txt", "A\\B A /PURGE");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\B\\Foo.txt").c_str()) == 0);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\Hej.txt").c_str()) == 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}
*/
EACOPY_TEST(CopyFileListWithDirPurged)
{
	createTestFile(L"A\\B\\Foo.txt", 10);
	createTestFile(L"A\\B\\Bar.txt", 10, false);
	createTestFile(L"A\\Hej.txt", 10, false);
	createTestFile(L"Bau.txt", 10, false);
	createFileList(L"FileList.txt", "A /PURGE");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.copySubdirDepth = 2;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\B\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\B\\Bar.txt").c_str()) == 0);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"A\\Hej.txt").c_str()) == 0);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Bau.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
}

EACOPY_TEST(CopyFullPathFileListFlatten)
{
	String fooAbsPath(toString(testSourceDir.c_str()));
	fooAbsPath += "Foo.txt\n";
	String barAbsPath(toString(testSourceDir.c_str()));
	barAbsPath += "Dir\\Bar.txt";

	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Dir\\Bar.txt", 20);
	createFileList(L"FileList.txt", (fooAbsPath + barAbsPath).c_str());

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.flattenDestination = true;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Bar.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 20);
}

EACOPY_TEST(CopyFileListDuplicate)
{
	String fooAbsPath(toString(testSourceDir.c_str()));
	fooAbsPath += "Foo.txt\n";
	createTestFile(L"Foo.txt", 10);
	createFileList(L"FileList.txt", (fooAbsPath + "\nFoo.txt\nFoo.txt").c_str());

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 0);
}

EACOPY_TEST(CopyFileListMissing)
{
	createFileList(L"FileList.txt", "Foo.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) != 0);
}

#if defined(_WIN32)
EACOPY_TEST(CopyFileListMissingOptional)
{
	createFileList(L"FileList.txt", "Foo.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.optionalWildcards.push_back(L"*.txt");
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
}
#endif

EACOPY_TEST(CopyFileListMissingAndFound)
{
	createTestFile(L"Bar.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt\nBar.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.failCount == 1);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}

EACOPY_TEST(CopyFileListExcludeList)
{
	createTestFile(L"Foo.txt", 10);
	createFileList(L"IncludeList.txt", "Foo.txt\nBar.txt");
	createFileList(L"ExcludeList.txt", "Bar.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"IncludeList.txt");
	clientSettings.filesExcludeFiles.push_back(L"ExcludeList.txt");
	Client client(clientSettings);
	ClientStats clientStats;

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}

EACOPY_TEST(CopyFileListExcludeListError)
{
	createFileList(L"ExcludeList.txt", "*.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesExcludeFiles.push_back(L"ExcludeList.txt");
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) != 0);
}

 EACOPY_TEST(CopyFileListWithFileListFile)
{
	createFileList(L"FileList.txt", "FileList.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	Client client(clientSettings);
	clientSettings.threadCount = 2;

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\FileList.txt").c_str()) != 0);
}

EACOPY_TEST(CopyFilePurge)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"A\\B\\Foo.txt", 10);
	createTestFile(L"Bar.txt", 10, false);
	createTestFile(L"A\\B\\Bar.txt", 10, false);
	createTestFile(L"DestDir\\Boo.txt", 10, false);
	ensureDirectory((testSourceDir + L"SourceDir").c_str());
	ensureDirectory((testSourceDir + L"SourceDir2").c_str());
	createTestFile(L"SourceDir2\\Boo.txt", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 3;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"Foo.txt") == true);
	EACOPY_ASSERT(getTestFileExists(L"Bar.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"A\\B\\Foo.txt") == true);
	EACOPY_ASSERT(getTestFileExists(L"A\\B\\Bar.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir") == false);
	EACOPY_ASSERT(getTestFileExists(L"DestDir") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir2") == false);
}

EACOPY_TEST(CopyFileMirror)
{
	ensureDirectory((testSourceDir + L"SourceDir").c_str());
	ensureDirectory((testSourceDir + L"SourceDir2").c_str());
	createTestFile(L"SourceDir2\\Boo.txt", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 3;
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir") == true);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir2") == true);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir2\\Boo.txt") == false);
}

EACOPY_TEST(CopyFileTargetDirectoryIsfile)
{
	ensureDirectory((testSourceDir + L"SourceDir").c_str());
	createTestFile(L"SourceDir/Foo", 10, true);
	createTestFile(L"SourceDir", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1;
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) != 0);
}

#if defined(_WIN32)
EACOPY_TEST(CopyFileTargetHasSymlink)
{
	ensureDirectory((testSourceDir + L"Source\\RealDir").c_str());
	ensureDirectory((testDestDir + L"Dest").c_str());
	createTestFile(L"Source\\RealDir\\Boo.txt", 10, true);

	// Probably don't have privilege.. skip this test
	if (!CreateSymbolicLinkW((testDestDir + L"Dest\\RealDir").c_str(), (testSourceDir + L"Source\\RealDir").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY))
		return;

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.sourceDirectory = testSourceDir + L"Source\\";
	clientSettings.destDirectory = testDestDir + L"Dest\\";
	clientSettings.copySubdirDepth = 3;
	Client client(clientSettings);

	FileInfo fi;
	EACOPY_ASSERT((getFileInfo(fi, (testDestDir + L"Dest\\RealDir").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"Source\\RealDir\\Boo.txt", true) == true);
	EACOPY_ASSERT(getTestFileExists(L"Dest\\RealDir\\Boo.txt", false) == true);
	EACOPY_ASSERT((getFileInfo(fi, (testDestDir + L"Dest\\RealDir").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
}
#endif

#if defined(_WIN32)
EACOPY_TEST(CopyFileWithPurgeTargetHasSymlink)
{
	ensureDirectory((testSourceDir + L"Source").c_str());
	ensureDirectory((testSourceDir + L"RealDir").c_str());
	ensureDirectory((testDestDir + L"Dest").c_str());
	createTestFile(L"RealDir\\Boo.txt", 10, true);

	// Probably don't have privilege.. skip this test
	if (!CreateSymbolicLinkW((testDestDir + L"Dest\\RealDir").c_str(), (testSourceDir + L"RealDir").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY))
		return;

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.sourceDirectory = testSourceDir + L"Source\\";
	clientSettings.destDirectory = testDestDir + L"Dest\\";
	clientSettings.copySubdirDepth = 3;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	FileInfo fi;
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"RealDir\\Boo.txt", true) == true);
	EACOPY_ASSERT(getTestFileExists(L"Dest\\RealDir", false) == false);
	EACOPY_ASSERT((getFileInfo(fi, (testDestDir + L"Dest\\RealDir").c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
}
#endif

EACOPY_TEST(CopyFileWithVeryLongPath)
{
	WString longPath;
	for (uint i=0;i!=30; ++i)
		longPath.append(L"TestDir\\");
	longPath.append(L"Foo.txt");
	createTestFile(longPath.c_str(), 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1000;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(longPath.c_str()));
}

EACOPY_TEST(CopyUsingLink)
{
	createTestFile(L"Foo.txt", 100);

	auto originalTestDir = testDestDir;
	testDestDir = originalTestDir + L"1\\";
	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.destDirectory = testDestDir;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));

	clientSettings.useFileLinks = true;
	clientSettings.additionalLinkDirectories.push_back(testDestDir);

	testDestDir = originalTestDir + L"2\\";
	clientSettings.destDirectory = testDestDir;
	Client client2(clientSettings);
	EACOPY_ASSERT(client2.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(LinkFileWithVeryLongPath)
{
	WString longPath;
	for (uint i = 0; i != 30; ++i)
		longPath.append(L"TestDir\\");
	WString longPathFile = longPath + L"Foo.txt";
	createTestFile(longPathFile.c_str(), 100);

	{
		auto oldTestDestDir = testDestDir;
		testDestDir += L"1\\";
		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.destDirectory = testDestDir;
		clientSettings.copySubdirDepth = 1000;
		Client client(clientSettings);
		EACOPY_ASSERT(client.process(clientLog) == 0);
		EACOPY_ASSERT(isSourceEqualDest(longPathFile.c_str()));
		testDestDir = oldTestDestDir;
	}

	{
		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.copySubdirDepth = 1000;
		clientSettings.useFileLinks = true;
		clientSettings.additionalLinkDirectories.push_back(testDestDir + L"1\\" + longPath);
		testDestDir += L"2\\";
		clientSettings.destDirectory = testDestDir;
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(isSourceEqualDest(longPathFile.c_str()));
		EACOPY_ASSERT(clientStats.linkCount == 1);
	}

	// Purge
	{
		EACOPY_ASSERT(deleteFile((testSourceDir + longPathFile).c_str(), ioStats));
		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.purgeDestination = true;
		clientSettings.destDirectory = testDestDir;
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.ioStats.deleteFileCount == 1);
	}
}

EACOPY_TEST(CopyUsingOdx)
{
	testSourceDir = testDestDir;
	testSourceDir += L"A\\";
	ensureDirectory(testSourceDir.c_str());
	testDestDir += L"B\\";
	createTestFile(L"Foo.txt", 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

#if defined(_WIN32)
EACOPY_TEST(ServerCopyAttemptFallback)
{
	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Automatic;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.serverAttempt == 1);
	EACOPY_ASSERT(clientStats.destServerUsed == false);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopyAttemptFail)
{
	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.serverAttempt == true);
	EACOPY_ASSERT(clientStats.destServerUsed == false);
	EACOPY_ASSERT(clientStats.copyCount == 0);
}

EACOPY_TEST(ServerCopyDirs)
{
	createTestFile(L"A\\Foo.txt", 10);
	createTestFile(L"B\\Bar.txt", 11);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.copySubdirDepth = 100;

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
	EACOPY_ASSERT(isSourceEqualDest(L"A\\Foo.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"B\\Bar.txt"));
}

EACOPY_TEST(ServerCopySmallFile)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopySmallFileProtocolMismatch)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog, ~0u);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Automatic;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopySmallFileDestIsLocal)
{
	std::swap(testSourceDir, testDestDir);
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopyDirectories)
{
	testDestDir += L"ExtraDir\\ExtraDir2\\";
	createTestFile(L"A\\D\\Bar.txt", 11);
	createTestFile(L"B\\F\\Meh.txt", 12);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.copySubdirDepth = 2;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
	EACOPY_ASSERT(isSourceEqualDest(L"A\\D\\Bar.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"B\\F\\Meh.txt"));

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 2);
}

EACOPY_TEST(ServerCopyDirectoriesDestIsLocal)
{
	std::swap(testSourceDir, testDestDir);
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"A\\Bar.txt", 11);
	createTestFile(L"B\\Meh.txt", 12);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.copySubdirDepth = 2;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 3);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"A\\Bar.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"B\\Meh.txt"));

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 3);
}

EACOPY_TEST(ServerCopyMediumFile)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopyMediumFileCompressed)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.compressionEnabled = true;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopyMultiThreaded)
{
	uint fileCount = 50;
	uint testCount = 50;

	for (uint i=0; i!=fileCount; ++i)
	{
		wchar_t fileName[1024];
		StringCbPrintfW(fileName, sizeof(fileName), L"Foo%i.txt", i);
		createTestFile(fileName, 100 + i);
	}

	for (uint i=0; i!=testCount; ++i)
	{
		ServerSettings serverSettings(getDefaultServerSettings());
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.threadCount = 8;
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(i != 0 || clientStats.copyCount == fileCount);
		EACOPY_ASSERT(i == 0 || clientStats.skipCount == fileCount);
	}
}

EACOPY_TEST(ServerCopyMultiClient)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	for (uint i=0; i!=50; ++i)
	{
		wchar_t iStr[10];
		itow(i, iStr, eacopy_sizeof_array(iStr));

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir + iStr + L"\\";
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(i != 0 || clientStats.copyCount == 1);
		EACOPY_ASSERT(i == 0 || clientStats.linkCount == 1);
	}
}

EACOPY_TEST(ServerCopyLink)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats1;
	clientSettings.destDirectory = testDestDir + L"1\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats1) == 0);
	EACOPY_ASSERT(clientStats1.copyCount == 1);

	ClientStats clientStats2;
	clientSettings.destDirectory = testDestDir + L"2\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats2) == 0);
	EACOPY_ASSERT(clientStats2.linkCount == 1);
}

EACOPY_TEST(ServerCopyByHash)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	serverSettings.useHash = true;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats1;
	clientSettings.destDirectory = testDestDir + L"1\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats1) == 0);
	EACOPY_ASSERT(clientStats1.copyCount == 1);

	deleteFile((testSourceDir + L"\\Foo.txt").c_str(), ioStats);
	createTestFile(L"Foo.txt", 10);

	ClientStats clientStats2;
	clientSettings.destDirectory = testDestDir + L"2\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats2) == 0);
	EACOPY_ASSERT(clientStats2.linkCount == 1);

	deleteFile((testSourceDir + L"\\Foo.txt").c_str(), ioStats);
	createTestFile(L"Foo.txt", 10);

	ClientStats clientStats3;
	clientSettings.sourceDirectory = testDestDir + L"2\\";
	clientSettings.destDirectory = testSourceDir;
	EACOPY_ASSERT(client.process(clientLog, clientStats3) == 0);
	EACOPY_ASSERT(clientStats3.skipCount == 1);
}

EACOPY_TEST(ServerCopySameDest)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 1);
}

EACOPY_TEST(ServerCopyExistingDestAndFoundLinkSomewhereElse)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"1\\";
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	ClientSettings clientSettings2(getDefaultClientSettings());
	clientSettings2.useServer = UseServer_Required;
	clientSettings2.destDirectory = testDestDir + L"2\\";
	Client client2(clientSettings2);
	EACOPY_ASSERT(client2.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.linkCount == 1);

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 1);
}

EACOPY_TEST(ServerCopyBadDest)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = L"\\\\localhost\\";
	clientSettings.retryWaitTimeMs = 1;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.copyCount == 0);
}

EACOPY_TEST(ServerCopyFileFail)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Bar.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
	setReadOnly(L"Foo.txt", true, false);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.retryWaitTimeMs = 1;
	clientSettings.retryCount = 2;
	Client client(clientSettings);
	ClientStats clientStats;

	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.failCount == 1);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}

EACOPY_TEST(ServerCopyFileDestLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
	FileHandle destFile = CreateFileW((testDestDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	EACOPY_ASSERT(destFile);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.retryWaitTimeMs = 100;
	clientSettings.retryCount = 100;

	Client client(clientSettings);
	ClientStats clientStats;

	Thread thread([&]()
	{
		while (clientStats.retryCount <= 2)
			Sleep(10);
		CloseHandle(destFile);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}

EACOPY_TEST(ServerPurgeWithNoCopy)
{
	createTestFile(L"Foo.txt", 10, false);
	createTestFile(L"DestDir\\Boo.txt", 10, false);
	createTestFile(L"SourceDir2\\Boo.txt", 10, false);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.purgeDestination = true;
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"Foo.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"DestDir") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceDir2") == false);
}

EACOPY_TEST(ServerReport)
{
	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings;
	clientSettings.destDirectory = testDestDir;
	Client client(clientSettings);
	EACOPY_ASSERT(client.reportServerStatus(clientLog) == 0);
}

EACOPY_TEST(ServerReportUsingBadPath)
{
	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings;
	clientSettings.destDirectory = L"\\\\localhost\\";
	Client client(clientSettings);
	EACOPY_ASSERT(client.reportServerStatus(clientLog) == -1);
}

EACOPY_TEST(ServerLinkNotExists)
{
	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"1\\";

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	deleteFile((testDestDir + L"1\\Foo.txt").c_str(), ioStats);

	clientSettings.destDirectory = testDestDir + L"2\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
}

EACOPY_TEST(ServerLinkModified)
{
	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"1\\";

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	deleteFile((testDestDir + L"1\\Foo.txt").c_str(), ioStats);
	createTestFile(L"1\\Foo.txt", 10, false);

	clientSettings.destDirectory = testDestDir + L"2\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
}
#endif

EACOPY_TEST(CopyFileWithDoubleSlashPath)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Test\\\\Foo.txt", fileSize);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Test\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
}

EACOPY_TEST(CopyFileWithDoubleSlashPath2)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Test\\\\Test2\\Foo.txt", fileSize);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 2;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Test\\Test2\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
}

#if defined(_WIN32)
EACOPY_TEST(CopyFileWithExplicitWildCardExtensionUnderDirectories)
{
	uint fileSize = 3 * 1024 * 1024 + 123;
	createTestFile(L"Test\\Foo.txt", fileSize);
	createTestFile(L"Bar.txt", 100);
	createTestFile(L"Test2\\Foo2.txt", 1000);
	//Verify files not matching wild card are just skipped when wild cards are used. Does not apply to using FileList
	createTestFile(L"Test3\\NotFoo.xml", 100);

	ClientSettings clientSettings(getDefaultClientSettings(L"*.txt"));
	clientSettings.copySubdirDepth = 100;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Test\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Bar.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Test2\\Foo2.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 1000);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"Test3\\NotFoo.xml").c_str()) == 0);
}
#endif

#if defined(EACOPY_ALLOW_DELTA_COPY_SEND)
EACOPY_TEST_LOOP(ServerCopyMediumFileDelta, 3)
{
	u64 fileSizes[] =
	{
		8 * 1024,
		8 * 1024 * 1024,
		u64(INT_MAX) + 2*1024*1024 + 123,
	};
	u64 fileSize = fileSizes[loopIndex];

	createTestFile(L"Foo.txt", fileSize);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.deltaCompressionThreshold = 0;

	{
		clientSettings.destDirectory = testDestDir+ L"1\\";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	writeRandomData((testSourceDir + L"Foo.txt").c_str(), fileSize);

	{
		clientSettings.destDirectory = testDestDir+ L"2\\";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	FileInfo sourceFile;
	EACOPY_ASSERT(getFileInfo(sourceFile, (testSourceDir + L"Foo.txt").c_str()) != 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"2\\Foo.txt").c_str()) != 0);

	EACOPY_ASSERT(destFile.fileSize == sourceFile.fileSize);
	EACOPY_ASSERT(destFile.lastWriteTime.dwLowDateTime == sourceFile.lastWriteTime.dwLowDateTime && destFile.lastWriteTime.dwHighDateTime == sourceFile.lastWriteTime.dwHighDateTime);
}
#endif

#if defined(EACOPY_ALLOW_DELTA_COPY_RECEIVE)
EACOPY_TEST(ServerCopyDeltaSmallFileDestIsLocal)
{
	std::swap(testSourceDir, testDestDir);

	createTestFile(L"1\\Foo.txt", 16 * 1024 * 1024);
	createTestFile(L"2\\Foo.txt", 16 * 1024 * 1024);
	writeRandomData((testSourceDir + L"2\\Foo.txt").c_str(), 16 * 1024 * 1024);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();
	EACOPY_ASSERT(server.primeDirectory(wcschr(testSourceDir.c_str() + 2, '\\') + 1));

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	clientSettings.sourceDirectory = testSourceDir + L"1\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	clientSettings.sourceDirectory = testSourceDir + L"2\\";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}
#endif

EACOPY_TEST(CopyFileListDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createTestFile(L"Foo.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyMultiFileListDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createTestFile(L"Foo1.txt", 10);
	createTestFile(L"Foo2.txt", 10);
	createFileList(L"FileList1.txt", "Foo1.txt");
	createFileList(L"FileList2.txt", "Foo2.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList1.txt");
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList2.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo1.txt"));
	EACOPY_ASSERT(isSourceEqualDest(L"Foo2.txt"));
}

#if defined(_WIN32)
EACOPY_TEST(ServerCopyFileListDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createTestFile(L"Foo.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt");

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.useServer = UseServer_Required;
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerCopyMissingFileListDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createFileList(L"FileList.txt", "Foo.txt");

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.useServer = UseServer_Required;
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) != 0);
}

EACOPY_TEST(CopyFileWithVeryLongPathDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);

	//This test when run in Visual Studio must be have Visual Studio run as Administrator!
	WString longPath;
	for (uint i=0;i!=30; ++i)
		longPath.append(L"TestDir\\");
	longPath.append(L"Foo.txt");
	createTestFile(longPath.c_str(), 100);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.copySubdirDepth = 1000;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(longPath.c_str()));
}
#endif

EACOPY_TEST(CopyLargeFile)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);
	FileInfo sourceFile;
	EACOPY_ASSERT(getFileInfo(sourceFile, (testSourceDir + L"Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(sourceFile.fileSize == fileSize);

	for (uint i=0; i!=1; ++i)
	{
		wchar_t iStr[10];
		itow(i, iStr, eacopy_sizeof_array(iStr));

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.destDirectory = testDestDir+ iStr + L'\\';
		Client client(clientSettings);

		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + iStr + L"\\Foo.txt").c_str()) != 0);
		EACOPY_ASSERT(destFile.fileSize == fileSize);
	}
}

#if defined(_WIN32)
EACOPY_TEST(ServerCopyLargeFile)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		ServerSettings serverSettings(getDefaultServerSettings());
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		wchar_t iStr[10];
		itow(i, iStr, eacopy_sizeof_array(iStr));

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir + iStr + L'\\';

		Client client(clientSettings);
		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + iStr + L"\\Foo.txt").c_str()) != 0);
		EACOPY_ASSERT(destFile.fileSize == fileSize);
	}
}

EACOPY_TEST(ServerCopyLargeFileCompressed)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		ServerSettings serverSettings(getDefaultServerSettings());
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		wchar_t iStr[10];
		itow(i, iStr, eacopy_sizeof_array(iStr));

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir + iStr + L'\\';
		clientSettings.compressionEnabled = true;
		clientSettings.compressionLevel = 4;

		Client client(clientSettings);
		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + iStr + L"\\Foo.txt").c_str()) != 0);
		EACOPY_ASSERT(destFile.fileSize == fileSize);
	}
}
/*
EACOPY_TEST(ServerTestMemory)
{
	for (uint i=0; i!=50; ++i)
	{
		wchar_t fileName[1024];
		StringCbPrintfW(fileName, sizeof(fileName), L"Foo%i.txt", i);
		createTestFile(fileName, 100);
	}

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	for (uint i=0; i!=10000; ++i)
	{
		wchar_t iStr[10];
		itow(i, iStr, eacopy_sizeof_array(iStr));

		ClientSettings clientSettings;
		clientSettings.sourceDirectory = testSourceDir;
		clientSettings.destDirectory = testDestDir + iStr + L'\\';
		clientSettings.filesOrWildcards.push_back(L"*.*");
		clientSettings.copySubdirDepth = 100;
		clientSettings.useServer = UseServer_Required;
		clientSettings.threadCount = 8;
		clientSettings.compressionEnabled = true;

		Client client(clientSettings);

		ClientStats clientStats;
		client.process(clientLog, clientStats);
		deleteDirectory(clientSettings.destDirectory.c_str());
	}
}
*/

EACOPY_TEST(UsedByOtherProcessError)
{
	wchar_t buffer[1024];
	wchar_t buffer2[1024];
	uint size = GetCurrentDirectoryW(1024, buffer);

	#if !defined(NDEBUG)
	wcscat(buffer, L"\\..\\Debug\\");
	#else
	wcscat(buffer, L"\\..\\Release\\");
	#endif
	//Handle case for debugging in Visual Studio. Executable path is at ..\\ level (not under Debug or Release)
	wcsncpy(buffer2, buffer, 1024);
	wcscat(buffer2, L"eacopy.exe");
	if (!(getGeneralFileExists(buffer2) == true))
	{
		wcscat(buffer, L"..\\");
	}

	ClientSettings clientSettings;
	clientSettings.sourceDirectory += buffer;
	clientSettings.destDirectory = buffer;
	clientSettings.filesOrWildcards.push_back(L"EACopy.exe");
	clientSettings.forceCopy = true;
	clientSettings.retryCount = 0;

	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.failCount == 1);
	EACOPY_ASSERT(clientStats.copyCount == 0);
}
#endif

EACOPY_TEST(FileGoingOverMaxPath)
{
	//EACOPY_REQUIRE_ADMIN

	createTestFile(L"FooLongLongName.txt", 100);
	createTestFile(L"BarLongLongName.txt", 101);

	ClientSettings clientSettings(getDefaultClientSettings());
	while (clientSettings.destDirectory.size() <= MAX_PATH)
		clientSettings.destDirectory += L"DirName\\";
	clientSettings.destDirectory.resize(246);
	clientSettings.destDirectory += L"\\";

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isEqual((testSourceDir + L"FooLongLongName.txt").c_str(), (clientSettings.destDirectory + L"FooLongLongName.txt").c_str()));
	EACOPY_ASSERT(isEqual((testSourceDir + L"BarLongLongName.txt").c_str(), (clientSettings.destDirectory + L"BarLongLongName.txt").c_str()));
}

EACOPY_TEST(PathGoingOverMaxPath)
{
	//EACOPY_REQUIRE_ADMIN

	createTestFile(L"Foo.txt", 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	while (clientSettings.destDirectory.size() <= MAX_PATH + 100)
		clientSettings.destDirectory += L"wefwqwdqwdef\\";
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isEqual((testSourceDir + L"Foo.txt").c_str(), (clientSettings.destDirectory + L"Foo.txt").c_str()));
}

#if defined(_WIN32)
EACOPY_TEST(ServerCopyFileExternalPath)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	const wchar_t* file = L"Foo.txt";
	createTestFile(file, 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	WString externalDest = g_testExternalDestDir + L'\\';

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.serverAddress = L"localhost";
	clientSettings.destDirectory = externalDest;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
}

EACOPY_TEST(FromServerCopyFileExternalPath)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	WString externalDest = g_testExternalDestDir + L'\\';
	ensureDirectory(externalDest.c_str());
	const wchar_t* file = L"Foo.txt";
	createTestFile(externalDest.c_str(), file, 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.serverAddress = L"localhost";
	clientSettings.sourceDirectory = externalDest;
	clientSettings.destDirectory = testSourceDir;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
}

EACOPY_TEST(ServerCopyFileExternalPathUseHistory)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	const wchar_t* file = L"Foo.txt";
	createTestFile(file, 1024*1024);

	ServerSettings serverSettings(getDefaultServerSettings());
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.serverAddress = L"localhost";

	{
		WString externalDest = g_testExternalDestDir + L"\\A\\";
		deleteFile((externalDest + file).c_str(), ioStats, false);
		clientSettings.destDirectory = externalDest;
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
		EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
	}
	{
		WString externalDest = g_testExternalDestDir + L"\\B\\";
		deleteFile((externalDest + file).c_str(), ioStats, false);
		clientSettings.destDirectory = externalDest;
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.linkCount == 1);
		EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
	}
}

EACOPY_TEST(ServerTestSecurityFileCopyToServer)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	serverSettings.useSecurityFile = true;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerTestSecurityFileCopyFromServer)
{
	std::swap(testDestDir, testSourceDir);

	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	serverSettings.useSecurityFile = true;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(ServerTestSecurityFileMultiThreadedClient)
{
	createTestFile(L"Foo1.txt", 10);
	createTestFile(L"Foo2.txt", 10);
	createTestFile(L"Foo3.txt", 10);
	createTestFile(L"Foo4.txt", 10);

	ServerSettings serverSettings(getDefaultServerSettings());
	serverSettings.useSecurityFile = true;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.threadCount = 2;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 4);
}
#endif

void printHelp()
{
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef(L"  EACopyTest (Client v%ls Server v%ls) (c) Electronic Arts.  All Rights Reserved.", ClientVersion, ServerVersion);
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef();
	logInfoLinef(L"             Usage :: EACopyTest source destination");
	logInfoLinef();
	logInfoLinef(L"            source :: Source Directory (drive:\\path).");
	logInfoLinef(L"       destination :: Destination Dir  (\\\\localhost\\share\\path). Must be local host");
	logInfoLinef();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy


#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[])
{
	using namespace eacopy;
#else
int main(int argc, char* argv_[])
{
	using namespace eacopy;
	wchar_t* argv[64];
	WString temp[64];
	for (int i=0; i!=argc; ++i)
	{
		temp[i] = WString(argv_[i], argv_[i] + strlen(argv_[i]));
		argv[i] = const_cast<wchar_t*>(temp[i].c_str());
	}
#endif

	// If source and dest are not hardcoded we use command line
	if (g_testSourceDir.empty() || g_testDestDir.empty())
	{
		if (argc == 2 && equalsIgnoreCase(argv[1], L"/?"))
		{
			printHelp();

			return 0;
		}

		if (argc <= 2)
		{
			logInfoLinef(L"NO TESTS WERE EXECUTED. /? for help");
			return -1;
		}

		g_testSourceDir = argv[1];
		g_testDestDir = argv[2];
	}
	else
	{
#if !defined(_WIN32)
		const char* home = getenv("HOME");
		WString whome(home, home + strlen(home));
		g_testSourceDir = whome + L"/" + g_testSourceDir;
		g_testDestDir = whome + L"/" + g_testDestDir;
		std::replace(g_testSourceDir.begin(), g_testSourceDir.end(), '/', '\\');
		std::replace(g_testDestDir.begin(), g_testDestDir.end(), '/', '\\');
#endif
	}

	// Run all the tests
	TestBase::runAll();


	logInfoLinef();
	logInfoLinef(L"  Tests finished. (%u succeeded and %u skipped)", TestBase::s_successCount, TestBase::s_skipCount);


	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
