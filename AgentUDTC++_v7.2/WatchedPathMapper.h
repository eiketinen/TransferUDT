#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace WatchedPathMapper {

bool TryBuildRelativeDirectory(
    const std::filesystem::path &filePath,
    const std::vector<std::string> &watchedRoots,
    std::string &relativeDirectory);

} // namespace WatchedPathMapper
