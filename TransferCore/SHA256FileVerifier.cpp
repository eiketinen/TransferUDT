#include "Interfaces.h"
#include <openssl/evp.h>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>
#include "Logger.h"

std::string SHA256FileVerifier::calculateChunkHash(const std::vector<char>& data)
{
    // Usar a API EVP mais moderna do OpenSSL
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    // Criar e inicializar o contexto EVP
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to create EVP context for hash");
        return "";
    }

    // Inicializar o digest com SHA256
    if (!EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr))
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to initialize context for SHA256");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Atualizar o contexto com os dados
    if (!EVP_DigestUpdate(ctx, data.data(), data.size()))
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to process data for hash");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Finalizar e obter o resultado do hash
    if (!EVP_DigestFinal_ex(ctx, hash, &hashLen))
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to finalize hash calculation");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Liberar recursos
    EVP_MD_CTX_free(ctx);

    // Converter o hash para string hexadecimal
    std::ostringstream oss;
    for (unsigned int i = 0; i < hashLen; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return oss.str();
}

std::string SHA256FileVerifier::calculateFileHash(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        Logger::getInstance().error("SHA256FileVerifier", "Error opening file for hash calculation: " + filePath);
        return "";
    }

    // Usar a API EVP mais moderna do OpenSSL
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    // Criar e inicializar o contexto EVP
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to create EVP context for combined hash");
        return "";
    }

    // Inicializar o digest com SHA256
    if (!EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr))
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to initialize context for SHA256");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Ler o arquivo em blocos e atualizar o hash
    const size_t bufferSize = 8192;
    std::vector<char> buffer(bufferSize);

    while (file)
    {
        file.read(buffer.data(), bufferSize);
        size_t bytesRead = file.gcount();

        if (bytesRead > 0)
        {
            if (!EVP_DigestUpdate(ctx, buffer.data(), bytesRead))
            {
                Logger::getInstance().error("SHA256FileVerifier", "Failed to process file chunk for hash");
                EVP_MD_CTX_free(ctx);
                return "";
            }
        }
    }

    // Finalizar e obter o resultado do hash
    if (!EVP_DigestFinal_ex(ctx, hash, &hashLen))
    {
        Logger::getInstance().error("SHA256FileVerifier", "Failed to finalize hash calculation");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Liberar recursos
    EVP_MD_CTX_free(ctx);

    // Converter o hash para string hexadecimal
    std::ostringstream oss;
    for (unsigned int i = 0; i < hashLen; ++i)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    return oss.str();
}