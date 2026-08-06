#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"
#include "PdfWorkBudget.h"

namespace PdfMetadataLimits {

inline constexpr size_t TitleBytes = 192;
inline constexpr size_t AuthorBytes = 128;
inline constexpr size_t LanguageBytes = 24;
inline constexpr uint16_t MaxSections = 256;
inline constexpr uint16_t MaxOutlineEntries = 256;
inline constexpr size_t IoChunkBytes = 64;
inline constexpr uint16_t CodecVersion = 1;

}  // namespace PdfMetadataLimits

enum class PdfMetadataOrigin : uint8_t {
  Filename,
  Info,
  Xmp,
  Catalog,
};

struct PdfMetadata {
  char title[PdfMetadataLimits::TitleBytes]{};
  char author[PdfMetadataLimits::AuthorBytes]{};
  char language[PdfMetadataLimits::LanguageBytes]{};
  uint16_t titleLength = 0;
  uint16_t authorLength = 0;
  uint8_t languageLength = 0;
  uint8_t reserved = 0;
  uint16_t sectionCount = 0;
  uint16_t outlineCount = 0;
  uint32_t totalWords = 0;
};

struct PdfMetadataSection {
  uint32_t byteSize = 0;
  uint32_t cumulativeSize = 0;
  uint32_t firstWordOrdinal = 0;
  uint32_t wordCount = 0;
  uint32_t firstAnchorOrdinal = 0;
  int16_t tocIndex = -1;
  uint16_t reserved = 0;
};

static_assert(sizeof(PdfMetadataSection) == 24);

struct PdfMetadataSectionSource {
  using ReadFn = PdfStatus (*)(void* context, uint16_t index, PdfMetadataSection* output);

  void* context = nullptr;
  uint16_t count = 0;
  ReadFn read = nullptr;

  constexpr bool valid() const { return count == 0 || read != nullptr; }
};

enum class PdfMetadataEncodeStage : uint8_t {
  Header,
  Title,
  Author,
  Language,
  Sections,
  Crc,
  Complete,
};

struct PdfMetadataEncodeRuntime {
  uint32_t crc32 = 0;
  uint32_t cumulativeBytes = 0;
  uint32_t cumulativeWords = 0;
  uint16_t sectionIndex = 0;
  uint16_t fieldOffset = 0;
  PdfMetadataEncodeStage stage = PdfMetadataEncodeStage::Header;
};

struct PdfMetadataSectionVisitor {
  using AcceptFn = PdfStatus (*)(void* context, uint16_t index, const PdfMetadataSection& record);

  void* context = nullptr;
  AcceptFn accept = nullptr;

  constexpr bool valid() const { return accept != nullptr; }
};

class PdfMetadataBuilder {
 public:
  PdfStatus begin(const uint8_t* filename, size_t length);
  PdfStatus setTitle(PdfMetadataOrigin origin, const uint8_t* value, size_t length);
  PdfStatus setAuthor(PdfMetadataOrigin origin, const uint8_t* value, size_t length);
  PdfStatus setLanguage(PdfMetadataOrigin origin, const uint8_t* value, size_t length);

  const PdfMetadata& metadata() const { return metadata_; }

 private:
  PdfStatus setValue(PdfMetadataOrigin origin, const uint8_t* value, size_t length, char* destination, size_t capacity,
                     uint16_t* storedLength, uint8_t* priority, bool language);
  static uint8_t priorityFor(PdfMetadataOrigin origin, bool language);

  PdfMetadata metadata_{};
  uint8_t titlePriority_ = 0;
  uint8_t authorPriority_ = 0;
  uint8_t languagePriority_ = 0;
  bool initialized_ = false;
};

PdfStatus pdfEncodeMetadata(const PdfMetadata& metadata, const PdfMetadataSectionSource& sections,
                            const PdfByteSink& destination);
PdfStepResult pdfStepEncodeMetadata(const PdfMetadata& metadata, const PdfMetadataSectionSource& sections,
                                    const PdfByteSink& destination, PdfMetadataEncodeRuntime& runtime,
                                    PdfWorkBudget& budget);
PdfStatus pdfInspectMetadata(const PdfByteSource& source, PdfMetadata* metadata);
PdfStatus pdfDecodeMetadata(const PdfByteSource& source, PdfMetadata* metadata,
                            const PdfMetadataSectionVisitor& sections);
