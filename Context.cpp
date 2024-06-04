#include <Windows.h>

#include <iostream>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
#include <regex>

#include "picojson/picojson.h"

#include "Context.h"

namespace {
    constexpr std::string_view  symbols_s("symbols");
    constexpr std::wstring_view  symbols_ws(L"symbols");
    constexpr std::string_view  paths_s("paths");
    constexpr std::wstring_view  paths_ws(L"paths");
    constexpr std::string_view  callstacks_s("callstacks");
    constexpr std::wstring_view  callstacks_ws(L"callstacks");
    constexpr std::string_view  resolved_callstacks_s("resolved_callstacks");
    constexpr std::wstring_view  resolved_callstacks_ws(L"resolved_callstacks");

    std::wstring Utf8ToUtf16(const std::string& u8)
    {
        if (u8.length() > 0) {
            std::vector u16(u8.length() * 2, L'\0');

            if (MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.length(), u16.data(), (int)u16.size()) > 0) {
                return std::wstring(u16.data());
            }
        }

        return std::wstring();
    }

    std::string Utf16ToUtf8(const std::wstring& u16)
    {
        if (u16.length() > 0) {
            std::vector u8(u16.length() * 4, '\0');

            if (WideCharToMultiByte(CP_UTF8, 0, u16.c_str(), -1, u8.data(), (int)u8.size(), NULL, NULL) > 0) {
                return std::string(u8.data());
            }
        }

        return std::string();
    }

    std::optional<uint64_t> ParseNumber(const std::wstring& s)
    {
        try {
            auto toValue = [](const std::wstring &arg_s) -> std::optional<uint64_t> {
                uint64_t v;

                // Trim space
                auto b = arg_s.find_first_not_of(L" ");
                auto e = arg_s.find_last_not_of(L" ");
                const std::wstring_view s = std::wstring_view(arg_s).substr(b, e - b + 1);
 
                if (s.rfind(L"0x") == 0) {
                    // hex.
                    std::wstringstream ss;
                    ss << std::hex << s.substr(2, std::string::npos);
                    ss >> v;
                    if (ss.fail()) {
                        return std::nullopt;
                    }
                    return v;
                }
                else {
                    // dec.
                    std::wstringstream ss;
                    ss << std::dec << s;
                    ss >> v;
                    if (ss.fail()) {
                        return std::nullopt;
                    }
                    return v;
                }
            };

            {
                // contain plus.
                size_t pPos = s.find('+');
                if (pPos != std::string::npos) {
                    std::wstring valStr = s.substr(0, pPos);
                    std::wstring latterPart = s.substr(pPos + 1);

                    auto vl = toValue(valStr);
                    auto vr = ParseNumber(latterPart);
                    if (!vl.has_value() || !vr.has_value()) {
                        return std::nullopt;
                    }
                    vl.value() += vr.value();
                    return vl;
                }
            }
            {
                // contain minus.
                size_t mPos = s.find('-');
                if (mPos != std::string::npos) {
                    std::wstring valStr = s.substr(0, mPos);
                    std::wstring latterPart = s.substr(mPos + 1);

                    auto vl = toValue(valStr);
                    auto vr = ParseNumber(latterPart);
                    if (!vl.has_value() || !vr.has_value()) {
                        return std::nullopt;
                    }
                    vl.value() -= vr.value();
                    return vl;
                }
            }
            return toValue(s);
        }
        catch (...)
        {
        }
        return std::nullopt;
    }

    std::optional<std::wstring> ConvertToStr(const std::optional<uint64_t>& v, bool hex)
    {
        if (!v.has_value())
            return std::nullopt;

        std::wstringstream ss;
        if (hex)
            ss << L"0x" << std::hex;
        ss << v.value();

        return ss.str();
    }

    std::wstring ResolveDir(std::wstring& pathStr, const std::filesystem::path& rootPath, bool forceCreate = false)
    {
        std::filesystem::path p(pathStr);
        if (!p.is_absolute()) {
            if (rootPath.empty()) {
                std::wstringstream ss;
                ss << L"Relative path can't be used without a config file. " "\"" << p.wstring() << "\"";
                return ss.str();
            }
            p = rootPath / p;
            p = p.lexically_normal();
        }
        if (forceCreate && !std::filesystem::exists(p)) {
            // create directory.
            try {
                std::filesystem::create_directories(p);
            }
            catch (std::filesystem::filesystem_error& err) {
                std::wstringstream ss;
                ss << L"Failed to create a directory. \"" << p.wstring() << "\". The reason was.. " << err.what();
                return ss.str();
            }
        }

        if (!std::filesystem::is_directory(p)) {
            std::wstringstream ss;
            ss << "\"" << p.wstring() << "\" was not a valid directory.";
            return ss.str();
        }
        pathStr = p.wstring();

        return std::wstring();
    };

    std::wstring resolveFile(std::wstring& pathStr, const std::filesystem::path& rootPath)
    {
        std::filesystem::path p(pathStr);
        if (!p.is_absolute()) {
            if (rootPath.empty()) {
                std::wstringstream ss;
                ss << L"Relative path can't be used without a config file. " "\"" << p.wstring() << "\"";
                return ss.str();
            }
            p = rootPath / p;
            p = p.lexically_normal();
        }
        if (!std::filesystem::exists(p)) {
            std::wstringstream ss;
            ss << "\"" << p.wstring() << "\" didn't exist.";
            return ss.str();
        }
        if (!std::filesystem::is_regular_file(p)) {
            std::wstringstream ss;
            ss << "\"" << p.wstring() << "\" was not a regular file.";
            return ss.str();
        }
        pathStr = p.wstring();

        return std::wstring();
    };

    std::tuple<std::optional<Context::symbol>, std::wstring> parseSymbol(const picojson::object& o, const std::filesystem::path &rootPath)
    {
        Context::symbol s;

        for (picojson::object::const_iterator i = o.begin(); i != o.end(); ++i) {
            const auto& key(i->first);
            const auto& value(i->second);

            if (key == "force_create_cache_dir") {
                s.force_create_cache_dir = value.get<bool>();
            }

            if (!value.is<std::string>())
                continue;

            if (key == "server") {
                s.server = Utf8ToUtf16(value.get<std::string>());
            }
            else if (key == "cache") {
                s.cache = Utf8ToUtf16(value.get<std::string>());
            }
            else if (key == "direct") {
                s.direct = Utf8ToUtf16(value.get<std::string>());
            }
        }

        if (s.server != std::nullopt && s.cache == std::nullopt) {
            return { std::nullopt, L"Valid \"cache\" directory is needed when specifying a server." };
        }
        if (s.server == std::nullopt && s.cache == std::nullopt && s.direct == std::nullopt) {
            // empty but not an error.
            return {std::nullopt, std::wstring()};
        }
        // resolve and check paths.

        if (s.cache.has_value()) {
            bool forceCreate = false;
            if (s.force_create_cache_dir.has_value()) {
                forceCreate = s.force_create_cache_dir.value();
            }
            auto errStr = ResolveDir(s.cache.value(), rootPath, forceCreate);
            if (!errStr.empty()) {
                return { std::nullopt, errStr };
            }
        }
        if (s.direct.has_value()) {
            auto errStr = ResolveDir(s.direct.value(), rootPath);
            if (!errStr.empty()) {
                return { std::nullopt, errStr };
            }
        }

        return {s, std::wstring()};
    }

    void prepareToWrite(Context::resolved_callstack& c)
    {
        c.image_offset = ConvertToStr(c.values.image_offset, true);
        c.function_offset = ConvertToStr(c.values.function_offset, true);
        c.line_no = ConvertToStr(c.values.line_no, false);
        c.line_offset = ConvertToStr(c.values.line_offset, true);
    }

    std::wostream& operator<<(std::wostream& os, std::pair<std::wstring&, const Context::resolved_callstack&> p)
    {
        auto [prefix, c] = p;

        os << prefix << L"{" << std::endl;
        prefix += L"  ";
        bool flushLine = false;

        auto outOptionalDQ = [&](const std::optional<std::wstring>& s, const wchar_t* name) {
                if (s.has_value()) {
                    if (flushLine) {
                        os << "," << std::endl;
                        flushLine = false;
                    }
                    os << prefix << L"\"" << name << "\" : \"" << std::regex_replace(*s, std::wregex(L"\\\\"), L"\\\\") << "\"";
                    flushLine = true;
                }
            };

        if (c.isComment) {
            outOptionalDQ(c.image, L"comment");
        }
        else {
            outOptionalDQ(c.image, L"image");
            outOptionalDQ(c.pdb, L"pdb");
            outOptionalDQ(c.pdb_signature, L"pdb_signature");
            outOptionalDQ(c.image_offset, L"image_offset");
            outOptionalDQ(c.function, L"function");
            outOptionalDQ(c.function_offset, L"function_offset");
            outOptionalDQ(c.line, L"line");
            outOptionalDQ(c.line_no, L"line_no");
            outOptionalDQ(c.line_offset, L"line_offset");
        }

        prefix = prefix.substr(0, prefix.length() - 2);
        if (flushLine) {
            os << std::endl;
        }
        os << prefix << L"}";

        return os;
    }

    std::wostream& operator<<(std::wostream& os, std::pair<std::wstring &, const Context::symbol&> p)
    {
        auto [prefix, s] = p;

        os << prefix << L"{" << std::endl;
        prefix += L"  ";
        bool flushLine = false;

        auto outOptionalDQ = [&]<typename T>(const std::optional<T>&s, const wchar_t* name)
        {
            if (s.has_value()) {
                if (flushLine) {
                    os << L"," << std::endl;
                    flushLine = false;
                }
                os << prefix << L"\"" << name << "\" : \"" << std::regex_replace(*s, std::wregex(L"\\\\"), L"\\\\") << "\"";
                flushLine = true;
            }
        };
        auto outOptional = [&]<typename T>(const std::optional<T>&s, const wchar_t* name)
        {
            if (s.has_value()) {
                if (flushLine) {
                    os << L"," << std::endl;
                    flushLine = false;
                }
                os << std::boolalpha << prefix << L"\"" << name << "\" : " << *s;
                flushLine = true;
            }
        };

        outOptionalDQ(s.server, L"server");
        outOptionalDQ(s.cache, L"cache");
        outOptionalDQ(s.direct, L"direct");
        outOptional(s.force_create_cache_dir, L"force_create_cache_dir");

        prefix = prefix.substr(0, prefix.length() - 2);
        if (flushLine) {
            os << std::endl;
        }
        os << prefix << L"}";

        return os;
    }
};

std::wostream& operator<<(std::wostream& os, Context& ctx)
{
    for (auto& c : ctx.resolved_callstacks) {
        prepareToWrite(c);
    }

    std::wstring prefix;

    os << prefix << L"{" << std::endl;

    prefix += L"  ";

    // symobls
    {
        os << prefix << "\"" << symbols_ws << L"\" : [" << std::endl;
        prefix += L"  ";
        for (size_t i = 0; i < ctx.symbols.size(); ++i) {
            os << std::pair<std::wstring&, const Context::symbol&>(prefix, ctx.symbols[i]);
            if (i + 1 < ctx.symbols.size()) {
                os << L"," << std::endl;
            }
            else {
                os << std::endl;
            }
        }
        prefix = prefix.substr(0, prefix.length() - 2);
        os << prefix << L"]," << std::endl;
    }

    // paths
    {
        os << prefix << L"\"" << paths_ws << L"\" : [" << std::endl;
        prefix += L"  ";
        size_t ip_size = ctx.paths.size();
        size_t ip_idx = 0;
        for (const auto& ip : ctx.paths) {
            os << prefix << L"\"" << std::regex_replace(ip.second, std::wregex(L"\\\\"), L"\\\\") << L"\"";
            if (ip_idx < ip_size - 1) {
                os << L"," << std::endl;
            }
            else {
                os << std::endl;
            }
            ++ip_idx;
        }
        prefix = prefix.substr(0, prefix.length() - 2);
        os << prefix << L"]," << std::endl;
    }

    // callstacks
    {
        os << prefix << L"\"" << callstacks_ws << L"\" : [" << std::endl;
        prefix += L"  ";
        for (size_t i = 0; i < ctx.callstacks.size(); ++i) {
            const auto& cs(ctx.callstacks[i]);
            os << prefix << L"\"" << cs << L"\"";
            if (i < ctx.callstacks.size() - 1) {
                os << L"," << std::endl;
            }
            else {
                os << std::endl;
            }
        }
        prefix = prefix.substr(0, prefix.length() - 2);
        os << prefix << L"]," << std::endl;
    }

    // resolved_callstacks
    {
        os << prefix << L"\"" << resolved_callstacks_ws << L"\" : [" << std::endl;
        prefix += L"  ";
        for (size_t i = 0; i < ctx.resolved_callstacks.size(); ++i) {
            os << std::pair<std::wstring&, const Context::resolved_callstack&>(prefix, ctx.resolved_callstacks[i]);

            if (i + 1 < ctx.callstacks.size()) {
                os << L"," << std::endl;
            }
            else {
                os << std::endl;
            }
        }
        prefix = prefix.substr(0, prefix.length() - 2);
        os << prefix << L"]" << std::endl;
    }

    os << L"}" << std::endl;

    return os;
}

std::ostream& operator<<(std::ostream& os, Context& ctx)
{
    std::wstringstream ss;
    ss << ctx;
    os << Utf16ToUtf8(ss.str());

    return os;
}

std::wstring Context::ParseCallstackString(const std::wstring& inputStr, std::wstring& imageStr, uint64_t& offsetVal, bool& isPDB)
{
    auto stripDQS = [](const std::wstring& src) -> std::wstring {
        auto b = src.find_first_not_of(L" \"");
        auto e = src.find_last_not_of(L" \"");

        return src.substr(b, e - b + 1);
        };

    // serach the first "+"
    size_t ppos = inputStr.find(L"+");
    if (ppos == std::string::npos) {
        std::wstringstream ss;
        ss << L"Failed to parse a callstack string \"" << inputStr << L"\". (No \"+\" in the string.)";
        return ss.str();
    }

    std::wstring offsetStr;
    if (ppos > 0) {
        imageStr = stripDQS(inputStr.substr(0, ppos));
    }
    else {
        std::wstringstream ss;
        ss << L"Failed to parse a callstack string \"" << inputStr << "\". (Image file name was a empty string.)";
        return ss.str();
    }
    if (ppos + 1 < inputStr.length() - 1) {
        offsetStr = stripDQS(inputStr.substr(ppos + 1));
    }
    else {
        std::wstringstream ss;
        ss << L"Failed to parse a callstack string \"" << inputStr << "\". (Offset number was a empty string.)";
        return ss.str();
    }

    {
        constexpr std::wstring_view allowedExts[] = { L".pdb", L".PDB", L".dll", L".DLL", L".exe", L".EXE" };
        bool validExt = false;
        {
            size_t idx = 0;
            isPDB = false;
            for (auto& ext : allowedExts) {
                if (imageStr.ends_with(ext)) {
                    validExt = true;
                    if (idx < 2)
                        isPDB = true;
                    break;
                }
                ++idx;
            }
        }
        if (!validExt) {
            // it's not a neither exe, dll nor pdb
            std::wstringstream ss;
            ss << L"Failed to parse a callstack string \"" << inputStr << "\". ";
            ss << L"The string \"" << imageStr << L"\" needs to be end with \".dll\", \".exe\" or \".pdb\"";
            return ss.str();
        }
    }

    // resolve offset.
    {
        auto v = ParseNumber(offsetStr);

        if (!v.has_value()) {
            std::wstringstream ss;
            ss << L"Failed to parse the offset value from the call stack string \"" << inputStr << "\". ";
            ss << L"The string \"" << offsetStr << L"\" needs to be a valid form of a number.";
            return ss.str();
        }
        offsetVal = *v;
    }

    return std::wstring();
}

std::wstring Context::ParseCallstacks(bool strictParsing)
{
    for (const auto& csStr : callstacks) {
        std::wstring imageStr;
        uint64_t     imageOffset = 0;
        bool         isComment = false;
        bool         isPDB = false;
        auto errStr = Context::ParseCallstackString(csStr, imageStr, imageOffset, isPDB);
        if (!errStr.empty()) {
            if (strictParsing) {
                return errStr;
            }
            isComment = true;
        }

        // Resolve the image path
        std::filesystem::path imagePath(imageStr);
        if (imagePath.is_relative()) {
            // Image file with a relative path. Search from the path dict in the context.
            auto itr = paths.find(imagePath);
            if (itr != paths.end()) {
                // Found abs path
                imagePath = itr->second;
            }
            else {
                // Check the current path it it doesn't exits...
                imagePath = std::filesystem::current_path() / imagePath;
                if (!std::filesystem::exists(imagePath)) {
                    if (strictParsing) {
                        std::wstringstream ss;
                        ss << L"Failed to find ";
                        if (isPDB) {
                            ss << L"a pdb ";
                        }
                        else {
                            ss << L"an image ";
                        }
                        ss << "file \"" << imageStr << L"\" from the call stack string, \"" << csStr << L"\". ";
                        return ss.str();
                    }
                    isComment = true;
                }
            }
        }

        {
            Context::resolved_callstack rcs;

            if (isComment) {
                // Make it as a comment line.
                rcs.isComment = true;
                rcs.image = csStr;
            }
            else {
                if (isPDB) {
                    rcs.pdb = imagePath;
                }
                else {
                    rcs.image = imagePath;
                }

                rcs.values.image_offset = imageOffset;
            }

            resolved_callstacks.push_back(rcs);
        }
    }

    return std::wstring();
}


std::wstring Context::ParseInputConfig(std::istream& is, const std::filesystem::path& rootPath)
{
    // Parse Json stream.
    picojson::value v;

    is >> v;
    {
        std::string errStr = picojson::get_last_error();
        if (errStr != "") {
            std::wstringstream ss;
            ss << L"Failed to parse input json stream. The last error was.." << std::endl;
            ss << Utf8ToUtf16(errStr) << std::endl;
            return ss.str();
        }
    }

    if (!v.is<picojson::object>()) {
        return L"Failed to parse input config. Root is not a JSON object.\n";
    }

    const picojson::object& rootObj = v.get<picojson::object>();

    for (picojson::object::const_iterator i = rootObj.begin(); i != rootObj.end(); ++i) {
        if (i->first == symbols_s) {
            auto& e(i->second);
            if (!e.is<picojson::array>()) {
                std::wstringstream ss;
                ss << L"\"" << symbols_ws << "\" needs to be an array.";
                return ss.str();
            }
            const picojson::array& arr = e.get<picojson::array>();
            for (picojson::array::const_iterator arrItr = arr.begin(); arrItr != arr.end(); ++arrItr) {
                if (!arrItr->is<picojson::object>()) {
                    std::wstringstream ss;
                    ss << L"\"" << symbols_ws << "\" needs to be an array of ojects.";
                    return ss.str();
                }
                auto [s, errStr] = parseSymbol(arrItr->get<picojson::object>(), rootPath);
                if (!s.has_value())
                    return errStr;

                symbols.push_back(s.value());
            }
        }

        if (i->first == paths_s) {
            auto& e(i->second);
            if (!e.is<picojson::array>()) {
                std::wstringstream ss;
                ss << L"\"" << paths_ws << "\" needs to be an array.";
                return ss.str();
            }
            const picojson::array& arr = e.get<picojson::array>();
            for (picojson::array::const_iterator arrItr = arr.begin(); arrItr != arr.end(); ++arrItr) {
                if (!arrItr->is<std::string>()) {
                    std::wstringstream ss;
                    ss << L"\"" << paths_ws << "\" needs to be an array of strings.";
                    return ss.str();
                }

                std::string pathStr(arrItr->get<std::string>());
                std::filesystem::path p(pathStr);
                std::string filenameStr(p.filename().string());

                if (!pathStr.empty() && !filenameStr.empty()) {
                    paths.insert({ Utf8ToUtf16(filenameStr), Utf8ToUtf16(pathStr) });
                }
                else {
                    std::wstringstream ss;
                    ss << L"Invalid path string detected in \"" << paths_ws << "\". \"" << Utf8ToUtf16(pathStr) << "\". ";
                    return ss.str();
                }
            }
        }

        // Parse callstacks if the command arguments didn't specify a callstack.
        if (callstacks.empty()) {
            if (i->first == callstacks_s) {
                auto& e(i->second);
                if (!e.is<picojson::array>()) {
                    std::wstringstream ss;
                    ss << L"\"" << callstacks_ws << "\" needs to be an array.";
                    return ss.str();
                }
                const picojson::array& arr = e.get<picojson::array>();
                for (picojson::array::const_iterator arrItr = arr.begin(); arrItr != arr.end(); ++arrItr) {
                    if (!arrItr->is<std::string>()) {
                        std::wstringstream ss;
                        ss << L"\"" << callstacks_ws << "\" needs to be an array of strings.";
                        return ss.str();
                    }
                    callstacks.push_back(Utf8ToUtf16(arrItr->get<std::string>()));
                }
            }
        }
    }

    return std::wstring();
}

std::wstring Context::ParseInputConfig(const std::filesystem::path& inputPath)
{
    std::fstream s;
    s.open(inputPath.wstring(), std::ios_base::in);
    if (!s) {
        std::wstringstream ss;
        ss << L"Failed to open file \"" << inputPath.wstring() << "\". ";
        return ss.str();
    }

    {
        auto errStr = ParseInputConfig(s, inputPath.parent_path());
        if (!errStr.empty()) {
            std::wstringstream ss;
            ss << L"An error occured while parsing a config file \"" << inputPath.wstring() << "\". " << errStr;
            return ss.str();
        }
    }
    return std::wstring();
}

std::wstring Context::ParseInputText(std::istream& is, const std::filesystem::path& rootPath)
{
    std::string line;
    constexpr std::wstring_view path_tag = L"--- paths";
    constexpr std::wstring_view callstacks_tag = L"--- callstacks";

    int section = 0;
    while (std::getline(is, line)) {
        if (line.length() < 1)
            continue;

        std::wstring wLine = Utf8ToUtf16(line);
        if (wLine.rfind(path_tag, 0) == 0) {
            section = 1;
            continue;
        }
        if (wLine.rfind(callstacks_tag, 0) == 0) {
            section = 2;
            continue;
        }
        if (section == 1) {
            std::filesystem::path p(wLine);
            std::wstring filenameStr(p.filename().wstring());

            if (!wLine.empty() && !filenameStr.empty()) {
                paths.insert({ filenameStr, wLine });
            }
            else {
                std::wstringstream ss;
                ss << L"Invalid path string detected. \"" << wLine << "\".";
                return ss.str();
            }
        }
        if (section == 2) {
            callstacks.push_back(Utf8ToUtf16(line));
        }
    }

    return std::wstring();
}

std::wstring Context::ParseInputText(const std::filesystem::path& inputPath)
{
    std::fstream s;
    s.open(inputPath.wstring(), std::ios_base::in);
    if (!s) {
        std::wstringstream ss;
        ss << L"Failed to open file \"" << inputPath.wstring() << "\". ";
        return ss.str();
    }

    {
        auto errStr = ParseInputText(s, inputPath.parent_path());
        if (!errStr.empty()) {
            std::wstringstream ss;
            ss << L"An error occured while parsing an input text file \"" << inputPath.wstring() << "\". " << errStr;
            return ss.str();
        }
    }
    return std::wstring();
}

std::wstring Context::DumpResolvedInReadable()
{
    for (auto& c : resolved_callstacks) {
        prepareToWrite(c);
    }

    std::wstringstream ss;

    ss << "--- Resolved Callstacks ---" << std::endl;

    for (const auto& cs : resolved_callstacks) {
        std::wstring moduleName;

        if (cs.isComment) {
            if (cs.image.has_value()) {
                ss << cs.image.value();
            }
        }
        else {
            if (cs.image.has_value()) {
                moduleName = std::filesystem::path(cs.image.value()).filename();
            }

            ss << moduleName << L"!";
            if (cs.function.has_value()) {
                ss << cs.function.value() << L" + " << cs.function_offset.value();
            }
            else {
                ss << cs.image_offset.value();
            }
            if (cs.line.has_value() && cs.function.has_value()) {
                ss << L" [" << cs.line.value() << L" @ " << cs.line_no.value() << L"] + " << cs.line_offset.value();
            }
        }

        ss << std::endl;
    }

    return ss.str();
}
