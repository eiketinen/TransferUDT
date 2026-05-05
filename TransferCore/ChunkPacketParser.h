#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ChunkMetadata.h"

namespace ChunkPacketParser {

inline constexpr uint32_t kMaxChunkSizeBytes = 4U * 1024U * 1024U;
inline constexpr uint32_t kMaxMetadataSizeBytes = 256U * 1024U;
inline constexpr uint32_t kMaxPacketSizeBytes = kMaxChunkSizeBytes + kMaxMetadataSizeBytes;
inline constexpr uint64_t kMaxFileSizeBytes = 100ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr uint32_t kMaxTotalChunks = 1000000U;

bool IsSafeFilename(const std::string& filename);
bool IsSafeRelativeDirectory(const std::string& directory);

bool Parse(const std::vector<char>& buffer, ChunkMetadata& chunk, std::string* errorMessage = nullptr);

}
