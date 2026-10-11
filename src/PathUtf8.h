#pragma once

#include <filesystem>
#include <string>

namespace dvb
{
	// JSON expects UTF-8, not Windows path::string()'s ANSI conversion. Preserve
	// the exact UTF-8 bytes, including non-ASCII paths and embedded NULs.
	inline std::string PathUtf8(const std::filesystem::path& a_path)
	{
		const std::u8string bytes = a_path.u8string();
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}
}
