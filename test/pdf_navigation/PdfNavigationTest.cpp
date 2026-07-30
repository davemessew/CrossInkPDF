#include <Print.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfMetadataStore.h"
#include "PdfObjectResolver.h"
#include "PdfOutline.h"
#include "PdfReflowDocument.h"
#include "PdfSemanticWriter.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

namespace {

PdfStatus setTitle(PdfMetadataBuilder& builder, const PdfMetadataOrigin origin, const char* value) {
  return builder.setTitle(origin, reinterpret_cast<const uint8_t*>(value), std::strlen(value));
}

PdfStatus setAuthor(PdfMetadataBuilder& builder, const PdfMetadataOrigin origin, const char* value) {
  return builder.setAuthor(origin, reinterpret_cast<const uint8_t*>(value), std::strlen(value));
}

PdfStatus setLanguage(PdfMetadataBuilder& builder, const PdfMetadataOrigin origin, const char* value) {
  return builder.setLanguage(origin, reinterpret_cast<const uint8_t*>(value), std::strlen(value));
}

struct SectionTable {
  std::vector<PdfMetadataSection> records;

  static PdfStatus read(void* context, const uint16_t index, PdfMetadataSection* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<SectionTable*>(context);
    if (index >= self.records.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.records[index];
    return PdfStatus::success();
  }

  PdfMetadataSectionSource source() { return {this, static_cast<uint16_t>(records.size()), read}; }
};

struct SectionCollector {
  std::vector<PdfMetadataSection> records;

  static PdfStatus accept(void* context, const uint16_t index, const PdfMetadataSection& record) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<SectionCollector*>(context);
    if (index != self.records.size()) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    self.records.push_back(record);
    return PdfStatus::success();
  }

  PdfMetadataSectionVisitor visitor() { return {this, accept}; }
};

struct OutlineTable {
  std::vector<PdfOutlineEntry> records;

  static PdfStatus read(void* context, const uint16_t index, PdfOutlineEntry* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<OutlineTable*>(context);
    if (index >= self.records.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.records[index];
    return PdfStatus::success();
  }

  PdfOutlineEntrySource source() { return {this, static_cast<uint16_t>(records.size()), read}; }
};

struct OutlineCollector {
  std::vector<PdfOutlineEntry> records;

  static PdfStatus accept(void* context, const uint16_t index, const PdfOutlineEntry& record) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<OutlineCollector*>(context);
    if (index != self.records.size()) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    self.records.push_back(record);
    return PdfStatus::success();
  }

  PdfOutlineEntryVisitor visitor() { return {this, accept}; }
};

PdfOutlineCandidate candidate(uint32_t objectNumber, int16_t parentIndex, const char* title, uint16_t sectionIndex,
                              uint32_t anchorOrdinal);

struct RequiredFileTable {
  std::vector<PdfRequiredFileRecord> records;

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<RequiredFileTable*>(context);
    if (index >= self.records.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.records[index];
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, static_cast<uint32_t>(records.size()), read}; }
};

class BufferPrint final : public Print {
 public:
  size_t write(const uint8_t* bytes, const size_t length) override {
    output.insert(output.end(), bytes, bytes + length);
    return length;
  }

  std::vector<uint8_t> output;
};

struct NavigableCacheFixture {
  static constexpr const char* SourcePath = "/books/navigation.pdf";
  static constexpr const char* CacheDirectory = "/.crosspoint";

  PdfTestCacheIo storage;
  PdfSourceIdentity identity{};
  std::string cacheRoot;
  RequiredFileTable required;

  PdfRequiredFileRecord addRequired(const std::string& relative, const std::string& bytes) {
    const std::string path = cacheRoot + "/" + relative;
    PdfCacheTrackedWriter writer;
    EXPECT_TRUE(pdfOpenTrackedCacheWriter(storage.io(), path.c_str(), relative.c_str(), PdfCacheFileKind::Required,
                                          64 * 1024, &writer)
                    .ok());
    EXPECT_TRUE(pdfWriteTrackedCacheFile(&writer, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()).ok());
    PdfRequiredFileRecord record{};
    EXPECT_TRUE(pdfCloseTrackedCacheFile(&writer, &record).ok());
    required.records.push_back(record);
    return record;
  }

  void build() {
    const std::string source = "%PDF-navigation-source-for-bounded-identity";
    storage.addFile(SourcePath, std::vector<uint8_t>(source.begin(), source.end()), 77, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> identityWorkspace{};
    ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), SourcePath, identityWorkspace.data(), identityWorkspace.size(),
                                         &identity)
                    .ok());

    std::array<char, PDF_CACHE_PATH_CAPACITY> root{};
    ASSERT_TRUE(pdfFormatCacheRoot(CacheDirectory, SourcePath, root.data(), root.size()).ok());
    cacheRoot = root.data();
    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(storage.io(), cacheRoot.c_str()).ok());
    ASSERT_TRUE(cache.ensureGeneration(7).ok());

    const std::string section0 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head><body>"
        "<span id=\"p00000000\" role=\"doc-pagebreak\" aria-label=\"i\"></span>"
        "<h1 id=\"b00000000\">Contents</h1><p id=\"b00000001\">"
        "<a href=\"sections/000001.xhtml#b00000003\">Chapter Two</a></p></body></html>";
    const std::string section1 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head><body>"
        "<span id=\"p00000001\" role=\"doc-pagebreak\" aria-label=\"A-1\"></span>"
        "<h1 id=\"b00000003\">Chapter Two</h1><p id=\"b00000004\">Index "
        "<a href=\"sections/000000.xhtml#b00000000\">Contents</a></p></body></html>";
    const PdfRequiredFileRecord section0Record = addRequired("gen_7/sections/000000.xhtml", section0);
    const PdfRequiredFileRecord section1Record = addRequired("gen_7/sections/000001.xhtml", section1);

    PdfMetadataBuilder metadataBuilder;
    ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("navigation"), 10).ok());
    ASSERT_TRUE(setTitle(metadataBuilder, PdfMetadataOrigin::Info, "Info title").ok());
    ASSERT_TRUE(setTitle(metadataBuilder, PdfMetadataOrigin::Xmp, "XMP Navigation").ok());
    ASSERT_TRUE(setAuthor(metadataBuilder, PdfMetadataOrigin::Xmp, "XMP Author").ok());
    ASSERT_TRUE(setLanguage(metadataBuilder, PdfMetadataOrigin::Catalog, "de-CH").ok());
    PdfMetadata metadata = metadataBuilder.metadata();
    metadata.sectionCount = 2;
    metadata.outlineCount = 3;
    metadata.totalWords = 10;
    SectionTable sections{{
        {.byteSize = static_cast<uint32_t>(section0Record.size),
         .cumulativeSize = static_cast<uint32_t>(section0Record.size),
         .firstWordOrdinal = 0,
         .wordCount = 4,
         .firstAnchorOrdinal = 0,
         .tocIndex = 0},
        {.byteSize = static_cast<uint32_t>(section1Record.size),
         .cumulativeSize = static_cast<uint32_t>(section0Record.size + section1Record.size),
         .firstWordOrdinal = 4,
         .wordCount = 6,
         .firstAnchorOrdinal = 3,
         .tocIndex = 2},
    }};
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(pdfEncodeMetadata(metadata, sections.source(), metadataBytes.sink()).ok());
    addRequired("gen_7/metadata.bin", std::string(metadataBytes.bytes().begin(), metadataBytes.bytes().end()));

    std::array<PdfOutlineEntry, 3> outlineWorkspace{};
    PdfOutlineBuilder outlineBuilder({outlineWorkspace.data(), static_cast<uint16_t>(outlineWorkspace.size())});
    ASSERT_TRUE(outlineBuilder.begin().ok());
    ASSERT_TRUE(outlineBuilder.append(candidate(10, -1, "Part One", 0, 0)).ok());
    ASSERT_TRUE(outlineBuilder.append(candidate(11, 0, "Chapter One", 0, 0)).ok());
    ASSERT_TRUE(outlineBuilder.append(candidate(12, 0, "Chapter Two", 1, 3)).ok());
    ASSERT_TRUE(outlineBuilder.finish(reinterpret_cast<const uint8_t*>("fallback"), 8).ok());
    OutlineTable outline;
    outline.records.assign(outlineWorkspace.begin(), outlineWorkspace.end());
    PdfTestByteSink outlineBytes;
    ASSERT_TRUE(pdfEncodeOutline(outline.source(), outlineBytes.sink()).ok());
    addRequired("gen_7/outline.bin", std::string(outlineBytes.bytes().begin(), outlineBytes.bytes().end()));

    PdfCacheManifest manifest{};
    manifest.sequence = 3;
    manifest.completed = true;
    manifest.source = identity;
    manifest.generation = 7;
    manifest.totalWords = metadata.totalWords;
    manifest.requiredFileCount = static_cast<uint32_t>(required.records.size());
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    for (const auto& record : required.records) {
      manifest.requiredFileBytes += record.size;
      manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, record);
    }
    const PdfCacheCommitEvidence evidence{
        true,
        manifest.requiredFileCount,
        manifest.requiredFileBytes,
        manifest.requiredFileLedger,
    };
    const PdfCacheManifestSelection prior{};
    ASSERT_TRUE(cache.commitManifest(manifest, required.source(), evidence, prior, nullptr).ok());
    storage.clearEvents();
  }
};

PdfOutlineCandidate candidate(const uint32_t objectNumber, const int16_t parentIndex, const char* title,
                              const uint16_t sectionIndex, const uint32_t anchorOrdinal) {
  PdfOutlineCandidate result{};
  result.reference = {objectNumber, 0};
  result.parentIndex = parentIndex;
  result.destination = {sectionIndex, anchorOrdinal, 0, true};
  result.title = reinterpret_cast<const uint8_t*>(title);
  result.titleLength = std::strlen(title);
  return result;
}

std::vector<uint8_t> loadNavigationFixture() {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" /
                                     "fixtures" / "navigation_outline.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct NavigationParserHarness {
  std::vector<uint8_t> fixture = loadNavigationFixture();
  PdfTestByteSource bytes{fixture};
  PdfByteSource source = bytes.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<PdfValue, 256> values{};
  std::array<PdfDictionaryEntry, 256> dictionaries{};
  std::array<PdfArrayItem, 256> arrays{};
  std::array<uint8_t, 4096> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore xrefRecords{sizeof(PdfXrefEntry), 128};
  PdfXrefTable xref{xrefRecords.store()};
  PdfObjectResolver resolver{source, xref, sourceBuffer.data(), sourceBuffer.size(), arena};

  PdfStepResult run(auto& stepper) {
    while (true) {
      PdfWorkBudget budget{4, 64};
      const PdfStepResult result = stepper.step(budget);
      if (!result.yielded()) {
        return result;
      }
    }
  }

  void parseXref() {
    PdfXrefParser parser(source, sourceBuffer.data(), sourceBuffer.size(), arena, xref);
    parser.begin();
    ASSERT_TRUE(run(parser).complete());
  }

  PdfResolvedObject resolve(const PdfObjectReference reference) {
    EXPECT_TRUE(resolver.begin(reference).ok());
    EXPECT_TRUE(run(resolver).complete());
    return resolver.result();
  }
};

}  // namespace

TEST(PdfMetadataStore, AppliesDeterministicSourcePrecedenceAndUtf8Bounds) {
  PdfMetadataBuilder builder;
  ASSERT_TRUE(builder.begin(reinterpret_cast<const uint8_t*>("fallback.pdf"), 12).ok());

  ASSERT_TRUE(setTitle(builder, PdfMetadataOrigin::Info, "Info title").ok());
  ASSERT_TRUE(setTitle(builder, PdfMetadataOrigin::Xmp, "XMP title").ok());
  ASSERT_TRUE(setTitle(builder, PdfMetadataOrigin::Info, "late info").ok());
  ASSERT_TRUE(setAuthor(builder, PdfMetadataOrigin::Info, "Info author").ok());
  ASSERT_TRUE(setAuthor(builder, PdfMetadataOrigin::Xmp, "XMP author").ok());
  ASSERT_TRUE(setLanguage(builder, PdfMetadataOrigin::Xmp, "de").ok());
  ASSERT_TRUE(setLanguage(builder, PdfMetadataOrigin::Catalog, "de-CH").ok());
  ASSERT_TRUE(setLanguage(builder, PdfMetadataOrigin::Xmp, "fr").ok());

  const PdfMetadata& metadata = builder.metadata();
  EXPECT_STREQ(metadata.title, "XMP title");
  EXPECT_STREQ(metadata.author, "XMP author");
  EXPECT_STREQ(metadata.language, "de-CH");

  std::string longTitle(PdfMetadataLimits::TitleBytes - 1, 'a');
  longTitle += "\xC3\xA9";
  ASSERT_TRUE(
      builder.setTitle(PdfMetadataOrigin::Xmp, reinterpret_cast<const uint8_t*>(longTitle.data()), longTitle.size())
          .ok());
  EXPECT_EQ(builder.metadata().titleLength, PdfMetadataLimits::TitleBytes - 1);
  EXPECT_EQ(builder.metadata().title[PdfMetadataLimits::TitleBytes - 1], '\0');

  static constexpr uint8_t malformed[] = {0xC3, '('};
  EXPECT_EQ(builder.setAuthor(PdfMetadataOrigin::Xmp, malformed, sizeof(malformed)).error, PdfError::Malformed);
  EXPECT_STREQ(builder.metadata().author, "XMP author");
}

TEST(PdfMetadataStore, RoundTripsBoundedSectionsAndRejectsCorruption) {
  PdfMetadataBuilder builder;
  ASSERT_TRUE(builder.begin(reinterpret_cast<const uint8_t*>("book"), 4).ok());
  ASSERT_TRUE(setTitle(builder, PdfMetadataOrigin::Info, "A Book").ok());
  ASSERT_TRUE(setAuthor(builder, PdfMetadataOrigin::Info, "A Writer").ok());
  ASSERT_TRUE(setLanguage(builder, PdfMetadataOrigin::Catalog, "en-GB").ok());

  PdfMetadata metadata = builder.metadata();
  metadata.totalWords = 10;
  metadata.outlineCount = 2;
  SectionTable sections{{
      {.byteSize = 120,
       .cumulativeSize = 120,
       .firstWordOrdinal = 0,
       .wordCount = 4,
       .firstAnchorOrdinal = 0,
       .tocIndex = 0},
      {.byteSize = 150,
       .cumulativeSize = 270,
       .firstWordOrdinal = 4,
       .wordCount = 6,
       .firstAnchorOrdinal = 2,
       .tocIndex = 1},
  }};
  metadata.sectionCount = static_cast<uint16_t>(sections.records.size());

  PdfTestByteSink sink;
  sink.setMaximumWrite(PdfMetadataLimits::IoChunkBytes);
  ASSERT_TRUE(pdfEncodeMetadata(metadata, sections.source(), sink.sink()).ok());

  PdfMetadata decoded{};
  SectionCollector collector;
  PdfTestByteSource source(sink.bytes());
  ASSERT_TRUE(pdfDecodeMetadata(source.source(), &decoded, collector.visitor()).ok());
  EXPECT_STREQ(decoded.title, "A Book");
  EXPECT_STREQ(decoded.author, "A Writer");
  EXPECT_STREQ(decoded.language, "en-GB");
  EXPECT_EQ(decoded.totalWords, 10u);
  ASSERT_EQ(collector.records.size(), 2u);
  EXPECT_EQ(collector.records[1].firstWordOrdinal, 4u);
  EXPECT_EQ(collector.records[1].tocIndex, 1);

  std::vector<uint8_t> corrupted = sink.bytes();
  corrupted[corrupted.size() / 2] ^= 0x40;
  PdfTestByteSource corruptSource(corrupted);
  SectionCollector ignored;
  EXPECT_EQ(pdfDecodeMetadata(corruptSource.source(), &decoded, ignored.visitor()).error, PdfError::Malformed);
}

TEST(PdfOutline, PreservesHierarchyAndDestinationsWithHardCycleAndDepthLimits) {
  std::array<PdfOutlineEntry, 8> entries{};
  PdfOutlineBuilder builder({entries.data(), static_cast<uint16_t>(entries.size())});
  ASSERT_TRUE(builder.begin().ok());
  ASSERT_TRUE(builder.append(candidate(10, -1, "Part One", 0, 0)).ok());
  ASSERT_TRUE(builder.append(candidate(11, 0, "Chapter One", 0, 1)).ok());
  ASSERT_TRUE(builder.append(candidate(12, 0, "Chapter Two", 1, 2)).ok());
  ASSERT_TRUE(builder.finish(reinterpret_cast<const uint8_t*>("Fallback"), 8).ok());

  ASSERT_EQ(builder.count(), 3);
  EXPECT_EQ(entries[0].level, 1);
  EXPECT_EQ(entries[1].level, 2);
  EXPECT_EQ(entries[1].parentIndex, 0);
  EXPECT_EQ(entries[2].sectionIndex, 1);
  EXPECT_EQ(entries[2].anchorOrdinal, 2u);
  EXPECT_STREQ(entries[2].title, "Chapter Two");

  EXPECT_EQ(builder.append(candidate(12, -1, "cycle", 0, 0)).error, PdfError::Malformed);

  std::array<PdfOutlineEntry, PdfOutlineLimits::MaxDepth + 2> deepEntries{};
  PdfOutlineBuilder deep({deepEntries.data(), static_cast<uint16_t>(deepEntries.size())});
  ASSERT_TRUE(deep.begin().ok());
  for (uint8_t level = 0; level < PdfOutlineLimits::MaxDepth; ++level) {
    ASSERT_TRUE(deep.append(candidate(100 + level, level == 0 ? -1 : level - 1, "nested", 0, level)).ok());
  }
  EXPECT_EQ(deep.append(candidate(999, PdfOutlineLimits::MaxDepth - 1, "too deep", 0, 99)).error,
            PdfError::LimitExceeded);
}

TEST(PdfOutline, FallsBackToHeadingsThenToOneDocumentRoot) {
  std::array<PdfOutlineEntry, 4> headingEntries{};
  PdfOutlineBuilder headings({headingEntries.data(), static_cast<uint16_t>(headingEntries.size())});
  ASSERT_TRUE(headings.begin().ok());
  ASSERT_TRUE(headings.appendHeading(reinterpret_cast<const uint8_t*>("First heading"), 13, 0, 4, 2).ok());
  ASSERT_TRUE(headings.appendHeading(reinterpret_cast<const uint8_t*>("Second heading"), 14, 1, 9, 3).ok());
  ASSERT_TRUE(headings.finish(reinterpret_cast<const uint8_t*>("Ignored"), 7).ok());
  ASSERT_EQ(headings.count(), 2);
  EXPECT_STREQ(headingEntries[0].title, "First heading");
  EXPECT_EQ(headingEntries[1].sectionIndex, 1);

  std::array<PdfOutlineEntry, 1> rootEntry{};
  PdfOutlineBuilder root({rootEntry.data(), static_cast<uint16_t>(rootEntry.size())});
  ASSERT_TRUE(root.begin().ok());
  ASSERT_TRUE(root.finish(reinterpret_cast<const uint8_t*>("Fallback title"), 14).ok());
  ASSERT_EQ(root.count(), 1);
  EXPECT_STREQ(rootEntry[0].title, "Fallback title");
  EXPECT_EQ(rootEntry[0].sectionIndex, 0);
  EXPECT_STREQ(rootEntry[0].anchor, "b00000000");
}

TEST(PdfOutline, ResolvesOnlyInternalActionsAndFormatsStableCrossSectionHrefs) {
  char href[PdfOutlineLimits::HrefBytes]{};
  size_t length = 0;
  const PdfResolvedDestination target{7, 0x2a, 4, true};
  ASSERT_TRUE(pdfResolveInternalAction(PdfActionKind::GoTo, target, href, sizeof(href), &length).ok());
  EXPECT_STREQ(href, "sections/000007.xhtml#b0000002a");
  EXPECT_EQ(length, std::strlen(href));

  for (const PdfActionKind action : {PdfActionKind::Uri, PdfActionKind::Launch, PdfActionKind::JavaScript,
                                     PdfActionKind::Attachment, PdfActionKind::RemoteGoTo}) {
    href[0] = 'x';
    length = 99;
    EXPECT_EQ(pdfResolveInternalAction(action, target, href, sizeof(href), &length).error, PdfError::Unsupported);
    EXPECT_EQ(href[0], '\0');
    EXPECT_EQ(length, 0u);
  }

  const PdfResolvedDestination unresolved{};
  EXPECT_EQ(pdfResolveInternalAction(PdfActionKind::GoTo, unresolved, href, sizeof(href), &length).error,
            PdfError::InvalidOffset);
}

TEST(PdfOutline, FormatsBoundedPageLabelRangesWithoutReaderPageBreaks) {
  std::array<PdfPageLabelRange, 3> ranges{};
  PdfPageLabelMap labels({ranges.data(), static_cast<uint8_t>(ranges.size())});
  ASSERT_TRUE(labels.begin().ok());
  ASSERT_TRUE(labels.add({0, 1, PdfPageLabelStyle::LowerRoman, "", 0}).ok());
  ASSERT_TRUE(labels.add({4, 1, PdfPageLabelStyle::Decimal, "A-", 2}).ok());
  ASSERT_TRUE(labels.add({8, 3, PdfPageLabelStyle::UpperAlpha, "", 0}).ok());

  char label[PdfSemanticWriterLimits::PublisherLabelBytes]{};
  size_t length = 0;
  ASSERT_TRUE(labels.format(0, label, sizeof(label), &length).ok());
  EXPECT_STREQ(label, "i");
  ASSERT_TRUE(labels.format(3, label, sizeof(label), &length).ok());
  EXPECT_STREQ(label, "iv");
  ASSERT_TRUE(labels.format(4, label, sizeof(label), &length).ok());
  EXPECT_STREQ(label, "A-1");
  ASSERT_TRUE(labels.format(9, label, sizeof(label), &length).ok());
  EXPECT_STREQ(label, "D");

  PdfTestByteSink output;
  std::array<uint8_t, PdfSemanticWriterLimits::MinimumOutputBufferBytes> workspace{};
  PdfSemanticWriter writer;
  ASSERT_TRUE(writer
                  .begin(output.sink(),
                         {nullptr, [](void*, const PdfSemanticBlockRecord&) { return PdfStatus::success(); }},
                         {workspace.data(), workspace.size()})
                  .ok());
  ASSERT_TRUE(writer.writePublisherPageBreak(4, reinterpret_cast<const uint8_t*>("A-1"), 3).ok());
  ASSERT_TRUE(writer.finish().ok());
  const std::string xhtml(output.bytes().begin(), output.bytes().end());
  EXPECT_NE(xhtml.find("id=\"p00000004\""), std::string::npos);
  EXPECT_NE(xhtml.find("role=\"doc-pagebreak\""), std::string::npos);
  EXPECT_EQ(xhtml.find("break-before"), std::string::npos);
}

TEST(PdfOutline, RoundTripsFixedRecordsAndRejectsMalformedParentCycles) {
  OutlineTable table;
  table.records.resize(2);
  std::array<PdfOutlineEntry, 2> workspace{};
  PdfOutlineBuilder builder({workspace.data(), static_cast<uint16_t>(workspace.size())});
  ASSERT_TRUE(builder.begin().ok());
  ASSERT_TRUE(builder.append(candidate(1, -1, "Contents", 0, 0)).ok());
  ASSERT_TRUE(builder.append(candidate(2, 0, "Index", 1, 3)).ok());
  ASSERT_TRUE(builder.finish(reinterpret_cast<const uint8_t*>("Fallback"), 8).ok());
  table.records.assign(workspace.begin(), workspace.end());

  PdfTestByteSink sink;
  sink.setMaximumWrite(PdfOutlineLimits::EncodedRecordBytes);
  ASSERT_TRUE(pdfEncodeOutline(table.source(), sink.sink()).ok());

  PdfTestByteSource source(sink.bytes());
  PdfOutlineHeader header{};
  OutlineCollector collector;
  ASSERT_TRUE(pdfDecodeOutline(source.source(), &header, collector.visitor()).ok());
  EXPECT_EQ(header.entryCount, 2);
  ASSERT_EQ(collector.records.size(), 2u);
  EXPECT_STREQ(collector.records[1].title, "Index");
  EXPECT_EQ(collector.records[1].parentIndex, 0);

  table.records[0].parentIndex = 1;
  table.records[1].parentIndex = 0;
  PdfTestByteSink cyclicSink;
  EXPECT_EQ(pdfEncodeOutline(table.source(), cyclicSink.sink()).error, PdfError::Malformed);
}

TEST(PdfOutline, ParsesGeneratedCatalogMetadataDestinationsLabelsAndSafeActions) {
  NavigationParserHarness parser;
  parser.parseXref();
  PdfObjectReference catalogReference{};
  ASSERT_TRUE(parser.xref.root(&catalogReference));
  PdfObjectReference infoReference{};
  ASSERT_TRUE(parser.xref.info(&infoReference));
  EXPECT_EQ(infoReference.objectNumber, 30U);
  const PdfResolvedObject catalogObject = parser.resolve(catalogReference);

  PdfCatalogNavigation catalog{};
  ASSERT_TRUE(pdfReadCatalogNavigation(parser.arena, catalogObject.rootIndex, &catalog).ok());
  EXPECT_TRUE(catalog.hasPages);
  EXPECT_EQ(catalog.pages.objectNumber, 2u);
  EXPECT_TRUE(catalog.hasOutlines);
  EXPECT_EQ(catalog.outlines.objectNumber, 10u);
  EXPECT_TRUE(catalog.hasNamedDestinations);
  EXPECT_EQ(catalog.namedDestinations.objectNumber, 20u);
  EXPECT_TRUE(catalog.hasPageLabels);
  EXPECT_EQ(catalog.pageLabels.objectNumber, 25u);
  EXPECT_TRUE(catalog.hasMetadata);
  EXPECT_EQ(catalog.metadata.objectNumber, 31u);
  EXPECT_STREQ(catalog.language, "de-CH");

  const PdfResolvedObject outlineRootObject = parser.resolve(catalog.outlines);
  PdfObjectReference firstOutline{};
  ASSERT_TRUE(pdfReadOutlineRoot(parser.arena, outlineRootObject.rootIndex, &firstOutline).ok());
  EXPECT_EQ(firstOutline.objectNumber, 11u);

  const PdfResolvedObject partObject = parser.resolve(firstOutline);
  PdfRawOutlineNode part{};
  ASSERT_TRUE(pdfReadOutlineNode(parser.arena, partObject.rootIndex, &part).ok());
  EXPECT_STREQ(part.title, "Part One");
  EXPECT_TRUE(part.hasFirstChild);
  EXPECT_EQ(part.firstChild.objectNumber, 12u);
  EXPECT_EQ(part.destination.kind, PdfRawDestinationKind::Named);
  EXPECT_STREQ(part.destination.name, "part-one");

  const PdfResolvedObject chapterObject = parser.resolve(part.firstChild);
  PdfRawOutlineNode chapter{};
  ASSERT_TRUE(pdfReadOutlineNode(parser.arena, chapterObject.rootIndex, &chapter).ok());
  EXPECT_STREQ(chapter.title, "Chapter One");
  EXPECT_EQ(chapter.destination.kind, PdfRawDestinationKind::Explicit);
  EXPECT_EQ(chapter.destination.pageReference.objectNumber, 3u);
  EXPECT_TRUE(chapter.hasNext);
  EXPECT_EQ(chapter.next.objectNumber, 13u);

  std::array<PdfNamedDestinationRecord, 8> namedWorkspace{};
  PdfNamedDestinationMap named({namedWorkspace.data(), static_cast<uint8_t>(namedWorkspace.size())});
  ASSERT_TRUE(named.begin().ok());
  const PdfResolvedObject namesObject = parser.resolve(catalog.namedDestinations);
  ASSERT_TRUE(pdfReadNamedDestinations(parser.arena, namesObject.rootIndex, &named).ok());
  PdfRawDestination namedChapter{};
  ASSERT_TRUE(named.resolve(reinterpret_cast<const uint8_t*>("chapter-two"), 11, &namedChapter).ok());
  EXPECT_EQ(namedChapter.kind, PdfRawDestinationKind::Explicit);
  EXPECT_EQ(namedChapter.pageReference.objectNumber, 6u);

  std::array<PdfPageLabelRange, 8> labelWorkspace{};
  PdfPageLabelMap labels({labelWorkspace.data(), static_cast<uint8_t>(labelWorkspace.size())});
  ASSERT_TRUE(labels.begin().ok());
  const PdfResolvedObject labelsObject = parser.resolve(catalog.pageLabels);
  ASSERT_TRUE(pdfReadPageLabels(parser.arena, labelsObject.rootIndex, &labels).ok());
  char label[PdfSemanticWriterLimits::PublisherLabelBytes]{};
  size_t labelLength = 0;
  ASSERT_TRUE(labels.format(0, label, sizeof(label), &labelLength).ok());
  EXPECT_STREQ(label, "i");
  ASSERT_TRUE(labels.format(1, label, sizeof(label), &labelLength).ok());
  EXPECT_STREQ(label, "A-1");

  PdfMetadataBuilder metadataBuilder;
  ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("fallback"), 8).ok());
  ASSERT_TRUE(pdfApplyCatalogMetadata(catalog, &metadataBuilder).ok());
  const PdfResolvedObject infoObject = parser.resolve({30, 0});
  ASSERT_TRUE(pdfApplyInfoMetadata(parser.arena, infoObject.rootIndex, &metadataBuilder).ok());
  const PdfResolvedObject metadataObject = parser.resolve(catalog.metadata);
  ASSERT_TRUE(metadataObject.hasStream);
  std::vector<uint8_t> xmp(static_cast<size_t>(metadataObject.streamLength));
  size_t bytesRead = 0;
  ASSERT_TRUE(
      parser.source.readAt(parser.source.context, metadataObject.streamOffset, xmp.data(), xmp.size(), &bytesRead)
          .ok());
  ASSERT_EQ(bytesRead, xmp.size());
  ASSERT_TRUE(pdfApplyXmpMetadata(xmp.data(), xmp.size(), &metadataBuilder).ok());
  EXPECT_STREQ(metadataBuilder.metadata().title, "XMP Navigation");
  EXPECT_STREQ(metadataBuilder.metadata().author, "XMP Author");
  EXPECT_STREQ(metadataBuilder.metadata().language, "de-CH");

  const PdfResolvedObject safeAnnotationObject = parser.resolve({40, 0});
  PdfRawLinkAnnotation safe{};
  ASSERT_TRUE(pdfReadLinkAnnotation(parser.arena, safeAnnotationObject.rootIndex, &safe).ok());
  EXPECT_EQ(safe.action, PdfActionKind::GoTo);
  EXPECT_EQ(safe.destination.kind, PdfRawDestinationKind::Named);
  EXPECT_STREQ(safe.destination.name, "chapter-two");

  const PdfResolvedObject uriAnnotationObject = parser.resolve({42, 0});
  PdfRawLinkAnnotation unsafe{};
  ASSERT_TRUE(pdfReadLinkAnnotation(parser.arena, uriAnnotationObject.rootIndex, &unsafe).ok());
  EXPECT_EQ(unsafe.action, PdfActionKind::Uri);
  EXPECT_EQ(unsafe.destination.kind, PdfRawDestinationKind::None);
}

TEST(PdfNavigationDocument, LoadsMetadataSectionsAndHierarchicalTocWithoutReopeningSource) {
  NavigableCacheFixture fixture;
  fixture.build();
  const uint32_t sourceOpensBefore = fixture.storage.openCallsForPath(NavigableCacheFixture::SourcePath);

  PdfReflowDocument document;
  ASSERT_TRUE(
      document
          .initialize(fixture.storage.io(), NavigableCacheFixture::SourcePath, NavigableCacheFixture::CacheDirectory)
          .ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  EXPECT_EQ(document.getTitle(), "XMP Navigation");
  EXPECT_EQ(document.getAuthor(), "XMP Author");
  EXPECT_EQ(document.getLanguage(), "de-CH");
  EXPECT_EQ(document.getSectionCount(), 2);
  EXPECT_EQ(document.getTocEntryCount(), 3);
  EXPECT_EQ(document.getTotalWordCount(), 10u);

  const ReflowSectionInfo first = document.getSectionInfo(0);
  const ReflowSectionInfo second = document.getSectionInfo(1);
  EXPECT_EQ(first.href, "sections/000000.xhtml");
  EXPECT_EQ(first.wordCount, 4u);
  EXPECT_EQ(second.href, "sections/000001.xhtml");
  EXPECT_EQ(second.firstWordOrdinal, 4u);
  EXPECT_EQ(second.wordCount, 6u);
  EXPECT_EQ(second.tocIndex, 2);

  const ReflowTocEntry root = document.getTocEntry(0);
  const ReflowTocEntry chapter = document.getTocEntry(2);
  EXPECT_EQ(root.title, "Part One");
  EXPECT_EQ(root.level, 1);
  EXPECT_EQ(root.parentIndex, -1);
  EXPECT_EQ(chapter.title, "Chapter Two");
  EXPECT_EQ(chapter.level, 2);
  EXPECT_EQ(chapter.parentIndex, 0);
  EXPECT_EQ(chapter.sectionIndex, 1);
  EXPECT_EQ(chapter.anchor, "b00000003");
  EXPECT_EQ(chapter.href, "sections/000001.xhtml#b00000003");
  EXPECT_EQ(document.getSectionIndexForTocIndex(2), 1);
  EXPECT_EQ(document.getTocIndexForSectionIndex(1), 2);
  EXPECT_EQ(document.resolveHrefToSectionIndex("sections/000001.xhtml#b00000003"), 1);

  ReflowResource local;
  ASSERT_TRUE(document.getImmutableLocalSection(1, local));
  EXPECT_NE(local.localPath.find("/gen_7/sections/000001.xhtml"), std::string::npos);
  BufferPrint stream;
  ASSERT_TRUE(document.streamSection(1, stream, 31));
  const std::string xhtml(stream.output.begin(), stream.output.end());
  EXPECT_NE(xhtml.find("aria-label=\"A-1\""), std::string::npos);
  EXPECT_NE(xhtml.find("sections/000000.xhtml#b00000000"), std::string::npos);

  EXPECT_EQ(fixture.storage.openCallsForPath(NavigableCacheFixture::SourcePath), sourceOpensBefore + 1);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0u);
}

TEST(PdfNavigationDocument, RejectsCorruptOutlineBeforeExposingNavigation) {
  NavigableCacheFixture fixture;
  fixture.build();
  fixture.storage.corruptByte(fixture.cacheRoot + "/gen_7/outline.bin", 24, 0x20);

  PdfReflowDocument document;
  ASSERT_TRUE(
      document
          .initialize(fixture.storage.io(), NavigableCacheFixture::SourcePath, NavigableCacheFixture::CacheDirectory)
          .ok());
  EXPECT_EQ(document.loadCompletedCache().error, PdfError::Malformed);
  EXPECT_EQ(document.getSectionCount(), 0);
  EXPECT_EQ(document.getTocEntryCount(), 0);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0u);
}
