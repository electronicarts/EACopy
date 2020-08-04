// (c) Electronic Arts. All Rights Reserved.

#include "EACopyClient.h"
#include "EACopyServer.h"
#include <assert.h>
#include <shlwapi.h>
#include <strsafe.h>

namespace eacopy
{

#if defined(NDEBUG)
#define EACOPY_ASSERT(x) if (!(x)) { logInfoLinef(L"ASSERT! " #x "\r\n"); *(int*)nullptr = 0xdeadbeef; }
#else
	#define EACOPY_ASSERT(x) assert(x)
#endif

//Set these Variables to run EACopyTest without having to specify the source/dest input parameters.
//Examples are detailed:
// L"C:\\temp\\EACopyTest\\source" OR L"I:\\MyLocalDrive"
// This directory should exist.
#define DEFAULT_SOURCE_DIR  L"" 
// L"\\\\localhost\\EACopyTest\\dest" OR L"\\\\localhost\\MyShare"
// Locally configured share to C:\temp\EACopyTest\dest OR I:\MyShare
//The real directory should exist and the share setup to point to the directory.
#define DEFAULT_DEST_DIR  L""
// Some network share on another machine than where the EACopyService run
#define DEFAULT_EXTERNAL_DEST_DIR L""

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

class TestServer
{
public:
	TestServer(const ServerSettings& settings, Log& log)
	:	m_settings(settings)
	,	m_log(log)
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
		m_server.start(m_settings, m_log, false, [this](DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint) -> BOOL { m_isServerReady = dwCurrentState == SERVICE_RUNNING; return TRUE; });
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
			wprintf(L"Running test %2u/%u '%s'...", testIndex, count, name.c_str());
			EACOPY_ASSERT(deleteDirectory(testSourceDir.c_str()));
			EACOPY_ASSERT(deleteDirectory(testDestDir.c_str()));
			EACOPY_ASSERT(ensureDirectory(testSourceDir.c_str()));
			EACOPY_ASSERT(ensureDirectory(testDestDir.c_str()));
			m_setupTime = 0;
			u64 startTime = getTimeMs();
			runImpl(loopIndex);
			u64 endTime = getTimeMs();
			wprintf(L"Done (%s)\n", toHourMinSec(endTime - startTime - m_setupTime).c_str());
			EACOPY_ASSERT(deleteDirectory(testSourceDir.c_str()));
			EACOPY_ASSERT(deleteDirectory(testDestDir.c_str()));
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
		if (wildcard)
			settings.filesOrWildcards.push_back(wildcard);
		return settings;
	}

	void createTestFile(const wchar_t* name, u64 size, bool source = true)
	{
		const WString& dir = source ? testSourceDir : testDestDir;
		createTestFile(dir.c_str(), name, size);
	}
	void createTestFile(const wchar_t* dir_, const wchar_t* name, u64 size, bool source = true)
	{
		u64 startSetupTime = getTimeMs();

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

		HANDLE file = CreateFileW(fullFileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		EACOPY_ASSERT(file != INVALID_HANDLE_VALUE);

		u64 dataSize = min(size, 1024*1024*256ull);
		char* data = new char[dataSize];
		for (u64 i=0; i!=dataSize; ++i)
			data[i] = 'a' + i % 26;
		u64 left = size;
		while (left)
		{
			u64 toWrite = min(dataSize, left);
			DWORD written;
			EACOPY_ASSERT(WriteFile(file, data, (DWORD)toWrite, &written, nullptr) == TRUE);
			EACOPY_ASSERT(toWrite == written);
			left -= written;
		}

		delete[] data;

		CloseHandle(file);

		m_setupTime += getTimeMs() - startSetupTime;
	}

	bool getGeneralFileExists(const wchar_t* fullFilePath)
	{
		WString str = L"";
		str.append(fullFilePath);
		return PathFileExistsW(str.c_str()) == TRUE;
	}

	bool getTestFileExists(const wchar_t* name, bool source = false)
	{
		WString str = source ? testSourceDir : testDestDir;
		str.append(L"\\").append(name);
		return PathFileExistsW(str.c_str()) == TRUE;
	}

	bool isEqual(const wchar_t* fileA, const wchar_t* fileB)
	{
		FileInfo a;
		FileInfo b;
		DWORD attrA = getFileInfo(a, fileA);
		DWORD attrB = getFileInfo(b, fileB);
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
		HANDLE fileHandle;
		(void)fileHandle; // suppress unused variable warning

		EACOPY_ASSERT(openFileWrite(fileName.c_str(), fileHandle, true));
		EACOPY_ASSERT(writeFile(fileName.c_str(), fileHandle, fileOrWildcard, strlen(fileOrWildcard)));
		EACOPY_ASSERT(closeFile(fileName.c_str(), fileHandle));
	}

	void setReadOnly(const wchar_t* file, bool readonly, bool source = true)
	{
		const WString dir = source ? testSourceDir : testDestDir;
		EACOPY_ASSERT(SetFileAttributesW((dir + file).c_str(), readonly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0);
	}

	void writeRandomData(const wchar_t* sourceFile, u64 fileSize)
	{
		HANDLE file = CreateFileW(sourceFile, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		EACOPY_ASSERT(file != INVALID_HANDLE_VALUE);
		for (uint i=0; i!=100; ++i)
		{
			char buffer[128];
			memset(buffer, 0, sizeof(buffer));
			uint writePos = max(0, ((uint(rand()) << 16) + uint(rand())) % (fileSize - 128));
			EACOPY_ASSERT(SetFilePointer(file, writePos, NULL, FILE_BEGIN) == writePos);
			EACOPY_ASSERT(WriteFile(file, buffer, sizeof(buffer), NULL, NULL) != 0);
		}
		CloseHandle(file);
	}

	u64 m_setupTime;
	Log clientLog;
	Log serverLog;
	WString name;
	WString testSourceDir;
	WString testDestDir;

	static TestBase* s_firstTest;
	static TestBase* s_lastTest;
	TestBase* m_nextTest = nullptr;
};

TestBase* TestBase::s_firstTest;
TestBase* TestBase::s_lastTest;


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define EACOPY_TEST_LOOP(name, loopCount)															\
	struct Test_##name : public TestBase															\
	{																								\
		Test_##name(bool isTemp) : TestBase(L#name, isTemp) {}										\
		virtual void runImpl(uint loopIndex) override;												\
		virtual uint runCount() override { return loopCount; }										\
	} test_##name(false);																			\
	void test##name() { uint index = 0; Test_##name test(true); test.run(index, test.runCount()); }	\
	void Test_##name::runImpl(uint loopIndex)														\


#define EACOPY_TEST(name) EACOPY_TEST_LOOP(name, 1)


#define EACOPY_REQUIRE_EXTERNAL_SHARE																\
	if (g_testExternalDestDir.empty())																\
	{																								\
		logInfoLinef(L"No external dest dir provided. Test skipped");								\
		return;																						\
	}																								\

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

EACOPY_TEST(UncPathOptimization)
{
	WString temp;
	const wchar_t* optimizedPath = optimizeUncPath(g_testDestDir.c_str(), temp);
	EACOPY_ASSERT(optimizedPath[0] && optimizedPath[1] == L':');
}

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

	EACOPY_ASSERT(copyFile((testSourceDir + L"Foo.txt").c_str(), (testDestDir + L"Foo.txt").c_str(), true, existed, bytesCopied, UseBufferedIO_Enabled));
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
	ensureDirectory((testSourceDir + L"\\Folder").c_str());

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.copySubdirDepth = 100;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"FOLDER").c_str()) != 0);
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

EACOPY_TEST(CopyFileDestLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
	EACOPY_ASSERT(!isSourceEqualDest(L"Foo.txt"));

	HANDLE destFile = CreateFileW((testDestDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
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
		CloseHandle(destFile);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyFileSourceSharedReadLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	HANDLE sourceFile = CreateFileW((testSourceDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
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
		CloseHandle(sourceFile);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyFileSourceReadLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	HANDLE sourceFile = CreateFileW((testSourceDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
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
		CloseHandle(sourceFile);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}

EACOPY_TEST(CopyFileSourceWriteLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	HANDLE sourceFile = CreateFileW((testSourceDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
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
		CloseHandle(sourceFile);
		return 0;
	});

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.retryCount > 0);
	EACOPY_ASSERT(clientStats.failCount == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	EACOPY_ASSERT(isSourceEqualDest(L"Foo.txt"));
}


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
	clientSettings.retryCount = 0;

	EACOPY_ASSERT(client.process(clientLog) != 0);
}

EACOPY_TEST(CopyFileListMissingOptional)
{
	createFileList(L"FileList.txt", "Foo.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.optionalWildcards.push_back(L"*.txt");
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
}

EACOPY_TEST(CopyFileListMissingAndFound)
{
	createTestFile(L"Bar.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt\nBar.txt");

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	Client client(clientSettings);
	clientSettings.retryCount = 0;

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
	clientSettings.retryCount = 0;

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
	createTestFile(L"DestFolder\\Boo.txt", 10, false);
	ensureDirectory((testSourceDir + L"SourceFolder").c_str());
	ensureDirectory((testSourceDir + L"SourceFolder2").c_str());
	createTestFile(L"SourceFolder2\\Boo.txt", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 3;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"Foo.txt") == true);
	EACOPY_ASSERT(getTestFileExists(L"Bar.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"a\\b\\Foo.txt") == true);
	EACOPY_ASSERT(getTestFileExists(L"a\\b\\Bar.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder") == false);
	EACOPY_ASSERT(getTestFileExists(L"DestFolder") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder2") == false);
}

EACOPY_TEST(CopyFileMirror)
{
	ensureDirectory((testSourceDir + L"SourceFolder").c_str());
	ensureDirectory((testSourceDir + L"SourceFolder2").c_str());
	createTestFile(L"SourceFolder2\\Boo.txt", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 3;
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder") == true);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder2") == true);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder2\\Boo.txt") == false);
}

EACOPY_TEST(CopyFileTargetDirectoryIsfile)
{
	ensureDirectory((testSourceDir + L"SourceFolder").c_str());
	createTestFile(L"SourceFolder/Foo", 10, true);
	createTestFile(L"SourceFolder", 10, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1;
	clientSettings.copyEmptySubdirectories = true;
	clientSettings.purgeDestination = true;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) != 0);
}

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

EACOPY_TEST(CopyFileWithVeryLongPath)
{
	//This test when run in Visual Studio must be have Visual Studio run as Administrator!
	WString longPath;
	for (uint i=0;i!=30; ++i)
		longPath.append(L"TestFolder\\");
	longPath.append(L"Foo.txt");
	createTestFile(longPath.c_str(), 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1000;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isSourceEqualDest(longPath.c_str()));
}

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

EACOPY_TEST(ServerCopyFolders)
{
	createTestFile(L"A\\Foo.txt", 10);
	createTestFile(L"B\\Bar.txt", 10);

	ServerSettings serverSettings;
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

	ServerSettings serverSettings;
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

EACOPY_TEST(ServerCopySmallFileDestIsLocal)
{
	std::swap(testSourceDir, testDestDir);
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings;
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

EACOPY_TEST(ServerCopyDirectoriesDestIsLocal)
{
	std::swap(testSourceDir, testDestDir);
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"A\\Bar.txt", 11);
	createTestFile(L"B\\Meh.txt", 12);

	ServerSettings serverSettings;
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

	ServerSettings serverSettings;
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

	ServerSettings serverSettings;
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
		createTestFile(fileName, 100);
	}

	for (uint i=0; i!=testCount; ++i)
	{
		ServerSettings serverSettings;
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.threadCount = 8;
		clientSettings.retryCount = 0;
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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	for (uint i=0; i!=50; ++i)
	{
		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir + L"\\" + iStr;
		clientSettings.retryCount = 0;
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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	ClientStats clientStats1;
	clientSettings.destDirectory = testDestDir + L"\\1";
	EACOPY_ASSERT(client.process(clientLog, clientStats1) == 0);
	EACOPY_ASSERT(clientStats1.copyCount == 1);

	ClientStats clientStats2;
	clientSettings.destDirectory = testDestDir + L"\\2";
	EACOPY_ASSERT(client.process(clientLog, clientStats2) == 0);
	EACOPY_ASSERT(clientStats2.linkCount == 1);
}

EACOPY_TEST(ServerCopySameDest)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings;
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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"\\1";
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	ClientSettings clientSettings2(getDefaultClientSettings());
	clientSettings2.useServer = UseServer_Required;
	clientSettings2.destDirectory = testDestDir + L"\\2";
	Client client2(clientSettings2);
	EACOPY_ASSERT(client2.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.linkCount == 1);

	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.skipCount == 1);
}

EACOPY_TEST(ServerCopyBadDest)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = L"\\\\localhost\\";
	clientSettings.retryWaitTimeMs = 1;
	clientSettings.retryCount = 0;
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

	ServerSettings serverSettings;
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
	HANDLE destFile = CreateFileW((testDestDir + L"Foo.txt").c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	EACOPY_ASSERT(destFile);

	ServerSettings serverSettings;
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
	createTestFile(L"DestFolder\\Boo.txt", 10, false);
	createTestFile(L"SourceFolder2\\Boo.txt", 10, false);

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.purgeDestination = true;
	clientSettings.useServer = UseServer_Required;
	Client client(clientSettings);

	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(getTestFileExists(L"Foo.txt") == false);
	EACOPY_ASSERT(getTestFileExists(L"DestFolder") == false);
	EACOPY_ASSERT(getTestFileExists(L"SourceFolder2") == false);
}

EACOPY_TEST(ServerReport)
{
	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings;
	clientSettings.destDirectory = testDestDir;
	Client client(clientSettings);
	EACOPY_ASSERT(client.reportServerStatus(clientLog) == 0);
}

EACOPY_TEST(ServerReportUsingBadPath)
{
	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings;
	clientSettings.destDirectory = L"\\\\localhost\\";
	Client client(clientSettings);
	EACOPY_ASSERT(client.reportServerStatus(clientLog) == -1);
}

EACOPY_TEST(ServerLinkNotExists)
{
	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"\\1";

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	deleteFile((testDestDir + L"\\1\\Foo.txt").c_str());

	clientSettings.destDirectory = testDestDir + L"\\2";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
}

EACOPY_TEST(ServerLinkModified)
{
	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.destDirectory = testDestDir + L"\\1";

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 1);

	deleteFile((testDestDir + L"\\1\\Foo.txt").c_str());
	createTestFile(L"1\\Foo.txt", 10, false);

	clientSettings.destDirectory = testDestDir + L"\\2";
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.copyCount == 2);
}

EACOPY_TEST(CopyFileWithDoubleSlashPath)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Test\\\\Foo.txt", fileSize);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.copySubdirDepth = 1;
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Test\\Foo.txt").c_str()) != 0);
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
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Test\\Test2\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
}

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
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Test\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Bar.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Test2\\Foo2.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 1000);

	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Test3\\NotFoo.xml").c_str()) == 0);
}

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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.deltaCompressionThreshold = 0;

	{
		clientSettings.destDirectory = testDestDir+ L"\\1";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	writeRandomData((testSourceDir + L"Foo.txt").c_str(), fileSize);

	{
		clientSettings.destDirectory = testDestDir+ L"\\2";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	FileInfo sourceFile;
	EACOPY_ASSERT(getFileInfo(sourceFile, (testSourceDir + L"Foo.txt").c_str()) != 0);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\2\\Foo.txt").c_str()) != 0);

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

	ServerSettings serverSettings;
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

EACOPY_TEST(ServerCopyFileListDestIsLocal)
{
	std::swap(testDestDir, testSourceDir);
	createTestFile(L"Foo.txt", 10);
	createFileList(L"FileList.txt", "Foo.txt");

	ServerSettings serverSettings;
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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings = getDefaultClientSettings(nullptr);
	clientSettings.useServer = UseServer_Required;
	clientSettings.filesOrWildcardsFiles.push_back(L"FileList.txt");
	clientSettings.retryCount = 0;

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) != 0);
}

EACOPY_TEST(CopyLargeFile)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.destDirectory = testDestDir+ iStr + L'\\';
		Client client(clientSettings);

		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + iStr + L"\\Foo.txt").c_str()) != 0);
		EACOPY_ASSERT(destFile.fileSize == fileSize);
	}
}

EACOPY_TEST(ServerCopyLargeFile)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		ServerSettings serverSettings;
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir+ L"\\" + iStr + L'\\';

		Client client(clientSettings);
		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\" + iStr + L"\\Foo.txt").c_str()) != 0);
		EACOPY_ASSERT(destFile.fileSize == fileSize);
	}
}

EACOPY_TEST(ServerCopyLargeFileCompressed)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		ServerSettings serverSettings;
		TestServer server(serverSettings, serverLog);
		server.waitReady();

		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.useServer = UseServer_Required;
		clientSettings.destDirectory = testDestDir+ L"\\" + iStr + L'\\';
		clientSettings.compressionEnabled = true;
		clientSettings.compressionLevel = 4;

		Client client(clientSettings);
		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\" + iStr + L"\\Foo.txt").c_str()) != 0);
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

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	for (uint i=0; i!=10000; ++i)
	{
		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings;
		clientSettings.sourceDirectory = testSourceDir;
		clientSettings.destDirectory = testDestDir + L"\\" + iStr + L'\\';
		clientSettings.filesOrWildcards.push_back(L"*.*");
		clientSettings.copySubdirDepth = 100;
		clientSettings.useServer = UseServer_Required;
		clientSettings.threadCount = 8;
		clientSettings.retryCount = 0;
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
	DWORD size = GetCurrentDirectoryW(1024, buffer);

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

EACOPY_TEST(FileGoingOverMaxPath)
{
	createTestFile(L"FooLongLongName.txt", 100);
	createTestFile(L"BarLongLongName.txt", 101);

	ClientSettings clientSettings(getDefaultClientSettings());
	while (clientSettings.destDirectory.size() <= MAX_PATH)
		clientSettings.destDirectory += L"FolderName\\";
	clientSettings.destDirectory.resize(246);
	clientSettings.destDirectory += L"\\";

	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isEqual((testSourceDir + L"FooLongLongName.txt").c_str(), (clientSettings.destDirectory + L"FooLongLongName.txt").c_str()));
	EACOPY_ASSERT(isEqual((testSourceDir + L"BarLongLongName.txt").c_str(), (clientSettings.destDirectory + L"BarLongLongName.txt").c_str()));
}

EACOPY_TEST(PathGoingOverMaxPath)
{
	createTestFile(L"Foo.txt", 100);

	ClientSettings clientSettings(getDefaultClientSettings());
	while (clientSettings.destDirectory.size() <= MAX_PATH + 100)
		clientSettings.destDirectory += L"wefwqwdqwdef\\";
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);
	EACOPY_ASSERT(isEqual((testSourceDir + L"Foo.txt").c_str(), (clientSettings.destDirectory + L"Foo.txt").c_str()));
}

EACOPY_TEST(ServerCopySmallFileExternalPath)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	const wchar_t* file = L"Foo.txt";
	createTestFile(file, 10);

	ServerSettings serverSettings;
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

EACOPY_TEST(FromServerCopySmallFileExternalPath)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	WString externalDest = g_testExternalDestDir + L'\\';
	ensureDirectory(externalDest.c_str());
	const wchar_t* file = L"Foo.txt";
	createTestFile(externalDest.c_str(), file, 10);

	ServerSettings serverSettings;
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

EACOPY_TEST(ServerCopySmallFileExternalPathUseHistory)
{
	EACOPY_REQUIRE_EXTERNAL_SHARE

	const wchar_t* file = L"Foo.txt";
	createTestFile(file, 1024*1024);

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;
	clientSettings.serverAddress = L"localhost";

	{
		WString externalDest = g_testExternalDestDir + L"\\A\\";
		deleteFile((externalDest + file).c_str(), false);
		clientSettings.destDirectory = externalDest;
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
		EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
	}
	{
		WString externalDest = g_testExternalDestDir + L"\\B\\";
		deleteFile((externalDest + file).c_str(), false);
		clientSettings.destDirectory = externalDest;
		Client client(clientSettings);

		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.linkCount == 1);
		EACOPY_ASSERT(isEqual((testSourceDir + file).c_str(), (externalDest + file).c_str()));
	}
}

void printHelp()
{
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef(L"  EACopyTest (Client v%S Server v%S) (c) Electronic Arts.  All Rights Reserved.", ClientVersion, ServerVersion);
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


int wmain(int argc, wchar_t* argv[])
{
	using namespace eacopy;

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

	// Run all the tests
	TestBase::runAll();

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
