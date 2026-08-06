#include "production_stubs/EpubProductionTestState.h"

#include "Epub/Epub.h"
#include "Epub/Epub/Page.h"
#include "Epub/Epub/css/CssParser.h"
#include "Epub/Epub/hyphenation/Hyphenator.h"
#include "Epub/Epub/parsers/ChapterHtmlSlimParser.h"
#include "Epub/Epub/parsers/ContainerParser.h"
#include "Epub/Epub/parsers/ContentOpfParser.h"
#include "Epub/Epub/parsers/TocNavParser.h"
#include "Epub/Epub/parsers/TocNcxParser.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string normaliseFixturePath(const std::string_view path, const bool preserveRoot) {
  const bool rooted = preserveRoot && !path.empty() && path.front() == '/';
  std::vector<std::string_view> components;
  components.reserve(8);
  size_t start = 0;
  for (size_t index = 0; index <= path.size(); ++index) {
    if (index != path.size() && path[index] != '/') {
      continue;
    }
    if (index > start) {
      const std::string_view component = path.substr(start, index - start);
      if (component == "..") {
        if (!components.empty()) {
          components.pop_back();
        }
      } else {
        components.push_back(component);
      }
    }
    start = index + 1;
  }

  std::string result;
  if (rooted && !components.empty()) {
    result.push_back('/');
  }
  for (size_t index = 0; index < components.size(); ++index) {
    if (index > 0) {
      result.push_back('/');
    }
    result.append(components[index]);
  }
  return result;
}

}  // namespace

bool BookMetadataCache::beginWrite() { return false; }
bool BookMetadataCache::beginContentOpfPass() { return false; }
void BookMetadataCache::createSpineEntry(const std::string&) {}
bool BookMetadataCache::endContentOpfPass() { return false; }
bool BookMetadataCache::beginTocPass() { return false; }
void BookMetadataCache::createTocEntry(const std::string&, const std::string&, const std::string&, uint8_t) {}
bool BookMetadataCache::endTocPass() { return false; }
bool BookMetadataCache::endWrite() { return false; }
bool BookMetadataCache::cleanupTmpFiles() const { return true; }
bool BookMetadataCache::buildBookBin(const std::string&, const BookMetadata&) { return false; }
bool BookMetadataCache::exists(const std::string&) { return true; }

bool BookMetadataCache::load() {
  const auto& fixture = epub_production_test::metadata;
  loaded = fixture.loadSucceeds;
  spineCount = static_cast<uint16_t>(fixture.spines.size());
  tocCount = static_cast<uint16_t>(fixture.toc.size());
  coreMetadata.title = "Fixture book";
  coreMetadata.author = "Fixture author";
  coreMetadata.language = "en";
  return loaded;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  ++epub_production_test::metadata.spineEntryQueries;
  if (index < 0 || index >= static_cast<int>(epub_production_test::metadata.spines.size())) return {};
  const auto& fixture = epub_production_test::metadata.spines[static_cast<size_t>(index)];
  return {fixture.href, fixture.cumulativeSize, fixture.tocIndex};
}

size_t BookMetadataCache::getSpineCumulativeSize(const int index) {
  ++epub_production_test::metadata.cumulativeSizeQueries;
  if (index < 0 || index >= static_cast<int>(epub_production_test::metadata.spines.size())) return 0;
  return epub_production_test::metadata.spines[static_cast<size_t>(index)].cumulativeSize;
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  ++epub_production_test::metadata.tocEntryQueries;
  if (index < 0 || index >= static_cast<int>(epub_production_test::metadata.toc.size())) return {};
  const auto& fixture = epub_production_test::metadata.toc[static_cast<size_t>(index)];
  return {fixture.title, fixture.href, fixture.anchor, fixture.level, fixture.spineIndex};
}

bool CssParser::loadFromStream(HalFile&) { return false; }
bool CssParser::hasCache() const { return true; }
void CssParser::deleteCache() const {}
bool CssParser::saveToCache(bool) const { return false; }
bool CssParser::loadFromCache() { return false; }
size_t CssParser::SvHash::operator()(const std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}
size_t CssParser::SvHash::operator()(const std::string& value) const noexcept {
  return (*this)(std::string_view(value));
}
size_t CssParser::SvHash::operator()(const CompositeKey value) const noexcept {
  size_t hash = 0;
  for (const std::string_view piece : value.pieces) {
    hash ^= (*this)(piece) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
  }
  return hash;
}
bool CssParser::SvEqual::operator()(const std::string_view left, const std::string_view right) const noexcept {
  return left == right;
}
bool CssParser::SvEqual::operator()(const std::string& left, const std::string_view right) const noexcept {
  return (*this)(std::string_view(left), right);
}
bool CssParser::SvEqual::operator()(const std::string_view left, const std::string& right) const noexcept {
  return (*this)(left, std::string_view(right));
}
bool CssParser::SvEqual::operator()(const std::string& left, const std::string& right) const noexcept {
  return (*this)(std::string_view(left), std::string_view(right));
}
bool CssParser::SvEqual::operator()(const CompositeKey left, const std::string_view right) const noexcept {
  size_t offset = 0;
  for (const std::string_view piece : left.pieces) {
    if (offset + piece.size() > right.size() || right.substr(offset, piece.size()) != piece) return false;
    offset += piece.size();
  }
  return offset == right.size();
}
bool CssParser::SvEqual::operator()(const std::string_view left, const CompositeKey right) const noexcept {
  return (*this)(right, left);
}

ContainerParser::~ContainerParser() = default;
bool ContainerParser::setup() { return false; }
size_t ContainerParser::write(uint8_t) { return 0; }
size_t ContainerParser::write(const uint8_t*, size_t) { return 0; }

ContentOpfParser::~ContentOpfParser() = default;
bool ContentOpfParser::setup() { return false; }
size_t ContentOpfParser::write(uint8_t) { return 0; }
size_t ContentOpfParser::write(const uint8_t*, size_t) { return 0; }

TocNavParser::~TocNavParser() = default;
bool TocNavParser::setup() { return false; }
size_t TocNavParser::write(uint8_t) { return 0; }
size_t TocNavParser::write(const uint8_t*, size_t) { return 0; }

TocNcxParser::~TocNcxParser() = default;
bool TocNcxParser::setup() { return false; }
size_t TocNcxParser::write(uint8_t) { return 0; }
size_t TocNcxParser::write(const uint8_t*, size_t) { return 0; }

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() = default;
bool ChapterHtmlSlimParser::parseAndBuildPages() {
  auto& fixture = epub_production_test::parser;
  fixture.parsePath = filepath;
  fixture.contentBase = contentBase;
  fixture.semanticPaginationHooksPresent =
      paginationHooks_.context != nullptr && paginationHooks_.vtable != nullptr;
  if (fixture.mode == epub_production_test::ParserMode::Reject) {
    return false;
  }
  if (fixture.mode == epub_production_test::ParserMode::CaptureOnly) {
    return true;
  }

  fixture.preserveImagePathRoot = shouldPreserveImagePathRoot();
  fixture.resolvedImageHref =
      normaliseFixturePath(contentBase + fixture.relativeImageHref, fixture.preserveImagePathRoot);
  ReflowResource image;
  if (!sectionSource.getImmutableLocalResource(sectionIndex, fixture.resolvedImageHref, image)) {
    return false;
  }
  fixture.borrowedImagePath = image.localPath;
  fixture.borrowedImageWidth = image.width;
  fixture.borrowedImageHeight = image.height;
  fixture.borrowedPixelCache = image.imageKind == ReflowImageKind::PixelCache;
  const bool hasPaginationCompletion = fixture.semanticPaginationHooksPresent &&
                                       paginationHooks_.vtable->completePage != nullptr;
  if (!fixture.borrowedPixelCache || !Storage.exists(image.localPath.c_str()) ||
      (!completePageFn && !hasPaginationCompletion)) {
    return false;
  }

  for (uint16_t pageIndex = 0; pageIndex < fixture.pageCount; ++pageIndex) {
    auto page = std::make_unique<Page>();
    auto block = std::make_shared<ImageBlock>(image.localPath, static_cast<int16_t>(image.width),
                                              static_cast<int16_t>(image.height));
    page->elements.push_back(std::make_shared<PageImage>(std::move(block), 0, 0));
    if (hasPaginationCompletion) {
      paginationHooks_.vtable->completePage(
          paginationHooks_.context, std::move(page), static_cast<uint16_t>(fixture.paragraphIndex + pageIndex),
          static_cast<uint16_t>(fixture.listItemIndex + pageIndex));
    } else {
      completePageFn(std::move(page), static_cast<uint16_t>(fixture.paragraphIndex + pageIndex),
                     static_cast<uint16_t>(fixture.listItemIndex + pageIndex));
    }
  }
  return true;
}

void Hyphenator::setPreferredLanguage(const std::string&) {}

ImageBlock::ImageBlock(const std::string& imagePath, const int16_t width, const int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

void PageImage::render(GfxRenderer&, int, int, int, bool) {}
bool PageImage::serialize(FsFile&) { return true; }

bool Page::serialize(FsFile& file) const {
  auto& fixture = epub_production_test::parser;
  ++fixture.serializedPages;
  const uint8_t marker = static_cast<uint8_t>(fixture.serializedPageByte + fixture.serializedPages);
  if (file.write(&marker, 1) != 1) return false;
  for (const auto& element : elements) {
    if (!element || element->getTag() != TAG_PageImage) {
      continue;
    }
    const auto& pageImage = static_cast<const PageImage&>(*element);
    const ImageBlock& image = pageImage.getImageBlock();
    fixture.pageImageFound = true;
    fixture.pageImagePath = image.getImagePath();
    fixture.pageImageWidth = image.getWidth();
    fixture.pageImageHeight = image.getHeight();
  }
  return true;
}
std::unique_ptr<Page> Page::deserialize(FsFile&) { return nullptr; }
void PageLine::render(GfxRenderer&, int, int, int, bool) {}
bool PageLine::serialize(FsFile&) { return false; }
std::unique_ptr<PageLine> PageLine::deserialize(FsFile&) { return nullptr; }
