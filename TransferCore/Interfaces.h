#pragma once
#include <string>
#include <vector>
#include "ChunkMetadata.h"

// Interface para FileIntegrityVerifier
class IFileIntegrityVerifier
{
public:
    virtual ~IFileIntegrityVerifier() = default;

    virtual std::string calculateFileHash(const std::string& filePath) = 0;
    virtual std::string calculateChunkHash(const std::vector<char>& data) = 0;
};

// Implementação concreta para verificação de integridade
class SHA256FileVerifier : public IFileIntegrityVerifier
{
public:
    std::string calculateFileHash(const std::string& filePath) override;
    std::string calculateChunkHash(const std::vector<char>& data) override;
};