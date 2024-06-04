#pragma once
#include <string>
#include <optional>
#include <vector>
#include <map>
#include <filesystem>
#include <iostream>

struct Context {
    struct symbol {
        std::optional<std::wstring> server;
        std::optional<std::wstring> cache;
        std::optional<std::wstring> direct;
        std::optional<bool> force_create_cache_dir;
    };

    struct resolved_callstack {
        bool isComment = false;

        std::optional<std::wstring> image;
        std::optional<std::wstring> image_offset;
        std::optional<std::wstring> pdb;
        std::optional<std::wstring> pdb_signature;
        std::optional<std::wstring> function;
        std::optional<std::wstring> function_offset;
        std::optional<std::wstring> line;
        std::optional<std::wstring> line_no;
        std::optional<std::wstring> line_offset;

        struct {
            std::optional<uint64_t> image_offset;
            std::optional<uint64_t> function_offset;
            std::optional<uint64_t> line_no;
            std::optional<uint64_t> line_offset;
        } values;
    };

    std::vector<symbol>                     symbols;
    std::map<std::wstring, std::wstring>    paths;
    std::vector<std::wstring>               callstacks;

    std::vector<resolved_callstack>         resolved_callstacks;

public:
    static std::wstring ParseCallstackString(const std::wstring& inputStr, std::wstring& imageStr, uint64_t& offsetVal, bool& isPDB);
    std::wstring ParseCallstacks(bool strictParsing);
    std::wstring ParseInputConfig(std::istream& is, const std::filesystem::path& rootPath);
    std::wstring ParseInputConfig(const std::filesystem::path& inputPath);
    std::wstring ParseInputText(std::istream& is, const std::filesystem::path& rootPath);
    std::wstring ParseInputText(const std::filesystem::path& inputPath);
    std::wstring DumpResolvedInReadable();
};

std::wostream& operator<<(std::wostream& os, Context& is);
std::ostream& operator<<(std::ostream& os, Context& is);

