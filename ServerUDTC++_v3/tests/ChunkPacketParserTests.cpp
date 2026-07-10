#include "tests/TestSuites.h"

#include "ChunkPacketParser.h"
#include "SecurePacket.h"

#include <winsock2.h>

#include <cstring>
#include <vector>

namespace {

constexpr const char *kTestTransferId =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

void appendU32(std::vector<char> &out, uint32_t value) {
  const uint32_t network = htonl(value);
  const char *ptr = reinterpret_cast<const char *>(&network);
  out.insert(out.end(), ptr, ptr + sizeof(uint32_t));
}

void appendU64(std::vector<char> &out, uint64_t value) {
  appendU32(out, static_cast<uint32_t>(value >> 32));
  appendU32(out, static_cast<uint32_t>(value & 0xFFFFFFFF));
}

void appendString(std::vector<char> &out, const std::string &value) {
  appendU32(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

std::vector<char>
buildPacket(const std::string &filename, const std::string &directory,
            const std::string &serverAddress, uint32_t chunkNumber,
            uint32_t totalChunks, uint64_t chunkOffset, uint64_t totalFileSize,
            const std::string &hash, const std::vector<char> &chunkData,
            const std::string &transferId = kTestTransferId) {
  std::vector<char> payload;
  appendString(payload, filename);
  appendString(payload, directory);
  appendString(payload, serverAddress);
  appendString(payload, transferId);
  appendU32(payload, chunkNumber);
  appendU32(payload, totalChunks);
  appendU64(payload, chunkOffset);
  appendU64(payload, totalFileSize);
  appendString(payload, hash);
  appendU32(payload, static_cast<uint32_t>(chunkData.size()));
  payload.insert(payload.end(), chunkData.begin(), chunkData.end());

  std::vector<char> packet;
  appendU32(packet, static_cast<uint32_t>(payload.size()));
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

void overwritePacketLength(std::vector<char> &packet, uint32_t length) {
  const uint32_t network = htonl(length);
  std::memcpy(packet.data(), &network, sizeof(uint32_t));
}

} // namespace

void runChunkPacketParserTests(TestStats &stats) {
  runTest(
      "ChunkPacketParser parses valid chunk packet",
      [&]() {
        std::vector<char> data = {'a', 'b', 'c', 'd'};
        auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0, 2, 0,
                                  100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", data);

        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);

        require(ok, "Expected valid packet to parse, error: " + error);
        require(chunk.getFilename() == "file.bin", "Filename mismatch");
        require(chunk.getDirectory() == "incoming", "Directory mismatch");
        require(chunk.getServerAddress() == "127.0.0.1",
                "Server address mismatch");
        require(chunk.getTransferId() == kTestTransferId,
                "Transfer id mismatch");
        require(chunk.getChunkNumber() == 0, "Chunk number mismatch");
        require(chunk.getTotalChunk() == 2, "Total chunk mismatch");
        require(chunk.getHash() == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "Hash mismatch");
        require(chunk.getData() == data, "Chunk data mismatch");
      },
      stats);

  runTest(
      "ChunkPacketParser accepts nested relative directory",
      [&]() {
        std::vector<char> data = {'x'};
        auto packet = buildPacket("nomedoarquivo.bin", "XXX/XXXX",
                                  "127.0.0.1", 0, 1, 0, 1, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                  data);

        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);

        require(ok, "Expected nested relative directory to parse: " + error);
        require(chunk.getFilename() == "nomedoarquivo.bin",
                "Filename mismatch");
        require(chunk.getDirectory() == "XXX/XXXX",
                "Nested relative directory must be preserved");
      },
      stats);

  runTest(
      "ChunkPacketParser rejects oversized chunk payload",
      [&]() {
        std::vector<char> payload;
        appendString(payload, "file.bin");
        appendString(payload, "incoming");
        appendString(payload, "127.0.0.1");
        appendString(payload, kTestTransferId);
        appendU32(payload, 0);
        appendU32(payload, 1);
        appendU64(payload, 0);   // chunkOffset
        appendU64(payload, 100); // totalFileSize
        appendString(payload, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        appendU32(payload, ChunkPacketParser::kMaxChunkSizeBytes + 1);
        payload.push_back('x');

        std::vector<char> packet;
        appendU32(packet, static_cast<uint32_t>(payload.size()));
        packet.insert(packet.end(), payload.begin(), payload.end());

        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected oversized chunk payload to fail");
        require(error.find("Invalid chunk data size") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser rejects unsafe filename",
      [&]() {
        const auto packet = buildPacket("..\\evil.bin", "incoming", "127.0.0.1",
                                        0, 1, 0, 100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected unsafe filename to fail");
        require(error.find("Unsafe filename") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser rejects unsafe directory traversal",
      [&]() {
        const auto packet = buildPacket("file.bin", "..\\secret", "127.0.0.1",
                                        0, 1, 0, 100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected unsafe directory to fail");
        require(error.find("Unsafe directory") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser rejects out-of-range chunk number",
      [&]() {
        const auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 3,
                                        3, 0, 100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected out-of-range chunk number to fail");
        require(error.find("out of range") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser accepts unknown total for adaptive non-final chunks",
      [&]() {
        const auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 3,
                                        0, 4096, 10000, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(ok, "Expected unknown adaptive total to parse: " + error);
        require(chunk.getChunkNumber() == 3, "Chunk number mismatch");
        require(chunk.getTotalChunk() == 0, "Unknown total should be preserved");
      },
      stats);

  runTest(
      "ChunkPacketParser rejects chunk byte range past file size",
      [&]() {
        const auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0,
                                        1, 99, 100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x', 'y'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected byte range beyond total file size to fail");
        require(error.find("byte range") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser rejects mismatched packet length",
      [&]() {
        auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0, 1, 0,
                                  100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        overwritePacketLength(packet, 1);
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected mismatched packet length to fail");
        require(error.find("totalLength") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "SecurePacket encrypts and decrypts chunk packet",
      [&]() {
        const std::string key =
            "0123456789abcdef0123456789abcdef-secure-test-key";
        const std::string sessionId = "00112233445566778899aabbccddeeff";
        auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0, 1, 0,
                                  100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x', 'y', 'z'});

        auto encrypted = SecurePacket::EncryptPacket(packet, key, sessionId, 1);
        require(SecurePacket::IsEncryptedPacket(encrypted),
                "Encrypted packet should be marked with secure magic");
        require(encrypted != packet,
                "Encrypted packet should differ from plaintext packet");

        SecurePacket::SessionMetadata metadata;
        require(SecurePacket::TryGetSessionMetadata(encrypted, metadata),
                "Encrypted packet should expose session metadata");
        require(metadata.sessionId == sessionId && metadata.sequenceNumber == 1,
                "Encrypted packet should preserve session metadata");
        auto decrypted =
            SecurePacket::DecryptPacket(encrypted, key, sessionId, &metadata);
        require(decrypted == packet, "Decrypted packet should match plaintext");

        ChunkMetadata chunk;
        std::string error;
        require(ChunkPacketParser::Parse(decrypted, chunk, &error),
                "Decrypted packet should parse: " + error);
      },
      stats);

  runTest(
      "SecurePacket rejects tampered ciphertext",
      [&]() {
        const std::string key =
            "0123456789abcdef0123456789abcdef-secure-test-key";
        const std::string sessionId = "00112233445566778899aabbccddeeff";
        auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0, 1, 0,
                                  100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        auto encrypted = SecurePacket::EncryptPacket(packet, key, sessionId, 1);
        encrypted.back() ^= 0x01;

        bool rejected = false;
        try {
          (void)SecurePacket::DecryptPacket(encrypted, key, sessionId);
        } catch (const std::exception &) {
          rejected = true;
        }
        require(rejected, "Tampered encrypted packet should be rejected");
      },
      stats);

  runTest(
      "ChunkPacketParser rejects invalid transfer identity",
      [&]() {
        const auto packet = buildPacket(
            "file.bin", "incoming", "127.0.0.1", 0, 1, 0, 1,
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            {'x'}, "not-a-valid-transfer-id");

        ChunkMetadata chunk;
        std::string error;
        require(!ChunkPacketParser::Parse(packet, chunk, &error),
                "Invalid transfer identity must be rejected");
        require(error.find("transfer id") != std::string::npos,
                "Parser should identify the invalid transfer id");
      },
      stats);

  runTest(
      "SecurePacket rejects a different session and tampered sequence",
      [&]() {
        const std::string key =
            "0123456789abcdef0123456789abcdef-secure-test-key";
        const std::string sessionId = "00112233445566778899aabbccddeeff";
        auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0, 1, 0,
                                  100, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", {'x'});
        auto encrypted = SecurePacket::EncryptPacket(packet, key, sessionId, 5);

        bool wrongSessionRejected = false;
        try {
          (void)SecurePacket::DecryptPacket(
              encrypted, key, "ffeeddccbbaa99887766554433221100");
        } catch (const std::exception &) {
          wrongSessionRejected = true;
        }
        require(wrongSessionRejected,
                "Packet from another session should be rejected");

        const size_t sequenceOffset = sizeof(uint32_t) + SecurePacket::kMagicSize +
                                      sizeof(uint32_t) * 2U;
        encrypted[sequenceOffset + sizeof(uint64_t) - 1] ^= 0x01;
        bool tamperedSequenceRejected = false;
        try {
          (void)SecurePacket::DecryptPacket(encrypted, key, sessionId);
        } catch (const std::exception &) {
          tamperedSequenceRejected = true;
        }
        require(tamperedSequenceRejected,
                "Authenticated sequence metadata should reject tampering");
      },
      stats);

  runTest(
      "SecurePacket encrypts and decrypts control message",
      [&]() {
        const std::string key =
            "0123456789abcdef0123456789abcdef-secure-test-key";
        const std::string message = "SUCCESS_FILE_COMPLETE";
        const std::string sessionId = "00112233445566778899aabbccddeeff";

        auto encrypted =
            SecurePacket::EncryptControlMessage(message, key, sessionId, 3);
        require(SecurePacket::IsEncryptedPacket(encrypted),
                "Encrypted control message should use secure packet envelope");
        SecurePacket::SessionMetadata metadata;
        require(SecurePacket::DecryptControlMessage(encrypted, key, sessionId,
                                                     &metadata) == message,
                "Decrypted control message should match plaintext");
        require(metadata.sequenceNumber == 3,
                "Control message should preserve its sequence number");
      },
      stats);

  runTest(
      "ChunkPacketParser rejects hash with invalid hex characters",
      [&]() {
        // 64 chars but 'G' at position 0 is not lowercase hex.
        const std::string badHash =
            "G123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0,
                                        1, 0, 100, badHash, {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected non-hex hash to fail");
        require(error.find("Invalid hash format") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkPacketParser rejects hash with wrong length",
      [&]() {
        // 32 hex chars (SHA-1 length), not 64 (SHA-256).
        const std::string shortHash = "0123456789abcdef0123456789abcdef";
        const auto packet = buildPacket("file.bin", "incoming", "127.0.0.1", 0,
                                        1, 0, 100, shortHash, {'x'});
        ChunkMetadata chunk;
        std::string error;
        const bool ok = ChunkPacketParser::Parse(packet, chunk, &error);
        require(!ok, "Expected wrong-length hash to fail");
        require(error.find("Invalid hash length") != std::string::npos,
                "Unexpected error: " + error);
      },
      stats);

  runTest(
      "ChunkMetadata::getFilenameW round-trips UTF-8 multi-byte filename",
      [&]() {
        // "café.bin" in UTF-8: c a f 0xC3 0xA9 . b i n
        const std::string utf8Filename = std::string("caf\xC3\xA9.bin");
        ChunkMetadata chunk;
        chunk.setFilename(utf8Filename);
        const std::wstring wide = chunk.getFilenameW();
#ifdef _WIN32
        const std::wstring expected = L"café.bin";
        require(wide == expected,
                "UTF-8 filename should decode to wide-char U+00E9 (got length " +
                    std::to_string(wide.size()) + ")");
#else
        require(wide.size() == utf8Filename.size(),
                "Non-Windows wide conversion preserves byte count");
#endif
      },
      stats);
}
