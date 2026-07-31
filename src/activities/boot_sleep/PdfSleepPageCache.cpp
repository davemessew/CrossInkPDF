#include "PdfSleepPageCache.h"

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF

#include <BidiUtils.h>
#include <EpdFontFamily.h>
#include <Epub/EpubRenderMode.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PdfCacheIo.h>
#include <PdfCachedProductState.h>
#include <PdfHalCacheIo.h>
#include <PdfLayoutWordIndex.h>
#include <PdfSourceIdentity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include "util/BookMoveUtils.h"

namespace {

constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;
constexpr uint8_t SECTION_FILE_VERSION = 44;
constexpr char PDF_CACHE_DIRECTORY[] = "/.crosspoint";
constexpr size_t SECTION_HEADER_BYTES = 44;
constexpr size_t PAGE_LUT_ENTRY_BYTES = sizeof(uint32_t);
constexpr uint16_t MAX_TEXT_BLOCK_WORDS = 512;
constexpr size_t TEXT_BLOCK_HEADER_BYTES = sizeof(uint16_t) + sizeof(uint8_t) * 3 + sizeof(uint16_t);
constexpr size_t TEXT_BLOCK_STYLE_BYTES =
    sizeof(uint8_t) + sizeof(bool) + sizeof(int16_t) * 9 + sizeof(bool) * 3;
constexpr size_t FOOTNOTE_BYTES = 32 + 96;
constexpr size_t PUBLISHER_MARKER_BYTES = sizeof(int16_t) + 16;
constexpr uint8_t TAG_PAGE_LINE = 1;
constexpr uint8_t TAG_PAGE_IMAGE = 2;
constexpr uint8_t TAG_PAGE_TABLE_FRAGMENT = 3;
constexpr uint8_t TAG_PAGE_HORIZONTAL_RULE = 4;
constexpr uint8_t MAX_TABLE_ROWS = 64;
constexpr uint8_t MAX_TABLE_CELLS = 8;
constexpr uint8_t MAX_TABLE_LINES = 64;
constexpr uint16_t MAX_PAGE_FOOTNOTES = 16;
constexpr uint8_t MAX_PAGE_MARKERS = 8;
constexpr uint8_t WORD_FLAG_BACKGROUND_BLACK = 0x01;

static_assert(sizeof(int) == 4, "section v44 stores four-byte font IDs");
static_assert(sizeof(float) == 4, "section v44 stores four-byte floats");
static_assert(sizeof(bool) == 1, "section v44 stores one-byte booleans");

uint16_t readLe16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readLe32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) | (static_cast<uint32_t>(source[3]) << 24U);
}

int16_t readLeI16(const uint8_t* const source) { return static_cast<int16_t>(readLe16(source)); }

bool readExact(FsFile& file, void* const destination, const size_t size) {
  return file.read(destination, size) == static_cast<int>(size);
}

bool readU32At(FsFile& file, const uint32_t offset, uint32_t* const value) {
  uint8_t encoded[sizeof(uint32_t)]{};
  if (value == nullptr || !file.seek(offset) || !readExact(file, encoded, sizeof(encoded))) {
    return false;
  }
  *value = readLe32(encoded);
  return true;
}

bool checkedAdd(const size_t left, const size_t right, size_t* const result) {
  if (result == nullptr || right > std::numeric_limits<size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool checkedMultiply(const size_t left, const size_t right, size_t* const result) {
  if (result == nullptr || (left != 0 && right > std::numeric_limits<size_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

// A cursor over the single immutable page record. Wider values are decoded
// bytewise so unaligned cache bytes are safe on RV32.
class MemoryPageCursor {
 public:
  MemoryPageCursor(const uint8_t* const data, const size_t size) : data_(data), size_(size) {}

  bool take(const size_t bytes, const uint8_t** const result) {
    if (result == nullptr || bytes > remaining()) {
      return false;
    }
    *result = data_ + position_;
    position_ += bytes;
    return true;
  }

  bool skip(const size_t bytes) {
    if (bytes > remaining()) {
      return false;
    }
    position_ += bytes;
    return true;
  }

  bool readU8(uint8_t* const value) {
    const uint8_t* bytes = nullptr;
    if (value == nullptr || !take(sizeof(uint8_t), &bytes)) {
      return false;
    }
    *value = bytes[0];
    return true;
  }

  bool readBool(bool* const value) {
    uint8_t encoded = 0;
    if (value == nullptr || !readU8(&encoded) || encoded > 1) {
      return false;
    }
    *value = encoded != 0;
    return true;
  }

  bool readU16(uint16_t* const value) {
    const uint8_t* bytes = nullptr;
    if (value == nullptr || !take(sizeof(uint16_t), &bytes)) {
      return false;
    }
    *value = readLe16(bytes);
    return true;
  }

  bool readI16(int16_t* const value) {
    const uint8_t* bytes = nullptr;
    if (value == nullptr || !take(sizeof(int16_t), &bytes)) {
      return false;
    }
    *value = readLeI16(bytes);
    return true;
  }

  bool readU32(uint32_t* const value) {
    const uint8_t* bytes = nullptr;
    if (value == nullptr || !take(sizeof(uint32_t), &bytes)) {
      return false;
    }
    *value = readLe32(bytes);
    return true;
  }

  size_t position() const { return position_; }
  size_t remaining() const { return size_ - position_; }
  bool atEnd() const { return position_ == size_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
};

struct DecodeBudget {
  uint32_t textBlocks = 0;
  uint32_t words = 0;
  uint32_t textBytes = 0;
  uint32_t tableRows = 0;
  uint32_t tableCells = 0;
  uint32_t workUnits = 0;

  static bool add(uint32_t* const value, const uint32_t increment, const uint32_t maximum) {
    if (value == nullptr || *value > maximum || increment > maximum - *value) {
      return false;
    }
    *value += increment;
    return true;
  }

  bool consumeElement() { return add(&workUnits, 1, PdfSleepPageCache::MAX_DECODED_WORK_UNITS); }

  bool consumeTextBlock(const uint16_t blockWords, const uint16_t blockTextBytes) {
    return add(&textBlocks, 1, PdfSleepPageCache::MAX_TOTAL_TEXT_BLOCKS) &&
           add(&words, blockWords, PdfSleepPageCache::MAX_TOTAL_WORDS) &&
           add(&textBytes, blockTextBytes, PdfSleepPageCache::MAX_TOTAL_TEXT_BYTES) &&
           add(&workUnits, static_cast<uint32_t>(blockWords) + blockTextBytes + 1U,
               PdfSleepPageCache::MAX_DECODED_WORK_UNITS);
  }

  bool consumeTable(const uint8_t rows) {
    return add(&tableRows, rows, PdfSleepPageCache::MAX_TOTAL_TABLE_ROWS) &&
           add(&workUnits, rows, PdfSleepPageCache::MAX_DECODED_WORK_UNITS);
  }

  bool consumeCells(const uint8_t cells) {
    return add(&tableCells, cells, PdfSleepPageCache::MAX_TOTAL_TABLE_CELLS) &&
           add(&workUnits, cells, PdfSleepPageCache::MAX_DECODED_WORK_UNITS);
  }
};

struct TextBlockView {
  uint16_t wordCount = 0;
  uint16_t textBytes = 0;
  bool hasBionic = false;
  bool hasGuideDots = false;
  bool hasWordFlags = false;
  bool isRtl = false;
  const uint8_t* textOffsets = nullptr;
  const uint8_t* xPositions = nullptr;
  const uint8_t* bionicSuffixX = nullptr;
  const uint8_t* guideDotX = nullptr;
  const uint8_t* styles = nullptr;
  const uint8_t* bionicBoundaries = nullptr;
  const uint8_t* wordFlags = nullptr;
  const char* text = nullptr;

  uint16_t textOffset(const uint16_t index) const { return readLe16(textOffsets + static_cast<size_t>(index) * 2U); }
  int16_t xPosition(const uint16_t index) const {
    return readLeI16(xPositions + static_cast<size_t>(index) * 2U);
  }
  uint16_t wordLength(const uint16_t index) const {
    const uint16_t end = index + 1U < wordCount ? textOffset(index + 1U) : textBytes;
    return static_cast<uint16_t>(end - textOffset(index) - 1U);
  }
  const char* word(const uint16_t index) const { return text + textOffset(index); }
  EpdFontFamily::Style style(const uint16_t index) const {
    return static_cast<EpdFontFamily::Style>(styles[index]);
  }
  uint8_t boundary(const uint16_t index) const { return hasBionic ? bionicBoundaries[index] : 0; }
  uint16_t suffixX(const uint16_t index) const {
    return hasBionic ? readLe16(bionicSuffixX + static_cast<size_t>(index) * 2U) : 0;
  }
  uint16_t dotX(const uint16_t index) const {
    return hasGuideDots ? readLe16(guideDotX + static_cast<size_t>(index) * 2U) : 0;
  }
  uint8_t flags(const uint16_t index) const { return hasWordFlags ? wordFlags[index] : 0; }
};

bool parseTextBlock(MemoryPageCursor& cursor, DecodeBudget* const budget, TextBlockView* const output) {
  uint16_t words = 0;
  uint8_t hasBionic = 0;
  uint8_t hasGuideDots = 0;
  uint8_t hasWordFlags = 0;
  uint16_t textBytes = 0;
  if (!cursor.readU16(&words) || !cursor.readU8(&hasBionic) || !cursor.readU8(&hasGuideDots) ||
      !cursor.readU8(&hasWordFlags) || !cursor.readU16(&textBytes)) {
    return false;
  }
  if (words > MAX_TEXT_BLOCK_WORDS || hasBionic > 1 || hasGuideDots > 1 || hasWordFlags > 1 ||
      (words == 0 && textBytes != 0) || (words > 0 && textBytes < words)) {
    return false;
  }

  size_t wordBytes = 0;
  size_t arenaBytes = 0;
  if (!checkedMultiply(words, sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t), &arenaBytes)) {
    return false;
  }
  if (hasBionic) {
    if (!checkedMultiply(words, sizeof(uint16_t) + sizeof(uint8_t), &wordBytes) ||
        !checkedAdd(arenaBytes, wordBytes, &arenaBytes)) {
      return false;
    }
  }
  if (hasGuideDots) {
    if (!checkedMultiply(words, sizeof(uint16_t), &wordBytes) || !checkedAdd(arenaBytes, wordBytes, &arenaBytes)) {
      return false;
    }
  }
  if (hasWordFlags) {
    if (!checkedMultiply(words, sizeof(uint8_t), &wordBytes) || !checkedAdd(arenaBytes, wordBytes, &arenaBytes)) {
      return false;
    }
  }
  if (!checkedAdd(arenaBytes, textBytes, &arenaBytes) ||
      (budget != nullptr && !budget->consumeTextBlock(words, textBytes))) {
    return false;
  }

  const uint8_t* arena = nullptr;
  if (!cursor.take(arenaBytes, &arena)) {
    return false;
  }
  TextBlockView view{};
  view.wordCount = words;
  view.textBytes = textBytes;
  view.hasBionic = hasBionic != 0;
  view.hasGuideDots = hasGuideDots != 0;
  view.hasWordFlags = hasWordFlags != 0;
  view.textOffsets = arena;
  view.xPositions = view.textOffsets + static_cast<size_t>(words) * sizeof(uint16_t);
  size_t offset = static_cast<size_t>(words) * (sizeof(uint16_t) + sizeof(int16_t));
  if (view.hasBionic) {
    view.bionicSuffixX = arena + offset;
    offset += static_cast<size_t>(words) * sizeof(uint16_t);
  }
  if (view.hasGuideDots) {
    view.guideDotX = arena + offset;
    offset += static_cast<size_t>(words) * sizeof(uint16_t);
  }
  view.styles = arena + offset;
  offset += static_cast<size_t>(words);
  if (view.hasBionic) {
    view.bionicBoundaries = arena + offset;
    offset += static_cast<size_t>(words);
  }
  if (view.hasWordFlags) {
    view.wordFlags = arena + offset;
    offset += static_cast<size_t>(words);
  }
  view.text = reinterpret_cast<const char*>(arena + offset);

  if (words > 0) {
    if (view.textOffset(0) != 0 || view.text[textBytes - 1U] != '\0') {
      return false;
    }
    for (uint16_t index = 1; index < words; ++index) {
      const uint16_t current = view.textOffset(index);
      if (current <= view.textOffset(index - 1U) || current >= textBytes || view.text[current - 1U] != '\0') {
        return false;
      }
    }
  }

  const uint8_t* style = nullptr;
  if (!cursor.take(TEXT_BLOCK_STYLE_BYTES, &style) || style[1] > 1 || style[20] > 1 || style[21] > 1 ||
      style[22] > 1) {
    return false;
  }
  view.isRtl = style[21] != 0;
  if (output != nullptr) {
    *output = view;
  }
  return true;
}

bool isWhitespaceOnlyBackgroundToken(const char* const word) {
  if (word == nullptr || word[0] == '\0') {
    return false;
  }
  for (size_t index = 0; word[index] != '\0';) {
    const uint8_t current = static_cast<uint8_t>(word[index]);
    if (current == ' ' || current == '\r' || current == '\n' || current == '\t') {
      ++index;
    } else if (current == 0xc2 && word[index + 1U] != '\0' &&
               static_cast<uint8_t>(word[index + 1U]) == 0xa0) {
      index += 2;
    } else if (current == 0xe2 && word[index + 1U] != '\0' && word[index + 2U] != '\0' &&
               static_cast<uint8_t>(word[index + 1U]) == 0x80 &&
               static_cast<uint8_t>(word[index + 2U]) == 0xaf) {
      index += 3;
    } else {
      return false;
    }
  }
  return true;
}

bool hasSyntheticIndentPrefix(const char* const word, const uint16_t length) {
  return length >= 3 && static_cast<uint8_t>(word[0]) == 0xe2 &&
         static_cast<uint8_t>(word[1]) == 0x80 && static_cast<uint8_t>(word[2]) == 0x83;
}

uint16_t measureBackgroundWidth(const GfxRenderer& renderer, const int fontId, const char* const word,
                                const EpdFontFamily::Style style) {
  if (word[0] == ' ' && word[1] == '\0') {
    return static_cast<uint16_t>(std::max(0, renderer.getSpaceWidth(fontId, style)));
  }
  return static_cast<uint16_t>(std::max(0, renderer.getTextAdvanceX(fontId, word, style)));
}

void renderTextBlock(const TextBlockView& block, GfxRenderer& renderer, const int fontId, const int x, const int y,
                     const bool foregroundBlack) {
  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (uint16_t index = 0; index < block.wordCount; ++index) {
    const char* const word = block.word(index);
    const uint16_t wordLength = block.wordLength(index);
    const int wordX = x + block.xPosition(index);
    const EpdFontFamily::Style style = block.style(index);
    const uint8_t boundary = block.boundary(index);
    const auto baseDirection = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(word, block.isRtl ? 1 : 0));

    if ((block.flags(index) & WORD_FLAG_BACKGROUND_BLACK) != 0 && isWhitespaceOnlyBackgroundToken(word)) {
      const uint16_t backgroundWidth = measureBackgroundWidth(renderer, fontId, word, style);
      if (backgroundWidth > 0) {
        renderer.fillRect(wordX, y, backgroundWidth, ascender, true);
      }
    }

    int wordY = y;
    if ((style & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((style & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
      char boldBuffer[40]{};
      const size_t boldLength =
          std::min<size_t>({boundary, wordLength, sizeof(boldBuffer) - 1U});
      std::memcpy(boldBuffer, word, boldLength);
      renderer.drawText(fontId, wordX, wordY, boldBuffer, foregroundBlack, boldStyle, baseDirection);
      renderer.drawText(fontId, wordX + block.suffixX(index), wordY, word + boldLength, foregroundBlack, style,
                        baseDirection);
    } else {
      renderer.drawText(fontId, wordX, wordY, word, foregroundBlack, style, baseDirection);
    }

    const uint16_t guideDot = block.dotX(index);
    if (guideDot > 0) {
      renderer.drawText(fontId, wordX + guideDot, wordY, "\xc2\xb7", foregroundBlack, EpdFontFamily::REGULAR,
                        baseDirection);
    }

    if (!scanning && (style & EpdFontFamily::UNDERLINE) != 0) {
      int startX = wordX;
      int width = renderer.getTextWidth(fontId, word, style, baseDirection);
      if (hasSyntheticIndentPrefix(word, wordLength)) {
        startX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", style);
        width = renderer.getTextWidth(fontId, word + 3, style, baseDirection);
      }
      if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        width = (width + 1) / 2;
      }
      const int underlineY = wordY + ascender + 2;
      renderer.drawLine(startX, underlineY, startX + width, underlineY, 3, foregroundBlack);
    }

    if ((style & EpdFontFamily::STRIKETHROUGH) != 0) {
      int startX = wordX;
      int width = renderer.getTextWidth(fontId, word, style, baseDirection);
      if (hasSyntheticIndentPrefix(word, wordLength)) {
        startX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", style);
        width = renderer.getTextWidth(fontId, word + 3, style, baseDirection);
      }
      if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        width = (width + 1) / 2;
      }
      const int strikeY = y + ascender / 2 + 6;
      renderer.drawLine(startX, strikeY, startX + width, strikeY, 3, foregroundBlack);
    }
  }
}

bool preflightTableFragment(MemoryPageCursor& cursor, DecodeBudget& budget) {
  int16_t unusedX = 0;
  int16_t unusedY = 0;
  uint16_t width = 0;
  uint8_t columns = 0;
  uint8_t unusedPadding = 0;
  uint16_t lineHeight = 0;
  uint8_t rows = 0;
  if (!cursor.readI16(&unusedX) || !cursor.readI16(&unusedY) || !cursor.readU16(&width) ||
      !cursor.readU8(&columns) || !cursor.readU8(&unusedPadding) || !cursor.readU16(&lineHeight) ||
      !cursor.readU8(&rows) || rows == 0 || rows > MAX_TABLE_ROWS || columns == 0 ||
      columns > MAX_TABLE_CELLS || width < 2 || lineHeight == 0 || !budget.consumeTable(rows)) {
    return false;
  }
  uint32_t totalHeight = 1;
  for (uint8_t row = 0; row < rows; ++row) {
    uint16_t rowHeight = 0;
    bool headerSeparator = false;
    uint8_t cells = 0;
    if (!cursor.readU16(&rowHeight) || !cursor.readBool(&headerSeparator) || !cursor.readU8(&cells) ||
        cells > MAX_TABLE_CELLS || !budget.consumeCells(cells) ||
        rowHeight > std::numeric_limits<uint16_t>::max() - totalHeight) {
      return false;
    }
    totalHeight += rowHeight;
    for (uint8_t cell = 0; cell < cells; ++cell) {
      bool isHeader = false;
      uint8_t lines = 0;
      if (!cursor.readBool(&isHeader) || !cursor.readU8(&lines) || lines > MAX_TABLE_LINES) {
        return false;
      }
      for (uint8_t line = 0; line < lines; ++line) {
        if (!parseTextBlock(cursor, &budget, nullptr)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool preflightSerializedPage(const uint8_t* const bytes, const size_t size) {
  MemoryPageCursor cursor(bytes, size);
  DecodeBudget budget{};
  uint16_t elements = 0;
  if (bytes == nullptr || !cursor.readU16(&elements) || elements > PdfSleepPageCache::MAX_PAGE_ELEMENTS) {
    return false;
  }
  for (uint16_t element = 0; element < elements; ++element) {
    uint8_t tag = 0;
    if (!cursor.readU8(&tag) || !budget.consumeElement()) {
      return false;
    }
    switch (tag) {
      case TAG_PAGE_LINE: {
        int16_t x = 0;
        int16_t y = 0;
        if (!cursor.readI16(&x) || !cursor.readI16(&y) || !parseTextBlock(cursor, &budget, nullptr)) {
          return false;
        }
        break;
      }
      case TAG_PAGE_IMAGE: {
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
        uint32_t pathLength = 0;
        if (!cursor.readI16(&x) || !cursor.readI16(&y) || !cursor.readU32(&pathLength)) {
          return false;
        }
        if (pathLength == 0 || pathLength >= PDF_CACHE_PATH_CAPACITY || !cursor.skip(pathLength) ||
            !cursor.readI16(&width) || !cursor.readI16(&height)) {
          return false;
        }
        break;
      }
      case TAG_PAGE_TABLE_FRAGMENT:
        if (!preflightTableFragment(cursor, budget)) {
          return false;
        }
        break;
      case TAG_PAGE_HORIZONTAL_RULE: {
        int16_t x = 0;
        int16_t y = 0;
        uint16_t width = 0;
        uint8_t thickness = 0;
        if (!cursor.readI16(&x) || !cursor.readI16(&y) || !cursor.readU16(&width) ||
            !cursor.readU8(&thickness) || width == 0 || thickness == 0) {
          return false;
        }
        break;
      }
      default:
        return false;
    }
  }

  uint16_t footnotes = 0;
  if (!cursor.readU16(&footnotes) || footnotes > MAX_PAGE_FOOTNOTES ||
      !cursor.skip(static_cast<size_t>(footnotes) * FOOTNOTE_BYTES)) {
    return false;
  }
  uint8_t markers = 0;
  return cursor.readU8(&markers) && markers <= MAX_PAGE_MARKERS &&
         cursor.skip(static_cast<size_t>(markers) * PUBLISHER_MARKER_BYTES) && cursor.atEnd();
}

bool scanTableRows(MemoryPageCursor& cursor, const uint8_t rowCount, uint16_t* const totalHeight) {
  uint32_t height = 1;
  for (uint8_t row = 0; row < rowCount; ++row) {
    uint16_t rowHeight = 0;
    bool headerSeparator = false;
    uint8_t cells = 0;
    if (!cursor.readU16(&rowHeight) || !cursor.readBool(&headerSeparator) || !cursor.readU8(&cells) ||
        cells > MAX_TABLE_CELLS || rowHeight > std::numeric_limits<uint16_t>::max() - height) {
      return false;
    }
    height += rowHeight;
    for (uint8_t cell = 0; cell < cells; ++cell) {
      bool isHeader = false;
      uint8_t lines = 0;
      if (!cursor.readBool(&isHeader) || !cursor.readU8(&lines) || lines > MAX_TABLE_LINES) {
        return false;
      }
      for (uint8_t line = 0; line < lines; ++line) {
        if (!parseTextBlock(cursor, nullptr, nullptr)) {
          return false;
        }
      }
    }
  }
  if (totalHeight != nullptr) {
    *totalHeight = static_cast<uint16_t>(height);
  }
  return true;
}

bool renderTableFragment(MemoryPageCursor& cursor, DecodeBudget& budget, GfxRenderer& renderer, const int fontId,
                         const int xOffset, const int yOffset, const bool foregroundBlack) {
  int16_t x = 0;
  int16_t y = 0;
  uint16_t width = 0;
  uint8_t columns = 0;
  uint8_t cellPadding = 0;
  uint16_t lineHeight = 0;
  uint8_t rows = 0;
  if (!cursor.readI16(&x) || !cursor.readI16(&y) || !cursor.readU16(&width) || !cursor.readU8(&columns) ||
      !cursor.readU8(&cellPadding) || !cursor.readU16(&lineHeight) || !cursor.readU8(&rows) || rows == 0 ||
      rows > MAX_TABLE_ROWS || columns == 0 || columns > MAX_TABLE_CELLS || width < 2 || lineHeight == 0 ||
      !budget.consumeTable(rows)) {
    return false;
  }

  MemoryPageCursor measurement = cursor;
  uint16_t totalHeight = 0;
  if (!scanTableRows(measurement, rows, &totalHeight)) {
    return false;
  }

  const int drawX = xOffset + x;
  const int drawY = yOffset + y;
  int16_t columnStarts[MAX_TABLE_CELLS + 1]{};
  for (uint8_t column = 0; column < columns; ++column) {
    columnStarts[column] =
        static_cast<int16_t>((static_cast<uint32_t>(width) * column) / columns);
  }
  columnStarts[columns] = static_cast<int16_t>(width - 1U);
  renderer.drawRect(drawX, drawY, width, totalHeight, foregroundBlack);
  for (uint8_t column = 1; column < columns; ++column) {
    const int lineX = drawX + columnStarts[column];
    renderer.drawLine(lineX, drawY, lineX, drawY + totalHeight - 1, foregroundBlack);
  }

  int currentY = 0;
  for (uint8_t row = 0; row < rows; ++row) {
    uint16_t rowHeight = 0;
    bool headerSeparator = false;
    uint8_t cells = 0;
    if (!cursor.readU16(&rowHeight) || !cursor.readBool(&headerSeparator) || !cursor.readU8(&cells) ||
        cells > MAX_TABLE_CELLS || !budget.consumeCells(cells)) {
      return false;
    }
    for (uint8_t cell = 0; cell < cells; ++cell) {
      bool isHeader = false;
      uint8_t lines = 0;
      if (!cursor.readBool(&isHeader) || !cursor.readU8(&lines) || lines > MAX_TABLE_LINES) {
        return false;
      }
      for (uint8_t line = 0; line < lines; ++line) {
        TextBlockView block{};
        if (!parseTextBlock(cursor, &budget, &block)) {
          return false;
        }
        if (cell < columns) {
          const int textX = drawX + columnStarts[cell] + cellPadding;
          const int textY = drawY + currentY + cellPadding + static_cast<int>(line) * lineHeight;
          renderTextBlock(block, renderer, fontId, textX, textY, foregroundBlack);
        }
      }
    }
    currentY += rowHeight;
    if (row + 1U < rows) {
      renderer.drawLine(drawX, drawY + currentY, drawX + width - 1, drawY + currentY,
                        headerSeparator ? 2 : 1, foregroundBlack);
    }
  }
  return cursor.position() == measurement.position();
}

bool renderSerializedTextPage(const uint8_t* const bytes, const size_t size, GfxRenderer& renderer, const int fontId,
                              const int xOffset, const int yOffset, const bool foregroundBlack) {
  MemoryPageCursor cursor(bytes, size);
  DecodeBudget budget{};
  uint16_t elements = 0;
  if (bytes == nullptr || !cursor.readU16(&elements) || elements > PdfSleepPageCache::MAX_PAGE_ELEMENTS) {
    return false;
  }
  for (uint16_t element = 0; element < elements; ++element) {
    uint8_t tag = 0;
    if (!cursor.readU8(&tag) || !budget.consumeElement()) {
      return false;
    }
    if (tag == TAG_PAGE_LINE) {
      int16_t x = 0;
      int16_t y = 0;
      TextBlockView block{};
      if (!cursor.readI16(&x) || !cursor.readI16(&y) || !parseTextBlock(cursor, &budget, &block)) {
        return false;
      }
      renderTextBlock(block, renderer, fontId, xOffset + x, yOffset + y, foregroundBlack);
    } else if (tag == TAG_PAGE_IMAGE) {
      int16_t x = 0;
      int16_t y = 0;
      int16_t width = 0;
      int16_t height = 0;
      uint32_t pathLength = 0;
      if (!cursor.readI16(&x) || !cursor.readI16(&y) || !cursor.readU32(&pathLength) || pathLength == 0 ||
          pathLength >= PDF_CACHE_PATH_CAPACITY || !cursor.skip(pathLength) || !cursor.readI16(&width) ||
          !cursor.readI16(&height)) {
        return false;
      }
    } else if (tag == TAG_PAGE_TABLE_FRAGMENT) {
      if (!renderTableFragment(cursor, budget, renderer, fontId, xOffset, yOffset, foregroundBlack)) {
        return false;
      }
    } else if (tag == TAG_PAGE_HORIZONTAL_RULE) {
      int16_t x = 0;
      int16_t y = 0;
      uint16_t width = 0;
      uint8_t thickness = 0;
      if (!cursor.readI16(&x) || !cursor.readI16(&y) || !cursor.readU16(&width) ||
          !cursor.readU8(&thickness) || width == 0 || thickness == 0) {
        return false;
      }
      renderer.drawLine(xOffset + x, yOffset + y, xOffset + x + width - 1, yOffset + y, thickness,
                        foregroundBlack);
    } else {
      return false;
    }
  }

  uint16_t footnotes = 0;
  uint8_t markers = 0;
  return cursor.readU16(&footnotes) && footnotes <= MAX_PAGE_FOOTNOTES &&
         cursor.skip(static_cast<size_t>(footnotes) * FOOTNOTE_BYTES) && cursor.readU8(&markers) &&
         markers <= MAX_PAGE_MARKERS && cursor.skip(static_cast<size_t>(markers) * PUBLISHER_MARKER_BYTES) &&
         cursor.atEnd();
}

struct WordIndexSource {
  const PdfCacheIo* io = nullptr;
  PdfCacheHandle handle{};
  uint64_t size = 0;

  static PdfStatus read(void* const context, const uint64_t offset, uint8_t* const destination,
                        const size_t requested, size_t* const bytesRead) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& source = *static_cast<WordIndexSource*>(context);
    return source.io->read(source.io->context, source.handle, offset, destination, requested, bytesRead);
  }

  PdfByteSource source() { return {this, size, read}; }
};

bool closeWordIndex(const PdfCacheIo& io, WordIndexSource& source) {
  PdfStatus status = PdfStatus::success();
  if (source.handle.valid()) {
    status = io.close(io.context, &source.handle);
  }
  source = {};
  return status.ok();
}

bool openWordIndex(const PdfCacheIo& io, const char* const path, WordIndexSource& source) {
  source = {};
  source.io = &io;
  PdfStatus status = io.open(io.context, path, PdfCacheOpenMode::Read, &source.handle);
  PdfCacheFileMetadata metadata{};
  if (status) {
    status = io.metadata(io.context, source.handle, &metadata);
  }
  if (!status || metadata.directory || metadata.symlinkLike) {
    closeWordIndex(io, source);
    return false;
  }
  source.size = metadata.size;
  return true;
}

bool formatSectionPaths(const char* const cacheRoot, const uint16_t section, char* const sectionPath,
                        const size_t sectionCapacity, char* const wordIndexPath, const size_t wordIndexCapacity) {
  if (cacheRoot == nullptr || cacheRoot[0] == '\0') {
    return false;
  }
  const int sectionLength =
      std::snprintf(sectionPath, sectionCapacity, "%s/sections/%u_light.bin", cacheRoot, section);
  if (sectionLength <= 0 || static_cast<size_t>(sectionLength) >= sectionCapacity) {
    return false;
  }
  const int wordIndexLength = std::snprintf(wordIndexPath, wordIndexCapacity, "%s.pwi", sectionPath);
  return wordIndexLength > 0 && static_cast<size_t>(wordIndexLength) < wordIndexCapacity;
}

}  // namespace

struct PdfSleepProductCache::Impl {
  PdfHalCacheIoContext ioContext{};
  PdfCachedProductState state{};
  PdfCachedProductStateLoadResult result{};
  char loadedPath[PDF_CACHE_PATH_CAPACITY]{};
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t loadedCacheHash = 0;
  bool loadedReadOnlyFallback = false;
  bool attempted = false;
};

struct PdfSleepPageCache::Impl {
  PdfHalCacheIoContext ioContext{};
  PdfSleepPageLayout layout{};
  std::unique_ptr<uint8_t[]> pageRecord;
  size_t pageRecordBytes = 0;
  char sectionPath[PDF_CACHE_PATH_CAPACITY]{};
  char wordIndexPath[PDF_CACHE_PATH_CAPACITY]{};

  void releasePageRecord() {
    pageRecord.reset();
    pageRecordBytes = 0;
  }

  bool selectPage(const PdfSleepProductCache& product, uint16_t* const pageNumber,
                  uint16_t* const sidecarPageCount) {
    if (pageNumber == nullptr || sidecarPageCount == nullptr ||
        !formatSectionPaths(product.cacheRoot(), product.currentSection(), sectionPath, sizeof(sectionPath),
                            wordIndexPath, sizeof(wordIndexPath))) {
      LOG_ERR("SLP", "PDF sleep layout path exceeds fixed capacity");
      return false;
    }
    const uint32_t totalWords = product.totalWords();
    const uint32_t currentWord = product.currentWord();
    if (totalWords == 0 || currentWord > totalWords) {
      LOG_ERR("SLP", "PDF sleep progress is unavailable or out of range");
      return false;
    }

    const PdfCacheIo io = pdfHalCacheIo(ioContext);
    if (!io.valid()) {
      LOG_ERR("SLP", "PDF sleep sidecar I/O is unavailable");
      return false;
    }
    WordIndexSource source;
    if (!openWordIndex(io, wordIndexPath, source)) {
      LOG_DBG("SLP", "PDF sleep word index is unavailable");
      return false;
    }

    PdfLayoutWordIndexInfo info{};
    PdfStatus status = pdfInspectLayoutWordIndex(source.source(), &info);
    if (status && (info.sectionIndex != product.currentSection() ||
                   info.firstGlobalWordOrdinal != product.currentSectionFirstWordOrdinal() ||
                   info.sectionWordCount != product.currentSectionWordCount() || info.pageCount == 0 ||
                   info.pageCount > MAX_LAYOUT_PAGES)) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
    if (status) {
      status = pdfFindLayoutCursor(source.source(), currentWord, pageNumber);
    }
    const bool closed = closeWordIndex(io, source);
    if (!status || !closed) {
      LOG_DBG("SLP", "PDF sleep word index is missing, stale, or corrupt");
      return false;
    }
    *sidecarPageCount = info.pageCount;
    return true;
  }

  bool loadPage(const uint16_t pageNumber, const uint16_t sidecarPageCount) {
    releasePageRecord();
    FsFile file;
    if (!Storage.openFileForRead("SLP", sectionPath, file)) {
      LOG_DBG("SLP", "PDF sleep section layout is unavailable");
      return false;
    }

    uint8_t header[SECTION_HEADER_BYTES]{};
    const uint64_t fileSize64 = file.fileSize64();
    bool valid = fileSize64 >= SECTION_HEADER_BYTES && fileSize64 <= std::numeric_limits<uint32_t>::max() &&
                 readExact(file, header, sizeof(header));
    const uint32_t fileSize = valid ? static_cast<uint32_t>(fileSize64) : 0;
    const uint16_t pageCount = valid ? readLe16(header + 26) : 0;
    const uint32_t lutOffset = valid ? readLe32(header + 28) : 0;
    const uint32_t anchorMapOffset = valid ? readLe32(header + 32) : 0;
    const uint32_t paragraphLutOffset = valid ? readLe32(header + 36) : 0;
    const uint32_t liLutOffset = valid ? readLe32(header + 40) : 0;
    const int fileFontId = valid ? static_cast<int32_t>(readLe32(header + 5)) : 0;
    const uint64_t lutEnd =
        static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(pageCount) * PAGE_LUT_ENTRY_BYTES;

    valid = valid && readLe32(header) == SECTION_CACHE_MAGIC && header[4] == SECTION_FILE_VERSION &&
            readLe16(header + 16) == layout.viewportWidth && readLe16(header + 18) == layout.viewportHeight &&
            header[25] == static_cast<uint8_t>(EpubRenderMode::Light) && pageCount == sidecarPageCount &&
            pageNumber < pageCount && lutOffset >= SECTION_HEADER_BYTES && lutEnd <= anchorMapOffset &&
            anchorMapOffset <= paragraphLutOffset && paragraphLutOffset <= liLutOffset && liLutOffset <= fileSize;

    uint32_t pagePosition = 0;
    uint32_t nextPagePosition = lutOffset;
    if (valid) {
      valid = readU32At(file, lutOffset + static_cast<uint32_t>(pageNumber) * PAGE_LUT_ENTRY_BYTES, &pagePosition);
    }
    if (valid && pageNumber + 1U < pageCount) {
      valid = readU32At(file, lutOffset + static_cast<uint32_t>(pageNumber + 1U) * PAGE_LUT_ENTRY_BYTES,
                        &nextPagePosition);
    }
    const size_t serializedBytes =
        nextPagePosition >= pagePosition ? static_cast<size_t>(nextPagePosition - pagePosition)
                                         : std::numeric_limits<size_t>::max();
    valid = valid && pagePosition >= SECTION_HEADER_BYTES && pagePosition < nextPagePosition &&
            nextPagePosition <= lutOffset && serializedBytes <= MAX_SERIALIZED_PAGE_BYTES &&
            serializedBytes <= static_cast<size_t>(std::numeric_limits<int>::max());

    if (valid) {
      pageRecord = makeUniqueNoThrow<uint8_t[]>(serializedBytes);
      valid = pageRecord != nullptr && file.seek(pagePosition) &&
              readExact(file, pageRecord.get(), serializedBytes);
    }
    const bool closed = file.close();
    valid = valid && closed && preflightSerializedPage(pageRecord.get(), serializedBytes);
    if (!valid || !closed) {
      releasePageRecord();
      LOG_DBG("SLP", "PDF sleep section layout is stale, corrupt, oversized, or out of memory");
      return false;
    }
    // The section header records the actual font used for this page, including
    // the reader's bounded low-memory fallback font.
    layout.fontId = fileFontId;
    pageRecordBytes = serializedBytes;
    return true;
  }
};

PdfSleepPageCache::PdfSleepPageCache() = default;

PdfSleepPageCache::~PdfSleepPageCache() = default;

PdfSleepProductCache::PdfSleepProductCache() = default;

PdfSleepProductCache::~PdfSleepProductCache() = default;

bool PdfSleepProductCache::load(const std::string& sourcePath) {
  if (impl_ == nullptr) {
    // Fixed-capacity product state is too large for the activity task stack.
    impl_ = makeUniqueNoThrow<Impl>();
    if (impl_ == nullptr) {
      LOG_ERR("SLP", "PDF sleep product allocation failed");
      return false;
    }
  }
  if (sourcePath.empty() || sourcePath.size() >= sizeof(impl_->loadedPath)) {
    LOG_ERR("SLP", "PDF sleep source path exceeds fixed capacity");
    impl_->state = {};
    impl_->result = {};
    impl_->attempted = true;
    impl_->loadedPath[0] = '\0';
    impl_->cacheRoot[0] = '\0';
    return false;
  }

  const uint64_t normalCacheHash = pdfPathHash64(sourcePath.c_str(), sourcePath.size());
  uint64_t resolvedCacheHash = normalCacheHash;
  bool readOnlyFallback = true;
  const bool resolved =
      BookMoveUtils::migrationCacheHash(sourcePath, normalCacheHash, &resolvedCacheHash, &readOnlyFallback);
  if (impl_->attempted && sourcePath == impl_->loadedPath && resolved && impl_->loadedCacheHash == resolvedCacheHash &&
      impl_->loadedReadOnlyFallback == readOnlyFallback) {
    return impl_->result.available();
  }

  std::memcpy(impl_->loadedPath, sourcePath.c_str(), sourcePath.size() + 1U);
  impl_->loadedCacheHash = resolvedCacheHash;
  impl_->loadedReadOnlyFallback = readOnlyFallback;
  impl_->attempted = true;
  impl_->state = {};
  impl_->result = {};
  if (!resolved ||
      !pdfFormatCacheRootForHash(PDF_CACHE_DIRECTORY, resolvedCacheHash, impl_->cacheRoot, sizeof(impl_->cacheRoot))) {
    impl_->cacheRoot[0] = '\0';
    LOG_ERR("SLP", "PDF sleep cache identity resolution failed");
    return false;
  }

  const uint64_t* const cacheHashOverride = resolvedCacheHash == normalCacheHash ? nullptr : &resolvedCacheHash;
  impl_->result =
      pdfLoadCachedProductStateForSleep(pdfHalCacheIo(impl_->ioContext), sourcePath.c_str(), PDF_CACHE_DIRECTORY,
                                        &impl_->state, cacheHashOverride);
  return impl_->result.available();
}

bool PdfSleepProductCache::available() const { return impl_ != nullptr && impl_->result.available(); }

void PdfSleepProductCache::reset() { impl_.reset(); }

const char* PdfSleepProductCache::title() const { return available() ? impl_->state.title : ""; }

const char* PdfSleepProductCache::author() const { return available() ? impl_->state.author : ""; }

const char* PdfSleepProductCache::chapter() const { return available() ? impl_->state.currentChapter : ""; }

const char* PdfSleepProductCache::coverPath() const { return available() ? impl_->state.coverPath : ""; }

const char* PdfSleepProductCache::thumbnailPath() const { return available() ? impl_->state.thumbnailPath : ""; }

const char* PdfSleepProductCache::cacheRoot() const { return available() ? impl_->cacheRoot : ""; }

uint16_t PdfSleepProductCache::currentSection() const { return available() ? impl_->state.currentSection : 0; }

uint32_t PdfSleepProductCache::currentWord() const { return available() ? impl_->state.currentWord : 0; }

uint32_t PdfSleepProductCache::totalWords() const { return available() ? impl_->state.totalWords : 0; }

uint32_t PdfSleepProductCache::currentSectionFirstWordOrdinal() const {
  return available() ? impl_->state.currentSectionFirstWordOrdinal : 0;
}

uint32_t PdfSleepProductCache::currentSectionWordCount() const {
  return available() ? impl_->state.currentSectionWordCount : 0;
}

float PdfSleepProductCache::progressPercent() const {
  if (!available() || !impl_->state.hasProgress || impl_->state.totalWords == 0) {
    return 0.0f;
  }
  const float percent =
      static_cast<float>(impl_->state.currentWord) / static_cast<float>(impl_->state.totalWords) * 100.0f;
  return std::clamp(percent, 0.0f, 100.0f);
}

bool pdfSnapshotBeforeFallback(GfxRenderer& renderer, const PdfSleepFallback fallback) {
  if (renderer.storeBwBuffer()) {
    return true;
  }
  if (fallback.load != nullptr) {
    fallback.load(fallback.context);
  }
  return false;
}

bool PdfSleepPageCache::load(const PdfSleepProductCache& product, const PdfSleepPageLayout& layout) {
  if (!product.available() || !layout.valid || layout.viewportWidth == 0 || layout.viewportHeight == 0) {
    return false;
  }
  if (impl_ == nullptr) {
    impl_ = makeUniqueNoThrow<Impl>();
    if (impl_ == nullptr) {
      LOG_ERR("SLP", "PDF sleep page cache allocation failed");
      return false;
    }
  }
  impl_->releasePageRecord();
  impl_->layout = layout;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  return impl_->selectPage(product, &pageNumber, &pageCount) && impl_->loadPage(pageNumber, pageCount);
}

bool PdfSleepPageCache::available() const {
  return impl_ != nullptr && impl_->pageRecord != nullptr && impl_->pageRecordBytes > 0;
}

bool PdfSleepPageCache::renderTextAndRelease(GfxRenderer& renderer) {
  if (!available()) {
    return false;
  }
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(impl_->layout.orientation));
  renderer.clearScreen(impl_->layout.backgroundColor);
  const bool rendered =
      renderSerializedTextPage(impl_->pageRecord.get(), impl_->pageRecordBytes, renderer, impl_->layout.fontId,
                               impl_->layout.marginLeft, impl_->layout.marginTop, impl_->layout.foregroundBlack);
  impl_->releasePageRecord();
  return rendered;
}

void PdfSleepPageCache::reset() { impl_.reset(); }

#endif  // defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
