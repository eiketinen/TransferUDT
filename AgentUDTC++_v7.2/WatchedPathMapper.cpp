#include "WatchedPathMapper.h"

#include <algorithm>
#include <cwctype>
#include <iterator>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
std::wstring normalizePathElement(const fs::path &path) {
  std::wstring value = path.wstring();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) {
                   return static_cast<wchar_t>(std::towlower(ch));
                 });
  return value;
}
#else
std::string normalizePathElement(const fs::path &path) {
  return path.string();
}
#endif

bool samePathElement(const fs::path &left, const fs::path &right) {
  return normalizePathElement(left) == normalizePathElement(right);
}

bool pathStartsWith(const fs::path &root, const fs::path &candidate) {
  auto rootIt = root.begin();
  auto candidateIt = candidate.begin();
  for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
    if (candidateIt == candidate.end() ||
        !samePathElement(*rootIt, *candidateIt)) {
      return false;
    }
  }
  return true;
}

fs::path absoluteNormalPath(const fs::path &path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) {
    absolute = path;
  }
  return absolute.lexically_normal();
}

fs::path weaklyCanonicalPath(const fs::path &path, std::error_code &ec) {
  fs::path canonical = fs::weakly_canonical(path, ec);
  if (ec) {
    return {};
  }
  return canonical.lexically_normal();
}

bool isReparsePointOrUnknown(const fs::path &path) {
#ifdef _WIN32
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  return ec || fs::is_symlink(status);
#endif
}

bool hasReparsePointInPath(const fs::path &root, const fs::path &candidate) {
  const fs::path rootAbs = absoluteNormalPath(root);
  const fs::path candidateAbs = absoluteNormalPath(candidate);
  if (!pathStartsWith(rootAbs, candidateAbs)) {
    return true;
  }

  fs::path current = rootAbs;
  if (isReparsePointOrUnknown(current)) {
    return true;
  }

  auto candidateIt = candidateAbs.begin();
  for (auto rootIt = rootAbs.begin(); rootIt != rootAbs.end(); ++rootIt) {
    if (candidateIt != candidateAbs.end()) {
      ++candidateIt;
    }
  }

  for (; candidateIt != candidateAbs.end(); ++candidateIt) {
    current /= *candidateIt;
    if (isReparsePointOrUnknown(current)) {
      return true;
    }
  }
  return false;
}

size_t elementCount(const fs::path &path) {
  return static_cast<size_t>(std::distance(path.begin(), path.end()));
}

} // namespace

namespace WatchedPathMapper {

bool TryBuildRelativeDirectory(const fs::path &filePath,
                               const std::vector<std::string> &watchedRoots,
                               std::string &relativeDirectory) {
  relativeDirectory.clear();

  std::error_code ec;
  const fs::path canonicalFile = weaklyCanonicalPath(filePath, ec);
  if (ec) {
    return false;
  }

  const auto status = fs::symlink_status(filePath, ec);
  if (ec || !fs::is_regular_file(status)) {
    return false;
  }

  bool foundRoot = false;
  size_t bestRootDepth = 0;
  fs::path bestRoot;

  for (const auto &rootString : watchedRoots) {
    const fs::path root(rootString);
    const fs::path canonicalRoot = weaklyCanonicalPath(root, ec);
    if (ec) {
      continue;
    }

    if (!pathStartsWith(canonicalRoot, canonicalFile) ||
        samePathElement(canonicalRoot, canonicalFile) ||
        hasReparsePointInPath(root, filePath)) {
      continue;
    }

    const size_t depth = elementCount(canonicalRoot);
    if (!foundRoot || depth > bestRootDepth) {
      foundRoot = true;
      bestRootDepth = depth;
      bestRoot = canonicalRoot;
    }
  }

  if (!foundRoot) {
    return false;
  }

  const fs::path parent = canonicalFile.parent_path().lexically_normal();
  if (samePathElement(bestRoot, parent)) {
    return true;
  }

  const fs::path relative = parent.lexically_relative(bestRoot);
  if (relative.empty()) {
    return false;
  }

  relativeDirectory = relative.generic_u8string();
  return true;
}

} // namespace WatchedPathMapper
