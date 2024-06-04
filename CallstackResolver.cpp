// CallstackResolver.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <Windows.h>
#include <DbgHelp.h>

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <filesystem>
#include <array>

#include "Context.h"
#include "HttpGet.h"

#pragma comment(lib, "dbghelp.lib")

namespace {
	std::wstring GetLastErrorAsWString()
	{
		DWORD errorMessageID = ::GetLastError();
		if (errorMessageID == 0) {
			return std::wstring();
		}

		LPWSTR messageBuffer = nullptr;
		size_t size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);

		std::wstring message(messageBuffer, size);
		LocalFree(messageBuffer);

		return message;
	}

	std::filesystem::path GetExePath()
	{
		std::vector<wchar_t>    u16buf(1024, L'\0');

		GetModuleFileName(NULL, u16buf.data(), 1023);
		std::filesystem::path p(u16buf.data());

		if (!p.has_parent_path())
			return std::filesystem::path();
		return p.parent_path();
	}

	std::tuple<std::filesystem::path, std::wstring> SearchFile(const std::wstring_view& defaultFileName, bool is_optional, std::wstring argFileStr)
	{
		std::filesystem::path   targetPath;
		bool					isDefaultFile = false;

		if (!argFileStr.empty()) {
			// The file name or path is specified.
			std::filesystem::path p(argFileStr);
			if (!p.is_absolute()) {
				targetPath = std::filesystem::current_path() / p;
			}
			else {
				targetPath = p;
			}

			if (!std::filesystem::is_regular_file(targetPath)) {
				std::wstringstream ss;
				ss << L"Failed to find the file, \"" << targetPath.wstring() << L"\".";
				return { std::filesystem::path(), ss.str() };
			}
		}
		else {
			// default behavior. check the file with the default name placed next to the exe file.
			targetPath = GetExePath() / defaultFileName;
			if (!std::filesystem::is_regular_file(targetPath)) {
				if (!is_optional) {
					std::wstringstream ss;
					ss << L"Failed to find the file, \"" << targetPath.wstring() << L"\".";
					return { std::filesystem::path(), ss.str() };
				}
				else {
					// make it empty.
					targetPath.clear();
				}
			}
			else {
				isDefaultFile = true;
			}
		}

		std::wstring  retStr;
		if (isDefaultFile) {
			retStr += L"DefaultFile";
		}

		return { targetPath, retStr };
	}

	std::wstring ParseArguments(const int argc, const wchar_t** argv, bool& verbose, bool& json, bool& cin, std::wstring& configFile, std::wstring& textFile)
	{
		constexpr std::wstring_view flags[] = {L"--verbose", L"--json", L"--cin", L"--config", L"--text", };
		enum flagIdx : size_t {
			eVerbose = 0,
			eJson,
			eCin,
			eConfg,
			eText
		};

		verbose = json = cin = false;
		configFile.clear();
		textFile.clear();

		if (argc < 2)
			return std::wstring();

		std::list<std::wstring> args;
		for (size_t i = 0; i < argc; ++i) {
			args.push_back(argv[i]);
		}

		// Check if there is an unknown flag starts with "--"
		for (auto itr = ++args.begin(); itr != args.end(); ++itr) {
			auto& arg(*itr);


			if (arg.rfind(L"--", 0) == 0) {
				// start with "--"
				bool isAFlag = false;
				for (auto& f : flags) {
					// is a flag.
					if (arg.rfind(f, 0) == 0 && arg.length() == f.length()) {
						isAFlag = true;
						break;
					}
				}
				if (!isAFlag) {
					std::wstringstream ss;
					ss << L"Invlid flag dtected. \"" << arg << "\".";
					return ss.str();
				}
			}
		}

		auto stripDQ = [](const std::wstring& src) -> std::wstring {
			auto b = src.find_first_of(L'\"', 0);
			auto e = src.find_last_of(L'\"', 0);

			if (b != std::string::npos && e != std::string::npos)
				return src.substr(b + 1, e - b - 1);
			return src;
			};


		// parse arguments.
		for (auto itr = ++args.begin(); itr != args.end();) {
			auto& arg(*itr);

			auto checkFlag = [&](const std::wstring_view flag) -> bool {
				if (arg.rfind(flag, 0) == 0) {
					itr = args.erase(itr);
					return true;
				}
				return false;
				};

			std::wstring errStr;
			auto checkFlagAndArg = [&](const std::wstring_view flag, std::wstring& dst) -> bool {
				if (checkFlag(flag)) {
					// flag found. get next token.
					if (itr == args.end()) {
						std::wstringstream ss;
						ss << L"\"" << flag << L"\" needs to be set with an argument.";
						errStr = ss.str();
						return true;
					}
					dst = stripDQ(*itr);
					itr = args.erase(itr);
					return true;
				}
				return false;
				};

			if (checkFlag(flags[eVerbose])) {
				verbose = true;
				continue;
			}
			if (checkFlag(flags[eJson])) {
				json = true;
				continue;
			}
			if (checkFlag(flags[eCin])) {
				cin = true;
				continue;
			}
			if (checkFlagAndArg(flags[eConfg], configFile)) {
				if (!errStr.empty()) {
					return errStr;
				}
				continue;
			}
			if (checkFlagAndArg(flags[eText], textFile)) {
				if (!errStr.empty()) {
					return errStr;
				}
				continue;
			}

			++itr;
		}

		// If there are other arguments, they're input call stack string to resolve a symbol.
		{
			std::wstring uArgs;
			for (auto itr = ++args.begin(); itr != args.end(); ++itr) {
				uArgs += *itr + L" ";
			}
			std::wstringstream ss;
			ss << L"Unknown parameter detected. \"" << uArgs << L"\".";
			return ss.str();
		}

		return std::wstring();
	}
};


class CallstackResolver
{
public:
	class ImageInfo {
	public:
		std::filesystem::path               m_imagePath;
		size_t                              m_imageSize;
		std::wstring                        m_pdbPathString;
		GUID                                m_guid;
		uint32_t                            m_age;
		std::wstring                        m_pdbSignature;
		std::wstring                        m_serchedPDBPathString;
	};

	class PDBInfo {
	public:
		std::filesystem::path               m_pdbPath;
		uintptr_t                           m_allocatedMemAddr = 0;
		size_t                              m_allocatedMemSize = 0;
		std::map<uint64_t, std::wstring>    m_symbolTable;

		virtual ~PDBInfo()
		{
			if (m_allocatedMemAddr != 0) {
				free(reinterpret_cast<void*>(m_allocatedMemAddr));
				m_allocatedMemAddr = 0;
				m_allocatedMemSize = 0;
			}
		}
	};

public:
	std::wostream		m_verboseOut;

	uintptr_t			m_allocatedMemAddr = 0; // dummy memory space used when mapping a PDB.
	static const size_t		m_allocatedMemSize = 2048u * 1024u * 1024u; // 2GB
	HANDLE				m_hDbgHelp = 0;

	std::map<std::wstring, std::unique_ptr<ImageInfo>>      m_imageList;
	std::map<std::wstring, std::unique_ptr<PDBInfo>>        m_loadedPDBList;
	std::list<std::filesystem::path>                        m_pdbStorageList;
	std::list<std::filesystem::path>                        m_pdbPathList;
	std::list<std::tuple<std::wstring, std::filesystem::path>>                        m_symbolServerList;

public:
	CallstackResolver() :
		m_verboseOut(nullptr)
	{
	};

	~CallstackResolver()
	{
		// nullify before destruction.
		m_verboseOut.rdbuf(nullptr);
	};

	std::wstring Init()
	{
		if (m_hDbgHelp != 0) {
			return L"Failed to initialize dbghelp module. It has already been initialized.";
		}

		DWORD options;
		options = SymGetOptions();
		options &= ~SYMOPT_DEFERRED_LOADS;
		options |= SYMOPT_LOAD_LINES;
		options |= SYMOPT_IGNORE_NT_SYMPATH;
		options |= SYMOPT_DEBUG;
		options |= SYMOPT_UNDNAME;
		SymSetOptions(options);

		BOOL isOK = SymInitialize(m_hDbgHelp, NULL, FALSE);
		if (!isOK) {
			m_hDbgHelp = 0;
			return L"Failed to initialize dbghelp module.";
		}

		m_allocatedMemAddr = reinterpret_cast<uintptr_t>(malloc(m_allocatedMemSize));

		return std::wstring();
	}

	std::wstring Finalize()
	{
		std::wstringstream ss;

		if (m_hDbgHelp != 0) {
			for (auto& itr : m_loadedPDBList) {
				if (!SymUnloadModule64(m_hDbgHelp, itr.second->m_allocatedMemAddr)) {
					ss << L"Failed to unload module \"" << itr.second->m_pdbPath << L"\". The last error was: " << GetLastErrorAsWString() << L" ";
				}
			}
			m_loadedPDBList.clear();

			SymCleanup(m_hDbgHelp);
			m_hDbgHelp = 0;
		}

		if (m_allocatedMemAddr != 0) {
			free(reinterpret_cast<void*>(m_allocatedMemAddr));
			m_allocatedMemAddr = 0;
		}

		return ss.str();
	}

	std::wstring LoadPDB(const std::filesystem::path& pdbFilePath_arg)
	{
		auto pdbFilePath = pdbFilePath_arg;
		pdbFilePath = pdbFilePath.make_preferred().lexically_normal();

		if (pdbFilePath.extension() != L".pdb" && pdbFilePath.extension() != L".PDB") {
			std::wstringstream ss;
			ss << L"Invalid pdb/PDB file name detected. \"" << pdbFilePath << L"\". ";
			return ss.str();
		}

		// Already have loaded the PDB.
		if (m_loadedPDBList.find(pdbFilePath.replace_extension(L".pdb")) != m_loadedPDBList.end()) {
			return std::wstring();
		}

		m_verboseOut << L"Loading PDB..  " << pdbFilePath << std::endl;

		{
			uintptr_t baseAddr = SymLoadModuleExW(
				m_hDbgHelp,							// handle to the process
				NULL,								// file handle
				pdbFilePath.wstring().c_str(),      // image name (.pdb, .dll, .exe ...)
				NULL,								// module name (shortcut name)
				m_allocatedMemAddr,					// base address. cannot be zero when loading a PDB.
				(DWORD)m_allocatedMemSize,			// DLL size, this cannot be zero when loading a PDB.
				NULL,								// pointer to MODLAOD_DATA. can be null.
				0);									// flags.

			if (baseAddr == 0) {
				std::wstringstream ss;
				ss << L"Failed to load a PDB file, \"" << pdbFilePath.wstring() << L"\". The last error was: " << GetLastErrorAsWString();
				return ss.str();
			}
		}

		std::unique_ptr<PDBInfo> loadingPDB = std::make_unique<PDBInfo>();
		loadingPDB->m_pdbPath = pdbFilePath;

		// Estimate the DLL image size and allocate memory.
		{
			IMAGEHLP_SYMBOL64_PACKAGE lastSymbol = { sizeof(IMAGEHLP_SYMBOL64) , };
			DWORD64 displacement = 0;
			lastSymbol.sym.MaxNameLength = sizeof(IMAGEHLP_SYMBOL64_PACKAGE) - sizeof(IMAGEHLP_SYMBOL64);

			if (!SymGetSymFromAddr64(m_hDbgHelp, m_allocatedMemAddr + m_allocatedMemSize - 1, &displacement, &lastSymbol.sym)) {
				std::wstringstream ss;
				ss << L"Failed to get the last symbol of the module \"" << pdbFilePath << "\". The last error was: " << GetLastErrorAsWString();
				return ss.str();
			}

			loadingPDB->m_allocatedMemSize = (((lastSymbol.sym.Address - m_allocatedMemAddr) >> 20) + 2) << 20; // + 1MB padding.

			m_verboseOut << L"Estimated image size. 0x" << std::hex << loadingPDB->m_allocatedMemSize << L" (" << std::dec << loadingPDB->m_allocatedMemSize / (1024u * 1024u) << L" MB)" << std::endl;

			loadingPDB->m_allocatedMemAddr = reinterpret_cast<uintptr_t>(malloc(loadingPDB->m_allocatedMemSize));
			if (loadingPDB->m_allocatedMemAddr == 0) {
				std::wstringstream ss;
				ss << L"Failed to allocate memory for the module \"" << pdbFilePath << "\". ";
				return ss.str();
			}
		}

		// unload module once.
		if (!SymUnloadModule64(m_hDbgHelp, m_allocatedMemAddr)) {
			std::wstringstream ss;
			ss << L"Failed to unload module \"" << pdbFilePath.wstring() << "\". The last error was: " << GetLastErrorAsWString();
			return ss.str();
		}

		// load again with the newly allocated memory.
		{
			uintptr_t baseAddr = SymLoadModuleExW(
				m_hDbgHelp,
				NULL,
				pdbFilePath.wstring().c_str(),
				NULL,
				loadingPDB->m_allocatedMemAddr,
				(DWORD)loadingPDB->m_allocatedMemSize,
				NULL,
				0);

			if (baseAddr == 0) {
				std::wstringstream ss;
				ss << L"Failed to load module \"" << pdbFilePath.wstring() << "\" The last error was: " << GetLastErrorAsWString();
				return ss.str();
			}
		}

		m_loadedPDBList.insert({ loadingPDB->m_pdbPath.replace_extension(L".pdb"), std::move(loadingPDB) });

		return std::wstring();
	}

	std::wstring LoadImage(const std::filesystem::path& imageFilePath_arg)
	{
		auto imageFilePath = imageFilePath_arg;
		imageFilePath = imageFilePath.make_preferred().lexically_normal();

		std::unique_ptr<ImageInfo> imageInfo = std::make_unique<ImageInfo>();
		imageInfo->m_imagePath = imageFilePath;

		SYMSRV_INDEX_INFOW info = { sizeof(SYMSRV_INDEX_INFOW), };
		if (!SymSrvGetFileIndexInfoW(imageFilePath.wstring().c_str(), &info, 0)) {
			std::wstringstream ss;
			ss << L"Failed to get an image file info for \"" << imageFilePath.wstring() << L"\". The last error was: " << GetLastErrorAsWString();
			return ss.str();
		}

		imageInfo->m_pdbPathString = info.pdbfile;
		imageInfo->m_imageSize = info.size;
		imageInfo->m_guid = info.guid;
		imageInfo->m_age = info.age;

		// Build a PDB Sinature.
		{
			std::vector<wchar_t>    u16buf(1024, L'\0');
			auto& gid(imageInfo->m_guid);
			swprintf_s(u16buf.data(), u16buf.size(), L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
				gid.Data1, gid.Data2, gid.Data3,
				gid.Data4[0], gid.Data4[1], gid.Data4[2], gid.Data4[3],
				gid.Data4[4], gid.Data4[5], gid.Data4[6], gid.Data4[7], imageInfo->m_age);

			imageInfo->m_pdbSignature = u16buf.data();
		}

		m_imageList.insert({ imageFilePath, std::move(imageInfo) });

		return std::wstring();
	}

	std::wstring SearchPDBfromImage(Context::resolved_callstack& cs)
	{
		if (cs.pdb.has_value())
			return std::wstring();

		const auto& imageName = cs.image.value();

		// load image if needed.
		bool isFirstTime = false;
		auto ilItr = m_imageList.find(imageName);
		if (ilItr == m_imageList.end()) {
			isFirstTime = true;
			auto errStr = LoadImage(imageName);
			if (!errStr.empty()) {
				return errStr;
			}
			ilItr = m_imageList.find(imageName);
		}
		auto& imageInfo = ilItr->second;

		// Already have the PDB information.
		if (!imageInfo->m_serchedPDBPathString.empty()) {
			// m_verboseOut << L"The PDB already has been loaded. " << imageInfo->m_serchedPDBPathString << std::endl;

			cs.pdb = imageInfo->m_serchedPDBPathString;
			cs.pdb_signature = imageInfo->m_pdbSignature;
			return std::wstring();
		}

		std::filesystem::path pdbName(imageName);
		pdbName = pdbName.filename().replace_extension(L".pdb");

		std::filesystem::path symbolCacheDirName = pdbName / std::filesystem::path(imageInfo->m_pdbSignature) / pdbName;

		// 1. Search under the the pdb cache storeages.
		for (auto& pdbStorage : m_pdbStorageList) {
			std::filesystem::path pdbFullpath = pdbStorage / symbolCacheDirName;

			m_verboseOut << L"Checking PDB.. " << pdbFullpath << ". ";

			if (std::filesystem::exists(pdbFullpath)) {
				// found. update searched path.
				m_verboseOut << L"Found. " << std::endl;

				imageInfo->m_serchedPDBPathString = pdbFullpath.wstring();
				cs.pdb = imageInfo->m_serchedPDBPathString;
				cs.pdb_signature = imageInfo->m_pdbSignature;
				return std::wstring();
			}
			else {
				// Not found.
				m_verboseOut << std::endl;
			}
		}

		// 2. Search under the pdb path directly.
		if (!cs.pdb.has_value()) {
			for (auto& pdbPath : m_pdbPathList) {
				std::filesystem::path pdbFullpath = pdbPath / pdbName;
				m_verboseOut << L"Checking PDB.. " << pdbFullpath << ". ";

				if (std::filesystem::exists(pdbFullpath)) {
					m_verboseOut << L"Found. " << std::endl;

					imageInfo->m_serchedPDBPathString = pdbFullpath.wstring();
					cs.pdb = imageInfo->m_serchedPDBPathString;
					cs.pdb_signature = imageInfo->m_pdbSignature;
					return std::wstring();
				}
				else {
					// Not found.
					m_verboseOut << std::endl;
				}
			}
		}

		if (isFirstTime) {
			// When the first time image load, try to access the symbol servers.
			// 3. server
			if (!cs.pdb.has_value()) {
				for (const auto& [url, cache] : m_symbolServerList) {
					// 1. Search under the path as a symbol cache storeage.
					std::wstring getReqURL = url;
					getReqURL += L"/";
					getReqURL += pdbName;
					getReqURL += L"/";
					getReqURL += imageInfo->m_pdbSignature;
					getReqURL += L"/";
					getReqURL += pdbName;

					std::filesystem::path destPath = cache;

					if (!std::filesystem::exists(destPath)) {
						std::wstringstream ss;
						ss << L"Invalid synbol server cache detected.. \"" << destPath.wstring() << "\".";
						return ss.str();
					}
					destPath /= symbolCacheDirName;
					if (std::filesystem::exists(destPath)) {
						std::wstringstream ss;
						ss << L"Symbol server cache already have the PDB. \"" << destPath.wstring() << "\"";
						return ss.str();
					}
					m_verboseOut << L"[HttpGet]:" << getReqURL << std::endl;

					{
						auto errStr = HttpGet::Get(getReqURL, destPath, m_verboseOut);
						if (errStr.empty()) {
							break;
						}
						else {
							// Ignoring errors of HTTP Get request. i.e. 404
							m_verboseOut << L"[HttpGet] Failed. " << errStr << std::endl;
							continue;
						}
					}
				}
			}

			// 4. Search under the the pdb cache storeages again.
			for (auto& pdbStorage : m_pdbStorageList) {
				std::filesystem::path pdbFullpath = pdbStorage / symbolCacheDirName;

				m_verboseOut << L"Checking PDB.. " << pdbFullpath << ". ";

				if (std::filesystem::exists(pdbFullpath)) {
					// found. update searched path.
					m_verboseOut << L"Found. " << std::endl;
					imageInfo->m_serchedPDBPathString = pdbFullpath.wstring();
					cs.pdb = imageInfo->m_serchedPDBPathString;
					cs.pdb_signature = imageInfo->m_pdbSignature;
					return std::wstring();
				}
				else {
					// Not found.
					m_verboseOut << std::endl;
				}
			}
		}

		std::wstringstream ss;
		ss << L"Failed to find the PDB for \"" << imageName << "\".";
		return ss.str();
	}

	std::wstring Resolve(Context::resolved_callstack& cs)
	{
		if (cs.isComment)
			return std::wstring();

		// Search and load the PDB.
		if (!cs.pdb.has_value()) {
			auto errStr = SearchPDBfromImage(cs);
			if (!errStr.empty()) {
				return errStr;
			}
		}
		{
			auto errStr = LoadPDB(cs.pdb.value());
			if (!errStr.empty()) {
				return errStr;
			}
		}

		const auto& pdbName = cs.pdb.value();

		// make sure the PDB has been loaded.
		auto pdbItr = m_loadedPDBList.find(pdbName);
		if (pdbItr == m_loadedPDBList.end()) {
			std::wstringstream ss;
			ss << L"Failed to find a loaded PDB to resolve a symbol \"" << pdbName << L"\".";
			return ss.str();
		}

		const auto& offsetAddr = cs.values.image_offset.value();
		DWORD64 targetAddr = pdbItr->second->m_allocatedMemAddr + offsetAddr;

		// Search the address using the Sym function.
		{
			DWORD64 displacement = 0;
			IMAGEHLP_SYMBOL64_PACKAGE symbol = { sizeof(IMAGEHLP_SYMBOL64) , };
			symbol.sym.MaxNameLength = sizeof(IMAGEHLP_SYMBOL64_PACKAGE) - sizeof(IMAGEHLP_SYMBOL64);


			if (!SymGetSymFromAddr64(m_hDbgHelp, targetAddr, &displacement, &symbol.sym)) {
				std::wstringstream ss;
				ss << L"Failed to get a symbol info in \"" << pdbName << L"\" with offset" << std::hex << L"0x" << offsetAddr << L". ";
				return ss.str();
			}

			std::string u8name = symbol.sym.Name;
			if (u8name.length() > 0) {
				std::vector<wchar_t>    u16buf(u8name.size() + 16, L'\0');
				if (MultiByteToWideChar(CP_UTF8, 0, u8name.c_str(), (int)u8name.length(), u16buf.data(), (int)u16buf.size()) > 0) {
					cs.function = u16buf.data();
					cs.values.function_offset = targetAddr - symbol.sym.Address;
				}
			}
		}

		// Search line info if available.
		{
			DWORD displacement = 0;
			IMAGEHLP_LINEW64 lineInfo = { sizeof(IMAGEHLP_LINEW64) , };

			if (!SymGetLineFromAddrW64(m_hDbgHelp, targetAddr, &displacement, &lineInfo)) {
				// A PDB which doesn't have line info.  
				cs.line.reset();
				cs.values.line_no.reset();
				cs.values.line_offset.reset();
			}
			else {
				cs.line = lineInfo.FileName;
				cs.values.line_no = lineInfo.LineNumber;
				cs.values.line_offset = targetAddr - lineInfo.Address;
			}
		}

		return std::wstring();
	}

	int Run(int argc, const wchar_t** argv)
	{
		Context ctx;

		// Parse input arguments.
		bool    verbose = false, json_out = false, use_cin = false;
		std::wstring argConfigFileStr, argTextFileStr;
		{
			auto errStr = ParseArguments(argc, argv, verbose, json_out, use_cin, argConfigFileStr, argTextFileStr);
			if (!errStr.empty()) {
				std::wcerr << L"Failed to parse arguments. " << errStr << std::endl;
				return 1;
			}
		}

		// Parse input config flie.
		if (use_cin) {
			// JSON will come from std::cin.
			auto exePath = GetExePath();

			auto errStr = ctx.ParseInputConfig(std::cin, exePath.parent_path());
			if (!errStr.empty()) {
				std::wcerr << L"Failed to parse input from the standard input. " << errStr << std::endl;
				return 1;
			}
		}
		else {
			constexpr std::wstring_view config_default_name = L"config.json";
			std::filesystem::path configPath;
			{
				auto [ret_configPath, errStr] = SearchFile(config_default_name, false, argConfigFileStr);
				if (ret_configPath.empty()) {
					std::wcerr << L"Failed to find a config file. " << errStr << std::endl;
					return 1;
				}
				std::swap(ret_configPath, configPath);
				if (!json_out && errStr == L"DefaultFile") {
					std::wcout << L"Using default config file, \"" << configPath.wstring() << "\"." << std::endl;
				}
			}

			{
				auto errStr = ctx.ParseInputConfig(configPath);
				if (!errStr.empty()) {
					std::wcerr << L"Failed to parse a config file, \"" << configPath.wstring() << L"\". " << errStr << std::endl;
					return 1;
				}
			}
		}

		// Parse input text. (Optional)
		if (!use_cin && ctx.callstacks.empty()) { // When using cin, all input call stacks should come through the JSON format.
			constexpr std::wstring_view text_default_name = L"callstacks.txt";
			std::filesystem::path textPath;
			{
				auto [ret_textPath, errStr] = SearchFile(text_default_name, true, argTextFileStr);
				if (ret_textPath.empty()) {
					std::wcerr << L"Failed to find the input text file. " << errStr << std::endl;
					return 1;
				}
				std::swap(ret_textPath, textPath);
				if (!json_out && errStr == L"DefaultFile") {
					std::wcerr << L"Using default input callstack file, \"" << textPath.wstring() << "\"." << std::endl;
				}
			}

			if (!textPath.empty()) {
				// Found the input text.
				auto errStr = ctx.ParseInputText(textPath);
				if (!errStr.empty()) {
					std::wcerr << L"Failed to parse an input text file, \"" << textPath.wstring() << L"\". " << errStr << std::endl;
					return 1;
				}
			}
		}

		if (ctx.symbols.size() == 0) {
			std::wcerr << L"There was no symbol storage in the configuration." << std::endl;
			return 1;
		}
		if (ctx.callstacks.size() == 0) {
			std::wcerr << L"There was no call stack to resolve." << std::endl;
			return 1;
		}

		// Parse input call stack strings and add resolved_callstack objects here.
		{
			auto errStr = ctx.ParseCallstacks(false);
			if (!errStr.empty()) {
				std::wcerr << errStr << std::endl;
				return 1;
			}
		}

		if (verbose) {
			// set verbose output.
			m_verboseOut.rdbuf(std::wcout.rdbuf());
		}

		m_verboseOut << L"--- input context ---" << std::endl;
		m_verboseOut << ctx;
		m_verboseOut << L"---------------------" << std::endl;

		{
			auto errStr = Init();
			if (!errStr.empty()) {
				std::wcerr << L"Failed to initialize a CallstackResolver instance. Perhaps missing dbghelp.dll on your system. You may need to install a Windows SDK." << std::endl;
				std::wcerr << errStr << std::endl;
				return 1;
			}
		}

		for (const auto& s : ctx.symbols) {
			if (s.server.has_value() && s.cache.has_value()) {
				m_symbolServerList.push_back({ s.server.value(), s.cache.value() });
				m_symbolServerList.unique();
				m_pdbStorageList.push_back(s.cache.value());
				m_pdbStorageList.unique();
			}
			if (s.direct.has_value()) {
				m_pdbPathList.push_back(s.direct.value());
				m_pdbPathList.unique();
			}
		}

		for (auto& cs : ctx.resolved_callstacks) {
			auto errStr = Resolve(cs);
			if (!errStr.empty()) {
				std::wcerr << L"Failed to resolve symbol. " << errStr << std::endl;
				continue;
			}
		}

		if (json_out) {
			std::wcout << ctx;
		}
		else {
			std::wcout << ctx.DumpResolvedInReadable();
		}

		{
			auto errStr = Finalize();
			if (!errStr.empty()) {
				std::wcerr << L"Failed to finalize a CallstackResolver instance." << std::endl;
				std::wcerr << errStr << std::endl;
				return 1;
			}
		}

		return 0;
	};
};

int wmain(int argc, const wchar_t **argv)
{
    CallstackResolver cr;

    return cr.Run(argc, argv);
}
