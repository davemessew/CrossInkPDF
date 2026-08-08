#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "PdfLexer.h"
#include "PdfObjectParser.h"
#include "PdfSecurity.h"
#include "PdfTestIo.h"

namespace {

constexpr char kOwner[] = "67e3f3f26d94dab47abc0c4bb86b6f667e413e7fbe71eae213f79db989b513d4";
constexpr char kUser[] = "0ce72e1e8cca5dedd57f947587f1f7f900000000000000000000000000000000";
constexpr char kVariableIdentifierUser[] =
    "083fdd19ecdbe0b4da678aed9c6eacaa00000000000000000000000000000000";

struct ArenaStorage {
  std::array<PdfValue, 96> values{};
  std::array<PdfDictionaryEntry, 96> dictionaries{};
  std::array<PdfArrayItem, 16> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
};

PdfStepResult parseObject(const std::string& input, ArenaStorage* const storage, uint16_t* const rootIndex) {
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 256> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  PdfObjectParser parser(lexer, storage->arena);
  parser.begin();
  for (uint16_t step = 0; step < 256; ++step) {
    PdfWorkBudget budget{4, 256};
    const PdfStepResult result = parser.step(budget);
    if (!result.yielded()) {
      if (result.complete()) {
        *rootIndex = parser.rootIndex();
      }
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

std::string securityDictionary(const char* const method = "V2", const char* const user = kUser,
                               const char* const metadata = "") {
  return std::string("<< /CF << /StdCF << /AuthEvent /DocOpen /CFM /") + method +
         " /Length 16 >> >> /Filter /Standard /Length 128 /O <" + kOwner + "> /P -1324 /R 4 " +
         "/StmF /StdCF /StrF /StdCF /U <" + user + "> /V 4 " + metadata + ">>";
}

PdfSecurityTrailer sonyTrailer() {
  PdfSecurityTrailer trailer{};
  trailer.encrypted = true;
  trailer.encryptionReference = {126423, 0};
  constexpr uint8_t identifier[] = {0xf7, 0x1f, 0x97, 0x2e, 0x16, 0xcf, 0xb5, 0xd7,
                                    0x50, 0x10, 0x2a, 0x31, 0x38, 0xac, 0x17, 0x0f};
  std::memcpy(trailer.fileIdentifier, identifier, sizeof(identifier));
  trailer.fileIdentifierLength = sizeof(identifier);
  return trailer;
}

}  // namespace

TEST(PdfSecurityTest, OpensSonyEmptyPasswordAndDecryptsStringsAndStreams) {
  ArenaStorage encryptionStorage;
  uint16_t encryptionRoot = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(securityDictionary(), &encryptionStorage, &encryptionRoot).complete());

  PdfSecurity security;
  ASSERT_TRUE(security.initializeEmptyPassword(encryptionStorage.arena, encryptionRoot, sonyTrailer()).ok());

  ArenaStorage catalogStorage;
  uint16_t catalogRoot = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject("<6481>", &catalogStorage, &catalogRoot).complete());
  ASSERT_TRUE(security.decryptStrings(catalogStorage.arena, {126424, 0}).ok());
  EXPECT_TRUE(pdfTextEquals(catalogStorage.arena, catalogStorage.arena.values[catalogRoot], "en"));

  PdfTestByteSource encryptedStream({0x64, 0x81});
  PdfByteSource decrypted{};
  ASSERT_TRUE(security.openStream(encryptedStream.source(), {126424, 0}, &decrypted).ok());
  uint8_t output[2]{};
  size_t bytesRead = 0;
  ASSERT_TRUE(decrypted.readAt(decrypted.context, 1, output + 1, 1, &bytesRead).ok());
  ASSERT_EQ(bytesRead, 1U);
  ASSERT_TRUE(decrypted.readAt(decrypted.context, 0, output, sizeof(output), &bytesRead).ok());
  ASSERT_EQ(bytesRead, sizeof(output));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(output), sizeof(output)), "en");
}

TEST(PdfSecurityTest, OpensEmptyPasswordWithVariableLengthFileIdentifier) {
  ArenaStorage encryptionStorage;
  uint16_t encryptionRoot = PDF_INVALID_INDEX;
  ASSERT_TRUE(
      parseObject(securityDictionary("V2", kVariableIdentifierUser), &encryptionStorage, &encryptionRoot).complete());

  PdfSecurityTrailer trailer = sonyTrailer();
  constexpr uint8_t suffix[] = {1, 2, 3, 4};
  std::memcpy(trailer.fileIdentifier + trailer.fileIdentifierLength, suffix, sizeof(suffix));
  trailer.fileIdentifierLength += sizeof(suffix);

  PdfSecurity security;
  EXPECT_TRUE(security.initializeEmptyPassword(encryptionStorage.arena, encryptionRoot, trailer).ok());
}

TEST(PdfSecurityTest, RejectsUnsupportedOrPasswordProtectedVariants) {
  const std::array<std::string, 4> dictionaries = {
      securityDictionary("AESV2"),
      securityDictionary("V2", "1ce72e1e8cca5dedd57f947587f1f7f900000000000000000000000000000000"),
      securityDictionary("V2", kUser, "/EncryptMetadata false "),
      securityDictionary("V2", kUser, "/EFF /StdCF "),
  };
  for (const std::string& dictionary : dictionaries) {
    ArenaStorage storage;
    uint16_t root = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseObject(dictionary, &storage, &root).complete());
    PdfSecurity security;
    const PdfStatus status = security.initializeEmptyPassword(storage.arena, root, sonyTrailer());
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error, PdfError::Encrypted);
  }
}
