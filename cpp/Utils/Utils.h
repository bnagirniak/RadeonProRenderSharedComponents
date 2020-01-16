#pragma once
#include <string>

namespace SharedComponentsUtils
{
	std::wstring s2ws(const std::string& str);
	std::string ws2s(const std::wstring& wstr);
}