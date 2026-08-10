#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfMetadataStore.h"
#include "PdfObjectParser.h"
#include "PdfTypes.h"
#include "PdfWorkBudget.h"

namespace PdfOutlineLimits {

inline constexpr size_t TitleBytes = 96;
inline constexpr size_t AnchorBytes = 10;
inline constexpr size_t HrefBytes = 64;
inline constexpr size_t PageLabelPrefixBytes = 16;
inline constexpr size_t DestinationNameBytes = 64;
inline constexpr uint16_t MaxEntries = 256;
inline constexpr uint8_t MaxNamedDestinations = 64;
inline constexpr uint8_t MaxDepth = 16;
inline constexpr uint8_t MaxPageLabelRanges = 32;
inline constexpr size_t EncodedRecordBytes = 128;
inline constexpr uint16_t CodecVersion = 1;

}  // namespace PdfOutlineLimits

struct PdfResolvedDestination {
  uint16_t sectionIndex = 0;
  uint32_t anchorOrdinal = 0;
  uint32_t sourcePageIndex = 0;
  bool resolved = false;
};

struct PdfOutlineCandidate {
  PdfObjectReference reference{};
  int16_t parentIndex = -1;
  PdfResolvedDestination destination{};
  const uint8_t* title = nullptr;
  size_t titleLength = 0;
};

struct PdfOutlineEntry {
  char title[PdfOutlineLimits::TitleBytes]{};
  char anchor[PdfOutlineLimits::AnchorBytes]{};
  PdfObjectReference sourceReference{};
  uint32_t anchorOrdinal = 0;
  uint32_t sourcePageIndex = 0;
  int16_t parentIndex = -1;
  uint16_t sectionIndex = 0;
  uint8_t titleLength = 0;
  uint8_t level = 0;
  uint16_t reserved = 0;
};

struct PdfOutlineWorkspace {
  PdfOutlineEntry* entries = nullptr;
  uint16_t capacity = 0;
};

class PdfOutlineBuilder {
 public:
  explicit PdfOutlineBuilder(PdfOutlineWorkspace workspace) : workspace_(workspace) {}

  PdfStatus begin();
  PdfStatus append(const PdfOutlineCandidate& candidate);
  PdfStatus appendHeading(const uint8_t* title, size_t titleLength, uint16_t sectionIndex, uint32_t anchorOrdinal,
                          uint8_t sourceHeadingLevel);
  PdfStatus finish(const uint8_t* fallbackTitle, size_t fallbackTitleLength);

  uint16_t count() const { return count_; }

 private:
  PdfStatus appendEntry(PdfObjectReference reference, int16_t parentIndex, const uint8_t* title, size_t titleLength,
                        const PdfResolvedDestination& destination, uint8_t explicitLevel = 0);
  bool hasReference(PdfObjectReference reference) const;

  PdfOutlineWorkspace workspace_{};
  uint16_t count_ = 0;
  bool initialized_ = false;
  bool finished_ = false;
  bool explicitOutline_ = false;
};

enum class PdfActionKind : uint8_t {
  GoTo,
  Uri,
  Launch,
  JavaScript,
  Attachment,
  RemoteGoTo,
};

PdfStatus pdfResolveInternalAction(PdfActionKind action, const PdfResolvedDestination& destination, char* href,
                                   size_t capacity, size_t* length);

enum class PdfRawDestinationKind : uint8_t {
  None,
  Explicit,
  Named,
};

class PdfPageLabelMap;

struct PdfRawDestination {
  PdfRawDestinationKind kind = PdfRawDestinationKind::None;
  PdfObjectReference pageReference{};
  char name[PdfOutlineLimits::DestinationNameBytes]{};
  uint8_t nameLength = 0;
};

struct PdfKeyTreeNode {
  uint16_t kidsArrayIndex = PDF_INVALID_INDEX;
  uint16_t kidCount = 0;
};

struct PdfNameTreeNode {
  uint16_t kidsArrayIndex = PDF_INVALID_INDEX;
  uint16_t kidCount = 0;
  uint16_t namesArrayIndex = PDF_INVALID_INDEX;
  uint16_t nameCount = 0;
};

enum class PdfNameTreeRelation : int8_t {
  Before = -1,
  Within = 0,
  After = 1,
};

struct PdfKeyTreeSource {
  using InspectFn = PdfStatus (*)(void* context, PdfObjectReference reference, uint16_t* kidCount);
  using ReadKidFn = PdfStatus (*)(void* context, PdfObjectReference parent, uint16_t ordinal,
                                  PdfObjectReference* child);

  void* context = nullptr;
  InspectFn inspect = nullptr;
  ReadKidFn readKid = nullptr;

  constexpr bool valid() const { return inspect != nullptr && readKid != nullptr; }
};

struct PdfKeyTreeFrame {
  PdfObjectReference reference{};
  uint16_t nextKid = 0;
  uint16_t kidCount = 0;
};

static_assert(sizeof(PdfKeyTreeFrame) <= 16, "key-tree traversal frames must remain compact");

enum class PdfKeyTreeWalkStage : uint8_t {
  Idle,
  Root,
  Descend,
  PersistParent,
  CheckCycle,
  InspectChild,
  Complete,
  Failed,
};

struct PdfKeyTreeWalkRuntime {
  PdfStatus failure{};
  PdfObjectReference root{};
  PdfObjectReference pendingChild{};
  PdfKeyTreeFrame activeFrame{};
  uint32_t depth = 0;
  uint32_t ancestorIndex = 0;
  PdfKeyTreeWalkStage stage = PdfKeyTreeWalkStage::Idle;
};

static_assert(sizeof(PdfKeyTreeWalkRuntime) <= 64, "key-tree traversal state must remain compact");

PdfStatus pdfReadKeyTreeNode(const PdfObjectArena& arena, uint16_t rootIndex, PdfKeyTreeNode* node);
PdfStatus pdfReadKeyTreeKid(const PdfObjectArena& arena, const PdfKeyTreeNode& node, uint16_t ordinal,
                            PdfObjectReference* child);
PdfStatus pdfReadNameTreeNode(const PdfObjectArena& arena, uint16_t rootIndex, PdfNameTreeNode* node);
PdfStatus pdfReadNameTreeKid(const PdfObjectArena& arena, const PdfNameTreeNode& node, uint16_t ordinal,
                            PdfObjectReference* child);
PdfStatus pdfReadNameTreeLimits(const PdfObjectArena& arena, uint16_t rootIndex, char* first,
                                size_t firstCapacity, uint8_t* firstLength, char* last,
                                size_t lastCapacity, uint8_t* lastLength);
PdfStatus pdfCompareNameTreeLimits(const PdfObjectArena& arena, uint16_t rootIndex, const uint8_t* name,
                                   size_t nameLength, PdfNameTreeRelation* relation);
PdfStatus pdfResolveNameTreeLeaf(const PdfObjectArena& arena, uint16_t rootIndex, const uint8_t* name,
                                 size_t nameLength, PdfRawDestination* destination,
                                 PdfObjectReference* indirectDestination);
PdfStatus pdfReadRawDestination(const PdfObjectArena& arena, uint16_t rootIndex,
                                PdfRawDestination* destination);
PdfStatus pdfBeginKeyTreeWalk(PdfObjectReference root, const PdfFixedRecordStore& frames,
                              PdfKeyTreeWalkRuntime* runtime);
PdfStepResult pdfStepKeyTreeWalk(const PdfKeyTreeSource& source, const PdfFixedRecordStore& frames,
                                 PdfKeyTreeWalkRuntime* runtime, PdfWorkBudget& budget);

struct PdfCatalogNavigation {
  PdfObjectReference pages{};
  PdfObjectReference outlines{};
  PdfObjectReference namedDestinations{};
  PdfObjectReference pageLabels{};
  PdfObjectReference metadata{};
  char language[PdfMetadataLimits::LanguageBytes]{};
  uint8_t languageLength = 0;
  bool hasPages = false;
  bool hasOutlines = false;
  bool hasNamedDestinations = false;
  bool namedDestinationsContainer = false;
  bool hasPageLabels = false;
  bool hasMetadata = false;
};

struct PdfRawOutlineNode {
  char title[PdfOutlineLimits::TitleBytes]{};
  uint8_t titleLength = 0;
  PdfObjectReference firstChild{};
  PdfObjectReference next{};
  PdfRawDestination destination{};
  bool hasFirstChild = false;
  bool hasNext = false;
};

struct PdfNamedDestinationRecord {
  char name[PdfOutlineLimits::DestinationNameBytes]{};
  PdfRawDestination destination{};
  uint8_t nameLength = 0;
};

struct PdfNamedDestinationWorkspace {
  PdfNamedDestinationRecord* records = nullptr;
  uint16_t capacity = 0;
};

class PdfNamedDestinationMap {
 public:
  explicit PdfNamedDestinationMap(PdfNamedDestinationWorkspace workspace) : workspace_(workspace) {}

  PdfStatus begin();
  PdfStatus add(const uint8_t* name, size_t nameLength, const PdfRawDestination& destination);
  PdfStatus resolve(const uint8_t* name, size_t nameLength, PdfRawDestination* destination) const;
  uint16_t count() const { return count_; }

 private:
  PdfNamedDestinationWorkspace workspace_{};
  uint16_t count_ = 0;
  bool initialized_ = false;
};

struct PdfRawLinkAnnotation {
  PdfRectangle rectangle{};
  PdfActionKind action = PdfActionKind::GoTo;
  PdfRawDestination destination{};
};

PdfStatus pdfReadCatalogNavigation(const PdfObjectArena& arena, uint16_t rootIndex, PdfCatalogNavigation* catalog);
PdfStatus pdfReadNamedDestinationsReference(const PdfObjectArena& arena, uint16_t rootIndex,
                                            PdfObjectReference* destinations);
PdfStatus pdfReadOutlineRoot(const PdfObjectArena& arena, uint16_t rootIndex, PdfObjectReference* first);
PdfStatus pdfReadOutlineNode(const PdfObjectArena& arena, uint16_t rootIndex, PdfRawOutlineNode* node);
PdfStatus pdfReadNamedDestinations(const PdfObjectArena& arena, uint16_t rootIndex,
                                   PdfNamedDestinationMap* destinations);
PdfStatus pdfReadPageLabels(const PdfObjectArena& arena, uint16_t rootIndex, PdfPageLabelMap* labels);
PdfStatus pdfReadLinkAnnotation(const PdfObjectArena& arena, uint16_t rootIndex, PdfRawLinkAnnotation* annotation);
PdfStatus pdfApplyCatalogMetadata(const PdfCatalogNavigation& catalog, PdfMetadataBuilder* metadata);
PdfStatus pdfApplyInfoMetadata(const PdfObjectArena& arena, uint16_t rootIndex, PdfMetadataBuilder* metadata);
PdfStatus pdfApplyXmpMetadata(const uint8_t* source, size_t length, PdfMetadataBuilder* metadata);

enum class PdfPageLabelStyle : uint8_t {
  None,
  Decimal,
  UpperRoman,
  LowerRoman,
  UpperAlpha,
  LowerAlpha,
};

struct PdfPageLabelRange {
  uint32_t firstPageIndex = 0;
  uint32_t startNumber = 1;
  PdfPageLabelStyle style = PdfPageLabelStyle::Decimal;
  char prefix[PdfOutlineLimits::PageLabelPrefixBytes]{};
  uint8_t prefixLength = 0;
};

struct PdfPageLabelWorkspace {
  PdfPageLabelRange* ranges = nullptr;
  uint8_t capacity = 0;
};

class PdfPageLabelMap {
 public:
  explicit PdfPageLabelMap(PdfPageLabelWorkspace workspace) : workspace_(workspace) {}

  PdfStatus begin();
  PdfStatus add(const PdfPageLabelRange& range);
  PdfStatus format(uint32_t pageIndex, char* output, size_t capacity, size_t* length) const;
  uint8_t count() const { return count_; }

 private:
  PdfPageLabelWorkspace workspace_{};
  uint8_t count_ = 0;
  bool initialized_ = false;
};

struct PdfOutlineHeader {
  uint16_t entryCount = 0;
};

struct PdfOutlineEntrySource {
  using ReadFn = PdfStatus (*)(void* context, uint16_t index, PdfOutlineEntry* output);

  void* context = nullptr;
  uint16_t count = 0;
  ReadFn read = nullptr;

  constexpr bool valid() const { return count == 0 || read != nullptr; }
};

struct PdfOutlineEntryVisitor {
  using AcceptFn = PdfStatus (*)(void* context, uint16_t index, const PdfOutlineEntry& record);

  void* context = nullptr;
  AcceptFn accept = nullptr;

  constexpr bool valid() const { return accept != nullptr; }
};

enum class PdfOutlineEncodeStage : uint8_t {
  Idle,
  Header,
  Records,
  Crc,
  Complete,
};

struct PdfOutlineEncodeRuntime {
  uint32_t crc32 = 0;
  uint16_t recordIndex = 0;
  PdfOutlineEncodeStage stage = PdfOutlineEncodeStage::Idle;
};

struct PdfOutlineEncodeWorkspace {
  PdfOutlineEntry entry{};
  PdfOutlineEntry parent{};
  uint8_t encoded[PdfOutlineLimits::EncodedRecordBytes]{};
};

PdfStepResult pdfStepEncodeOutline(
    const PdfOutlineEntrySource& entries,
    const PdfByteSink& destination,
    PdfOutlineEncodeRuntime& runtime,
    PdfOutlineEncodeWorkspace& workspace,
    PdfWorkBudget& budget);
PdfStatus pdfEncodeOutline(const PdfOutlineEntrySource& entries, const PdfByteSink& destination);
PdfStatus pdfDecodeOutline(const PdfByteSource& source, PdfOutlineHeader* header,
                           const PdfOutlineEntryVisitor& entries);
PdfStatus pdfReadOutlineEntry(const PdfByteSource& source, uint16_t index, PdfOutlineEntry* entry);
