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

#define DEFAULT_SOURCE_DIR  L"" //L"C:\\temp\\EACopyTest"
#define DEFAULT_DEST_DIR  L"" //L"\\\\localhost\\Tests\\EACopyTest"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Source directory used by unit tests.. Should be a absolute path on one of the local drives
WString g_testSourceDir = DEFAULT_SOURCE_DIR;

// Destination directory used by unit tests. Should be a network share on the local machine.
WString g_testDestDir = DEFAULT_DEST_DIR;


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
			EACOPY_ASSERT(!m_threadExited || !m_threadExited);
			Sleep(1);
		}
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

	void run(int index, int count)
	{
		testSourceDir = g_testSourceDir + L'\\' + name + L'\\';
		testDestDir = g_testDestDir + L'\\' + name + L'\\';

		bool log = false;
		if (log)
		{
			clientLog.init((name + L"_ClientLog.txt").c_str(), true);
			serverLog.init((name + L"_ServerLog.txt").c_str(), true);
		}
		wprintf(L"Running test %2u/%u '%s'...", index, count, name.c_str());
		EACOPY_ASSERT(deleteDirectory(testSourceDir.c_str()));
		EACOPY_ASSERT(deleteDirectory(testDestDir.c_str()));
		EACOPY_ASSERT(ensureDirectory(testSourceDir.c_str()));
		EACOPY_ASSERT(ensureDirectory(testDestDir.c_str()));
		m_setupTime = 0;
		u64 startTime = getTimeMs();
		runImpl();
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

	static void runAll()
	{
		uint testCount = 0;
		for (TestBase* it=s_firstTest; it; it = it->m_nextTest)
			++testCount;

		uint testIt = 0;
		for (TestBase* it=s_firstTest; it; it = it->m_nextTest)
			it->run(++testIt, testCount);
	}

	virtual void runImpl() = 0;

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
		u64 startSetupTime = getTimeMs();
		WString dir = source ? testSourceDir : testDestDir;

		const wchar_t* lastSlash = wcsrchr(name, L'\\');
		if (lastSlash)
		{
			dir.append(name, lastSlash - name);
			ensureDirectory(dir.c_str());
			dir += L'\\';
			name = lastSlash + 1;
		}

		//ensureDirectory((dir + name).c_str());
		HANDLE file = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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

	bool getTestFileExists(const wchar_t* name, bool source = false)
	{
		WString str = source ? testSourceDir : testDestDir;
		str.append(L"\\").append(name);
		return PathFileExistsW(str.c_str()) == TRUE;
	}

	void createFileList(const wchar_t* name, const char* fileOrWildcard)
	{
		WString fileName = testSourceDir + L'\\' + name;
		HANDLE fileHandle;
		(void)fileHandle; // suppress unused variable warning

		EACOPY_ASSERT(openFileWrite(fileName.c_str(), fileHandle, false));
		EACOPY_ASSERT(writeFile(fileName.c_str(), fileHandle, fileOrWildcard, strlen(fileOrWildcard)));
		EACOPY_ASSERT(closeFile(fileName.c_str(), fileHandle));
	}

	void setReadOnly(const wchar_t* file, bool readonly, bool source = true)
	{
		const WString dir = source ? testSourceDir : testDestDir;
		EACOPY_ASSERT(SetFileAttributesW((dir + file).c_str(), readonly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0);
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

#define EACOPY_TEST(name)											\
	struct Test_##name : public TestBase							\
	{																\
		Test_##name(bool isTemp) : TestBase(L#name, isTemp) {}		\
		virtual void runImpl() override;							\
	} test_##name(false);											\
	void test##name() { Test_##name test(true); test.run(1, 1); }	\
	void Test_##name::runImpl()										\


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

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);
}

EACOPY_TEST(CopyMediumFile)
{
	uint fileSize = 3*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	EACOPY_ASSERT(client.process(clientLog) == 0);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
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
}

EACOPY_TEST(OverwriteFile)
{
	createTestFile(L"Foo.txt", 100);
	createTestFile(L"Foo.txt", 101, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	Client client(clientSettings);
	ClientStats stats;
	EACOPY_ASSERT(client.process(clientLog, stats) == 0);
	EACOPY_ASSERT(stats.copyCount == 1);
	EACOPY_ASSERT(stats.copySize = 100);

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);
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

	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo1.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo2.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 100);
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

EACOPY_TEST(CopyFileFailWrite)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Bar.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
	setReadOnly(L"Foo.txt", true, false);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.retryWaitTimeMs = 1;
	clientSettings.retryCount = 2;

	Client client(clientSettings);
	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) != 0);
	EACOPY_ASSERT(clientStats.failCount == 1);
	EACOPY_ASSERT(clientStats.copyCount == 1);
}

EACOPY_TEST(CopyFileDestLockedAndThenUnlocked)
{
	createTestFile(L"Foo.txt", 10);
	createTestFile(L"Foo.txt", 100, false);
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
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
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
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Dir\\Bar.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 20);
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

EACOPY_TEST(ServerCopyAttemptFallback)
{
	createTestFile(L"Foo.txt", 10);

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Automatic;
	Client client(clientSettings);

	ClientStats clientStats;
	EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
	EACOPY_ASSERT(clientStats.serverAttempt == 1);
	EACOPY_ASSERT(clientStats.serverUsed == false);
	EACOPY_ASSERT(clientStats.copyCount == 1);
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
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
	EACOPY_ASSERT(clientStats.serverUsed == false);
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
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == 10);
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
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
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
	FileInfo destFile;
	EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	EACOPY_ASSERT(destFile.fileSize == fileSize);
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
	EACOPY_ASSERT(client.reportServerStatus(clientLog) == 0);
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
/*
EACOPY_TEST(ServerCopyDelta)
{
	createTestFile(L"Foo.txt", 10);

	ServerSettings serverSettings;
	TestServer server(serverSettings, serverLog);
	server.waitReady();

	ClientSettings clientSettings(getDefaultClientSettings());
	clientSettings.useServer = UseServer_Required;

	{
		clientSettings.destDirectory = testDestDir+ L"\\1";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	createTestFile(L"Foo.txt", 13);

	{
		clientSettings.destDirectory = testDestDir+ L"\\2";
		Client client(clientSettings);
		ClientStats clientStats;
		EACOPY_ASSERT(client.process(clientLog, clientStats) == 0);
		EACOPY_ASSERT(clientStats.copyCount == 1);
	}

	//FileInfo destFile;
	//EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\Foo.txt").c_str()) != 0);
	//EACOPY_ASSERT(destFile.fileSize == 10);
}
*/

EACOPY_TEST(CopyLargeFile)
{
	u64 fileSize = u64(INT_MAX) + 2*1024*1024 + 123;
	createTestFile(L"Foo.txt", fileSize);

	for (uint i=0; i!=1; ++i)
	{
		wchar_t iStr[10];
		_itow_s(i, iStr, eacopy_sizeof_array(iStr), 10);

		ClientSettings clientSettings(getDefaultClientSettings());
		clientSettings.destDirectory = testDestDir+ L"\\" + iStr;
		Client client(clientSettings);

		EACOPY_ASSERT(client.process(clientLog) == 0);

		FileInfo destFile;
		EACOPY_ASSERT(getFileInfo(destFile, (testDestDir + L"\\" + iStr + L"\\Foo.txt").c_str()) != 0);
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
		clientSettings.destDirectory = testDestDir+ L"\\" + iStr;

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
		clientSettings.destDirectory = testDestDir+ L"\\" + iStr;
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
		clientSettings.destDirectory = testDestDir + L"\\" + iStr;
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

	testServerCopyExistingDestAndFoundLinkSomewhereElse();

	// Run all the tests
	TestBase::runAll();

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
