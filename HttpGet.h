#pragma once
#include <string>
#include <filesystem>
#include <iostream>

class HttpGet
{
public:
	static std::wstring Get(const std::wstring& url, const std::filesystem::path& dest, std::wostream &verboseOut);

};