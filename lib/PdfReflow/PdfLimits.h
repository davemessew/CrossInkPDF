#pragma once

#include <cstddef>
#include <cstdint>

namespace PdfLimits {

inline constexpr uint32_t MaxIndirectObjects = 100000;
inline constexpr uint32_t MaxPages = 5000;
inline constexpr uint32_t MaxOperatorsPerPage = 250000;
inline constexpr uint32_t MaxOperatorsPerDocument = 10000000;
inline constexpr uint8_t MaxFormDepth = 16;
inline constexpr uint64_t MaxExpandedRequiredStreamBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr uint16_t MaxExpansionRatio = 200;
inline constexpr uint8_t MaxFiltersPerStream = 4;

inline constexpr uint8_t MaxContainerNesting = 32;
inline constexpr uint8_t MaxObjectRecursion = 32;
inline constexpr uint8_t MaxTrailerDepth = 32;
inline constexpr uint8_t MaxPageTreeDepth = 64;
inline constexpr uint8_t MaxContentStreamsPerPage = 16;
inline constexpr uint8_t MaxLinkAnnotationsPerPage = 16;
inline constexpr uint8_t MaxCoverCandidateSources = 8;
inline constexpr uint8_t MaxCoverScanPages = 8;
inline constexpr uint8_t MaxXrefSections = 64;
inline constexpr uint8_t MaxXrefFieldBytes = 8;
inline constexpr uint8_t MaxXrefEntryBytes = 24;
inline constexpr uint8_t MaxXrefIndexPairs = 64;
inline constexpr uint8_t XrefMergeEntries = 64;
inline constexpr uint32_t MaxCMapRanges = 8192;
inline constexpr uint16_t MaxPageUniqueGlyphs = 256;
inline constexpr uint16_t MaxPaletteEntries = 256;
inline constexpr uint32_t MaxImagePixels = 16000000;
inline constexpr uint32_t MaxImageDimension = 65535;
inline constexpr size_t MaxDecodedImageRowBytes = 8 * 1024;

inline constexpr size_t UzlibDictionaryBytes = 32768;
inline constexpr size_t SourceBufferBytes = 4096;
inline constexpr size_t DecoderOutputBytes = 4096;
inline constexpr size_t PageTextBytes = 8192;
inline constexpr size_t PageRunCount = 256;
inline constexpr size_t PageRunBytes = PageRunCount * 48;
inline constexpr size_t OperandOrderHistogramBytes = 2048;
inline constexpr size_t TotalWorkspaceBytes = UzlibDictionaryBytes + SourceBufferBytes + DecoderOutputBytes +
                                              PageTextBytes + PageRunBytes + OperandOrderHistogramBytes;
inline constexpr size_t MaxIndividualWorkspaceBytes = UzlibDictionaryBytes;

static_assert(PageRunBytes <= 12288);
static_assert(TotalWorkspaceBytes <= 63488);
static_assert(MaxIndividualWorkspaceBytes <= 32768);

}  // namespace PdfLimits
