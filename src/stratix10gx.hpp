// SPDX-License-Identifier: Apache-2.0
#ifndef SRC_STRATIX10GX_HPP_
#define SRC_STRATIX10GX_HPP_

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "part.hpp"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// Defaults apply only when the user selects the stratix10gx board.
namespace Stratix10GX {
inline std::string configurationDirectory(const char *config, const char *home)
{
	std::filesystem::path root;
	if (config && *config) root = config;
	else if (home && *home) root = std::filesystem::path(home)/".config";
	else return {};
	return (root/"openFPGALoader"/"stratix10gx").string();
}

inline std::string userDirectory()
{
	const char *config = std::getenv("XDG_CONFIG_HOME");
#ifdef _WIN32
	if (!config || !*config) config = std::getenv("APPDATA");
#endif
	return configurationDirectory(config,std::getenv("HOME"));
}

inline std::string installedDirectory()
{
#ifdef DATA_DIR
	return (std::filesystem::path(DATA_DIR)/"openFPGALoader"/"stratix10gx").string();
#else
	return {};
#endif
}

inline std::string relativeDirectory(const std::filesystem::path &executable)
{
	std::error_code error;
	const auto real = std::filesystem::canonical(executable,error);
	if (error) return {};
	const auto local = real.parent_path()/"share"/"openFPGALoader"/"stratix10gx";
	if (std::filesystem::is_directory(local,error)) return local.string();
	const auto installed = real.parent_path().parent_path()/"share"/"openFPGALoader"/"stratix10gx";
	return std::filesystem::is_directory(installed,error) ? installed.string() : local.string();
}

inline std::string portableDirectory()
{
#ifdef __APPLE__
	std::vector<char> path(1024);
	uint32_t size = uint32_t(path.size());
	if (_NSGetExecutablePath(path.data(),&size) != 0) {
		if (!size || size > 1024*1024) return {};
		path.resize(size);
		if (_NSGetExecutablePath(path.data(),&size) != 0) return {};
	}
	return relativeDirectory(path.data());
#else
	return {};
#endif
}

inline std::string existingFile(const std::string &directory, const char *name)
{
	if (directory.empty()) return {};
	const auto file = std::filesystem::path(directory)/name;
	std::error_code error;
	return std::filesystem::is_regular_file(file,error) ? file.string() : std::string();
}

// Never assume a fixed chain index: MAX V and FPGA ordering can differ.
// Explicit --index-chain selection bypasses this board default.
inline int targetIndex(const std::vector<uint32_t> &devices)
{
	int target = -1;
	for (size_t i = 0; i < devices.size(); ++i) {
		auto device = fpga_list.find(devices[i]);
		if (device == fpga_list.end()) device = fpga_list.find(devices[i] & 0x0fffffff);
		if (device == fpga_list.end() || device->second.family != "stratix 10") continue;
		if (target >= 0) return -2;
		target = int(i);
	}
	return target;
}
}  // namespace Stratix10GX
#endif  // SRC_STRATIX10GX_HPP_
