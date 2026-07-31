#include "PdfOutline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "PdfCacheFormat.h"
#include "PdfIo.h"
#include "PdfSemanticWriter.h"
#include "PdfUnicode.h"

namespace {

constexpr uint8_t kMagic[] = {'X', 'P', 'O', 'L'};
constexpr size_t kHeaderBytes = 16;
constexpr size_t kCrcBytes = 4;

void putU16(uint8_t* output, const uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, const uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) | (static_cast<uint32_t>(input[3]) << 24U);
}

PdfStatus copyBoundedUtf8(const uint8_t* const source, const size_t length, char* const output, const size_t capacity,
                          uint8_t* const outputLength) {
  if ((source == nullptr && length != 0) || output == nullptr || outputLength == nullptr || capacity < 2) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t offset = 0;
  size_t accepted = 0;
  while (offset < length) {
    uint32_t scalar = 0;
    const PdfStatus status = pdfDecodeUtf8Scalar(source, length, &offset, &scalar);
    if (!status) {
      return status;
    }
    (void)scalar;
    if (offset < capacity) {
      accepted = offset;
    }
  }
  if (accepted == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  std::memcpy(output, source, accepted);
  output[accepted] = '\0';
  *outputLength = static_cast<uint8_t>(accepted);
  return PdfStatus::success();
}

bool validEntry(const PdfOutlineEntry& entry, const uint16_t index, const uint8_t parentLevel) {
  if (entry.titleLength == 0 || entry.titleLength >= PdfOutlineLimits::TitleBytes ||
      entry.title[entry.titleLength] != '\0' || entry.level == 0 || entry.level > PdfOutlineLimits::MaxDepth ||
      entry.reserved != 0 || entry.parentIndex >= static_cast<int16_t>(index) || entry.parentIndex < -1) {
    return false;
  }
  if (entry.parentIndex == -1) {
    return entry.level == 1;
  }
  return parentLevel != 0 && parentLevel + 1 == entry.level;
}

PdfStatus validateUtf8Title(const PdfOutlineEntry& entry) {
  size_t offset = 0;
  while (offset < entry.titleLength) {
    uint32_t scalar = 0;
    const PdfStatus status =
        pdfDecodeUtf8Scalar(reinterpret_cast<const uint8_t*>(entry.title), entry.titleLength, &offset, &scalar);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

void encodeEntry(const PdfOutlineEntry& entry, uint8_t output[PdfOutlineLimits::EncodedRecordBytes]) {
  std::memset(output, 0, PdfOutlineLimits::EncodedRecordBytes);
  putU32(output, entry.sourceReference.objectNumber);
  putU16(output + 4, entry.sourceReference.generation);
  putU16(output + 6, static_cast<uint16_t>(entry.parentIndex));
  putU16(output + 8, entry.sectionIndex);
  output[10] = entry.level;
  output[11] = entry.titleLength;
  putU32(output + 12, entry.anchorOrdinal);
  putU32(output + 16, entry.sourcePageIndex);
  putU16(output + 20, entry.reserved);
  putU16(output + 22, 0);
  std::memcpy(output + 24, entry.title, entry.titleLength);
}

PdfOutlineEntry decodeEntry(const uint8_t input[PdfOutlineLimits::EncodedRecordBytes]) {
  PdfOutlineEntry entry{};
  entry.sourceReference.objectNumber = getU32(input);
  entry.sourceReference.generation = getU16(input + 4);
  entry.parentIndex = static_cast<int16_t>(getU16(input + 6));
  entry.sectionIndex = getU16(input + 8);
  entry.level = input[10];
  entry.titleLength = input[11];
  entry.anchorOrdinal = getU32(input + 12);
  entry.sourcePageIndex = getU32(input + 16);
  entry.reserved = getU16(input + 20);
  if (entry.titleLength < PdfOutlineLimits::TitleBytes) {
    std::memcpy(entry.title, input + 24, entry.titleLength);
    entry.title[entry.titleLength] = '\0';
  }
  (void)pdfFormatSemanticAnchor(entry.anchorOrdinal, entry.anchor);
  return entry;
}

PdfStatus appendNumber(char* output, const size_t capacity, size_t* length, const char* value) {
  const size_t count = std::strlen(value);
  if (*length > capacity || count >= capacity - *length) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  std::memcpy(output + *length, value, count);
  *length += count;
  output[*length] = '\0';
  return PdfStatus::success();
}

PdfStatus formatRoman(uint32_t value, const bool uppercase, char* output, const size_t capacity, size_t* length) {
  if (value == 0 || value > 3999) {
    return PdfStatus::failure(PdfError::LimitExceeded, value);
  }
  struct Roman {
    uint16_t value;
    const char* digits;
  };
  static constexpr Roman numerals[] = {
      {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
      {40, "XL"},  {10, "X"},   {9, "IX"},  {5, "V"},    {4, "IV"},  {1, "I"},
  };
  for (const Roman& numeral : numerals) {
    while (value >= numeral.value) {
      const size_t begin = *length;
      const PdfStatus status = appendNumber(output, capacity, length, numeral.digits);
      if (!status) {
        return status;
      }
      if (!uppercase) {
        for (size_t index = begin; index < *length; ++index) {
          output[index] = static_cast<char>(output[index] - 'A' + 'a');
        }
      }
      value -= numeral.value;
    }
  }
  return PdfStatus::success();
}

PdfStatus formatAlpha(uint32_t value, const bool uppercase, char* output, const size_t capacity, size_t* length) {
  if (value == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  char reversed[16]{};
  size_t count = 0;
  while (value != 0 && count < sizeof(reversed)) {
    --value;
    reversed[count++] = static_cast<char>((uppercase ? 'A' : 'a') + (value % 26U));
    value /= 26U;
  }
  if (value != 0 || count >= capacity - *length) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  while (count != 0) {
    output[(*length)++] = reversed[--count];
  }
  output[*length] = '\0';
  return PdfStatus::success();
}

const PdfValue* valueAt(const PdfObjectArena& arena, const uint16_t index) {
  return index < arena.valueCount ? &arena.values[index] : nullptr;
}

PdfStatus copyArenaText(const PdfObjectArena& arena, const PdfValue& value, char* const output, const size_t capacity,
                        uint8_t* const outputLength) {
  if ((value.kind != PdfValueKind::Name && value.kind != PdfValueKind::String) || value.textOffset > arena.textLength ||
      value.textLength > arena.textLength - value.textOffset) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return copyBoundedUtf8(arena.text + value.textOffset, value.textLength, output, capacity, outputLength);
}

bool referenceForKey(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
                     PdfObjectReference* const reference) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (reference == nullptr || !pdfDictionaryFind(arena, dictionaryIndex, key, &valueIndex)) {
    return false;
  }
  const PdfValue* const value = valueAt(arena, valueIndex);
  if (value == nullptr || value->kind != PdfValueKind::Reference) {
    return false;
  }
  *reference = {value->objectNumber, value->generation};
  return true;
}

PdfStatus parseRawDestination(const PdfObjectArena& arena, const uint16_t valueIndex, PdfRawDestination* destination,
                              const uint8_t depth = 0) {
  if (destination == nullptr || depth > 2) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfValue* const value = valueAt(arena, valueIndex);
  if (value == nullptr) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *destination = {};
  if (value->kind == PdfValueKind::Name || value->kind == PdfValueKind::String) {
    destination->kind = PdfRawDestinationKind::Named;
    const PdfStatus status =
        copyArenaText(arena, *value, destination->name, sizeof(destination->name), &destination->nameLength);
    if (!status) {
      *destination = {};
    }
    return status;
  }
  if (value->kind == PdfValueKind::Array) {
    uint16_t firstIndex = PDF_INVALID_INDEX;
    if (value->count == 0 || !pdfArrayAt(arena, valueIndex, 0, &firstIndex)) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const PdfValue* const first = valueAt(arena, firstIndex);
    if (first == nullptr || first->kind != PdfValueKind::Reference) {
      return PdfStatus::failure(PdfError::Unsupported);
    }
    destination->kind = PdfRawDestinationKind::Explicit;
    destination->pageReference = {first->objectNumber, first->generation};
    return PdfStatus::success();
  }
  if (value->kind == PdfValueKind::Dictionary) {
    uint16_t destinationIndex = PDF_INVALID_INDEX;
    if (!pdfDictionaryFind(arena, valueIndex, "D", &destinationIndex)) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    return parseRawDestination(arena, destinationIndex, destination, static_cast<uint8_t>(depth + 1));
  }
  return PdfStatus::failure(PdfError::Unsupported);
}

PdfActionKind actionKind(const PdfObjectArena& arena, const PdfValue& value) {
  if (value.kind != PdfValueKind::Name) {
    return PdfActionKind::RemoteGoTo;
  }
  if (pdfTextEquals(arena, value, "GoTo")) {
    return PdfActionKind::GoTo;
  }
  if (pdfTextEquals(arena, value, "URI")) {
    return PdfActionKind::Uri;
  }
  if (pdfTextEquals(arena, value, "Launch")) {
    return PdfActionKind::Launch;
  }
  if (pdfTextEquals(arena, value, "JavaScript")) {
    return PdfActionKind::JavaScript;
  }
  if (pdfTextEquals(arena, value, "GoToR")) {
    return PdfActionKind::RemoteGoTo;
  }
  return PdfActionKind::Attachment;
}

PdfStatus parseActionDictionary(const PdfObjectArena& arena, const uint16_t dictionaryIndex, PdfActionKind* action,
                                PdfRawDestination* destination) {
  if (action == nullptr || destination == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint16_t actionIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, "S", &actionIndex)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue* const actionValue = valueAt(arena, actionIndex);
  if (actionValue == nullptr) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *action = actionKind(arena, *actionValue);
  *destination = {};
  if (*action != PdfActionKind::GoTo) {
    return PdfStatus::success();
  }
  uint16_t destinationIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, "D", &destinationIndex)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return parseRawDestination(arena, destinationIndex, destination);
}

PdfStatus readActionOrDestination(const PdfObjectArena& arena, const uint16_t dictionaryIndex, PdfActionKind* action,
                                  PdfRawDestination* destination) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (pdfDictionaryFind(arena, dictionaryIndex, "Dest", &valueIndex)) {
    *action = PdfActionKind::GoTo;
    return parseRawDestination(arena, valueIndex, destination);
  }
  if (!pdfDictionaryFind(arena, dictionaryIndex, "A", &valueIndex)) {
    *action = PdfActionKind::GoTo;
    *destination = {};
    return PdfStatus::success();
  }
  const PdfValue* const actionValue = valueAt(arena, valueIndex);
  if (actionValue == nullptr || actionValue->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  return parseActionDictionary(arena, valueIndex, action, destination);
}

PdfStatus fixedCoordinate(const PdfValue& value, int32_t* const coordinate) {
  if (coordinate == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (value.kind == PdfValueKind::Real) {
    *coordinate = value.fixedValue;
    return PdfStatus::success();
  }
  if (value.kind != PdfValueKind::Integer || value.integerValue < (std::numeric_limits<int32_t>::min() >> 16) ||
      value.integerValue > (std::numeric_limits<int32_t>::max() >> 16)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *coordinate = static_cast<int32_t>(static_cast<int64_t>(value.integerValue) * 65536LL);
  return PdfStatus::success();
}

const uint8_t* findBytes(const uint8_t* const source, const size_t length, const char* const needle,
                         const uint8_t* const start = nullptr) {
  const size_t needleLength = std::strlen(needle);
  if (source == nullptr || needleLength == 0 || needleLength > length) {
    return nullptr;
  }
  const uint8_t* cursor = start == nullptr ? source : start;
  if (cursor < source || cursor > source + length) {
    return nullptr;
  }
  const uint8_t* const end = source + length;
  while (static_cast<size_t>(end - cursor) >= needleLength) {
    if (std::memcmp(cursor, needle, needleLength) == 0) {
      return cursor;
    }
    ++cursor;
  }
  return nullptr;
}

PdfStatus xmpElementText(const uint8_t* const source, const size_t length, const char* const container,
                         const char* const item, const uint8_t** const text, size_t* const textLength) {
  const uint8_t* const containerStart = findBytes(source, length, container);
  if (containerStart == nullptr) {
    *text = nullptr;
    *textLength = 0;
    return PdfStatus::success();
  }
  const uint8_t* const itemStart = findBytes(source, length, item, containerStart);
  if (itemStart == nullptr) {
    *text = nullptr;
    *textLength = 0;
    return PdfStatus::success();
  }
  const uint8_t* const sourceEnd = source + length;
  const uint8_t* valueStart = itemStart;
  while (valueStart < sourceEnd && *valueStart != '>') {
    ++valueStart;
  }
  if (valueStart == sourceEnd) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  ++valueStart;
  char closing[48]{};
  const int closingLength = std::snprintf(closing, sizeof(closing), "</%s", item + 1);
  if (closingLength <= 0 || static_cast<size_t>(closingLength) >= sizeof(closing)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  const uint8_t* const valueEnd = findBytes(source, length, closing, valueStart);
  if (valueEnd == nullptr || valueEnd < valueStart) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *text = valueStart;
  *textLength = static_cast<size_t>(valueEnd - valueStart);
  return PdfStatus::success();
}

}  // namespace

PdfStatus pdfReadCatalogNavigation(const PdfObjectArena& arena, const uint16_t rootIndex,
                                   PdfCatalogNavigation* const catalog) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (catalog == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *catalog = {};
  catalog->hasPages = referenceForKey(arena, rootIndex, "Pages", &catalog->pages);
  if (!catalog->hasPages) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  catalog->hasOutlines = referenceForKey(arena, rootIndex, "Outlines", &catalog->outlines);
  catalog->hasPageLabels = referenceForKey(arena, rootIndex, "PageLabels", &catalog->pageLabels);
  catalog->hasMetadata = referenceForKey(arena, rootIndex, "Metadata", &catalog->metadata);

  uint16_t namesIndex = PDF_INVALID_INDEX;
  if (pdfDictionaryFind(arena, rootIndex, "Names", &namesIndex)) {
    const PdfValue* const names = valueAt(arena, namesIndex);
    if (names != nullptr && names->kind == PdfValueKind::Dictionary) {
      catalog->hasNamedDestinations = referenceForKey(arena, namesIndex, "Dests", &catalog->namedDestinations);
    } else if (names != nullptr && names->kind == PdfValueKind::Reference) {
      catalog->namedDestinations = {names->objectNumber, names->generation};
      catalog->hasNamedDestinations = true;
    }
  }
  if (!catalog->hasNamedDestinations) {
    catalog->hasNamedDestinations = referenceForKey(arena, rootIndex, "Dests", &catalog->namedDestinations);
  }

  uint16_t languageIndex = PDF_INVALID_INDEX;
  if (pdfDictionaryFind(arena, rootIndex, "Lang", &languageIndex)) {
    const PdfValue* const language = valueAt(arena, languageIndex);
    if (language == nullptr) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const PdfStatus status =
        copyArenaText(arena, *language, catalog->language, sizeof(catalog->language), &catalog->languageLength);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus pdfReadOutlineRoot(const PdfObjectArena& arena, const uint16_t rootIndex, PdfObjectReference* const first) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (first == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary ||
      !referenceForKey(arena, rootIndex, "First", first)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

PdfStatus pdfReadOutlineNode(const PdfObjectArena& arena, const uint16_t rootIndex, PdfRawOutlineNode* const node) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (node == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *node = {};
  uint16_t titleIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, rootIndex, "Title", &titleIndex)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue* const title = valueAt(arena, titleIndex);
  if (title == nullptr) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PdfStatus status = copyArenaText(arena, *title, node->title, sizeof(node->title), &node->titleLength);
  if (!status) {
    return status;
  }
  node->hasFirstChild = referenceForKey(arena, rootIndex, "First", &node->firstChild);
  node->hasNext = referenceForKey(arena, rootIndex, "Next", &node->next);
  PdfActionKind ignoredAction = PdfActionKind::GoTo;
  status = readActionOrDestination(arena, rootIndex, &ignoredAction, &node->destination);
  if (!status && status.error == PdfError::Unsupported) {
    node->destination = {};
    return PdfStatus::success();
  }
  return status;
}

PdfStatus PdfNamedDestinationMap::begin() {
  if (workspace_.records == nullptr || workspace_.capacity == 0 ||
      workspace_.capacity > PdfOutlineLimits::MaxNamedDestinations) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  count_ = 0;
  initialized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfNamedDestinationMap::add(const uint8_t* const name, const size_t nameLength,
                                      const PdfRawDestination& destination) {
  if (!initialized_ || count_ >= workspace_.capacity || destination.kind != PdfRawDestinationKind::Explicit) {
    return count_ >= workspace_.capacity ? PdfStatus::failure(PdfError::LimitExceeded)
                                         : PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < count_; ++index) {
    if (workspace_.records[index].nameLength == nameLength &&
        std::memcmp(workspace_.records[index].name, name, nameLength) == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
  }
  PdfNamedDestinationRecord record{};
  PdfStatus status = copyBoundedUtf8(name, nameLength, record.name, sizeof(record.name), &record.nameLength);
  if (status) {
    record.destination = destination;
    workspace_.records[count_++] = record;
  }
  return status;
}

PdfStatus PdfNamedDestinationMap::resolve(const uint8_t* const name, const size_t nameLength,
                                          PdfRawDestination* const destination) const {
  if (!initialized_ || (name == nullptr && nameLength != 0) || destination == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < count_; ++index) {
    if (workspace_.records[index].nameLength == nameLength &&
        std::memcmp(workspace_.records[index].name, name, nameLength) == 0) {
      *destination = workspace_.records[index].destination;
      return PdfStatus::success();
    }
  }
  *destination = {};
  return PdfStatus::failure(PdfError::InvalidOffset);
}

PdfStatus pdfReadNamedDestinations(const PdfObjectArena& arena, const uint16_t rootIndex,
                                   PdfNamedDestinationMap* const destinations) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (destinations == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  uint16_t namesIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, rootIndex, "Names", &namesIndex)) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  const PdfValue* const names = valueAt(arena, namesIndex);
  if (names == nullptr || names->kind != PdfValueKind::Array || names->count == 0 || (names->count & 1U) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  for (uint16_t ordinal = 0; ordinal < names->count; ordinal += 2) {
    uint16_t nameIndex = PDF_INVALID_INDEX;
    uint16_t destinationIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, namesIndex, ordinal, &nameIndex) ||
        !pdfArrayAt(arena, namesIndex, static_cast<uint16_t>(ordinal + 1), &destinationIndex)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfValue* const name = valueAt(arena, nameIndex);
    if (name == nullptr || (name->kind != PdfValueKind::Name && name->kind != PdfValueKind::String) ||
        name->textOffset > arena.textLength || name->textLength > arena.textLength - name->textOffset) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    PdfRawDestination destination{};
    PdfStatus status = parseRawDestination(arena, destinationIndex, &destination);
    if (status) {
      status = destinations->add(arena.text + name->textOffset, name->textLength, destination);
    }
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus pdfReadPageLabels(const PdfObjectArena& arena, const uint16_t rootIndex, PdfPageLabelMap* const labels) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (labels == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  uint16_t numbersIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, rootIndex, "Nums", &numbersIndex)) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  const PdfValue* const numbers = valueAt(arena, numbersIndex);
  if (numbers == nullptr || numbers->kind != PdfValueKind::Array || numbers->count == 0 || (numbers->count & 1U) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  for (uint16_t ordinal = 0; ordinal < numbers->count; ordinal += 2) {
    uint16_t pageIndex = PDF_INVALID_INDEX;
    uint16_t labelIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, numbersIndex, ordinal, &pageIndex) ||
        !pdfArrayAt(arena, numbersIndex, static_cast<uint16_t>(ordinal + 1), &labelIndex)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfValue* const page = valueAt(arena, pageIndex);
    const PdfValue* const label = valueAt(arena, labelIndex);
    if (page == nullptr || page->kind != PdfValueKind::Integer || page->integerValue < 0 ||
        page->integerValue > UINT32_MAX || label == nullptr || label->kind != PdfValueKind::Dictionary) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    PdfPageLabelRange range{};
    range.firstPageIndex = static_cast<uint32_t>(page->integerValue);
    range.startNumber = 1;
    range.style = PdfPageLabelStyle::None;
    uint16_t valueIndex = PDF_INVALID_INDEX;
    if (pdfDictionaryFind(arena, labelIndex, "S", &valueIndex)) {
      const PdfValue* const style = valueAt(arena, valueIndex);
      if (style == nullptr || style->kind != PdfValueKind::Name) {
        return PdfStatus::failure(PdfError::Malformed, ordinal);
      }
      if (pdfTextEquals(arena, *style, "D")) {
        range.style = PdfPageLabelStyle::Decimal;
      } else if (pdfTextEquals(arena, *style, "R")) {
        range.style = PdfPageLabelStyle::UpperRoman;
      } else if (pdfTextEquals(arena, *style, "r")) {
        range.style = PdfPageLabelStyle::LowerRoman;
      } else if (pdfTextEquals(arena, *style, "A")) {
        range.style = PdfPageLabelStyle::UpperAlpha;
      } else if (pdfTextEquals(arena, *style, "a")) {
        range.style = PdfPageLabelStyle::LowerAlpha;
      } else {
        return PdfStatus::failure(PdfError::Unsupported);
      }
    }
    if (pdfDictionaryFind(arena, labelIndex, "St", &valueIndex)) {
      const PdfValue* const start = valueAt(arena, valueIndex);
      if (start == nullptr || start->kind != PdfValueKind::Integer || start->integerValue <= 0 ||
          start->integerValue > UINT32_MAX) {
        return PdfStatus::failure(PdfError::Malformed, ordinal);
      }
      range.startNumber = static_cast<uint32_t>(start->integerValue);
    }
    if (pdfDictionaryFind(arena, labelIndex, "P", &valueIndex)) {
      const PdfValue* const prefix = valueAt(arena, valueIndex);
      if (prefix == nullptr) {
        return PdfStatus::failure(PdfError::Malformed, ordinal);
      }
      PdfStatus status = copyArenaText(arena, *prefix, range.prefix, sizeof(range.prefix), &range.prefixLength);
      if (!status) {
        return status;
      }
    }
    const PdfStatus status = labels->add(range);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus pdfReadLinkAnnotation(const PdfObjectArena& arena, const uint16_t rootIndex,
                                PdfRawLinkAnnotation* const annotation) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (annotation == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  uint16_t subtypeIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, rootIndex, "Subtype", &subtypeIndex)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue* const subtype = valueAt(arena, subtypeIndex);
  if (subtype == nullptr || !pdfTextEquals(arena, *subtype, "Link")) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  *annotation = {};
  PdfStatus status = readActionOrDestination(arena, rootIndex, &annotation->action, &annotation->destination);
  if (!status) {
    return status;
  }
  uint16_t rectangleIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, rootIndex, "Rect", &rectangleIndex)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue* const rectangle = valueAt(arena, rectangleIndex);
  if (rectangle == nullptr || rectangle->kind != PdfValueKind::Array || rectangle->count != 4) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  int32_t* coordinates[] = {&annotation->rectangle.xMin, &annotation->rectangle.yMin, &annotation->rectangle.xMax,
                            &annotation->rectangle.yMax};
  for (uint16_t ordinal = 0; ordinal < 4; ++ordinal) {
    uint16_t coordinateIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, rectangleIndex, ordinal, &coordinateIndex)) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const PdfValue* const coordinate = valueAt(arena, coordinateIndex);
    if (coordinate == nullptr) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    status = fixedCoordinate(*coordinate, coordinates[ordinal]);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus pdfApplyCatalogMetadata(const PdfCatalogNavigation& catalog, PdfMetadataBuilder* const metadata) {
  if (metadata == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return catalog.languageLength == 0
             ? PdfStatus::success()
             : metadata->setLanguage(PdfMetadataOrigin::Catalog, reinterpret_cast<const uint8_t*>(catalog.language),
                                     catalog.languageLength);
}

PdfStatus pdfApplyInfoMetadata(const PdfObjectArena& arena, const uint16_t rootIndex,
                               PdfMetadataBuilder* const metadata) {
  const PdfValue* const root = valueAt(arena, rootIndex);
  if (metadata == nullptr || root == nullptr || root->kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  for (const char* const key : {"Title", "Author"}) {
    uint16_t valueIndex = PDF_INVALID_INDEX;
    if (!pdfDictionaryFind(arena, rootIndex, key, &valueIndex)) {
      continue;
    }
    const PdfValue* const value = valueAt(arena, valueIndex);
    if (value == nullptr || value->kind != PdfValueKind::String || value->textOffset > arena.textLength ||
        value->textLength > arena.textLength - value->textOffset) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const PdfStatus status =
        std::strcmp(key, "Title") == 0
            ? metadata->setTitle(PdfMetadataOrigin::Info, arena.text + value->textOffset, value->textLength)
            : metadata->setAuthor(PdfMetadataOrigin::Info, arena.text + value->textOffset, value->textLength);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus pdfApplyXmpMetadata(const uint8_t* const source, const size_t length, PdfMetadataBuilder* const metadata) {
  if ((source == nullptr && length != 0) || metadata == nullptr || length > 64U * 1024U) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  struct Element {
    const char* container;
    const char* item;
    uint8_t field;
  };
  static constexpr Element elements[] = {
      {"<dc:title", "<rdf:li", 0},
      {"<dc:creator", "<rdf:li", 1},
      {"<dc:language", "<rdf:li", 2},
  };
  for (const Element& element : elements) {
    const uint8_t* text = nullptr;
    size_t textLength = 0;
    PdfStatus status = xmpElementText(source, length, element.container, element.item, &text, &textLength);
    if (!status) {
      return status;
    }
    if (textLength == 0) {
      continue;
    }
    if (element.field == 0) {
      status = metadata->setTitle(PdfMetadataOrigin::Xmp, text, textLength);
    } else if (element.field == 1) {
      status = metadata->setAuthor(PdfMetadataOrigin::Xmp, text, textLength);
    } else {
      status = metadata->setLanguage(PdfMetadataOrigin::Xmp, text, textLength);
    }
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfOutlineBuilder::begin() {
  if (workspace_.entries == nullptr || workspace_.capacity == 0 || workspace_.capacity > PdfOutlineLimits::MaxEntries) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  count_ = 0;
  initialized_ = true;
  finished_ = false;
  explicitOutline_ = false;
  return PdfStatus::success();
}

bool PdfOutlineBuilder::hasReference(const PdfObjectReference reference) const {
  for (uint16_t index = 0; index < count_; ++index) {
    if (workspace_.entries[index].sourceReference == reference) {
      return true;
    }
  }
  return false;
}

PdfStatus PdfOutlineBuilder::appendEntry(const PdfObjectReference reference, const int16_t parentIndex,
                                         const uint8_t* const title, const size_t titleLength,
                                         const PdfResolvedDestination& destination, const uint8_t explicitLevel) {
  if (!initialized_ || finished_ || count_ >= workspace_.capacity || !destination.resolved ||
      parentIndex >= static_cast<int16_t>(count_) || parentIndex < -1) {
    return count_ >= workspace_.capacity ? PdfStatus::failure(PdfError::LimitExceeded)
                                         : PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint8_t level = explicitLevel != 0
                            ? explicitLevel
                            : static_cast<uint8_t>(parentIndex < 0 ? 1 : workspace_.entries[parentIndex].level + 1);
  if (level == 0 || level > PdfOutlineLimits::MaxDepth || (parentIndex < 0 && level != 1) ||
      (parentIndex >= 0 && level != static_cast<uint8_t>(workspace_.entries[parentIndex].level + 1))) {
    return PdfStatus::failure(PdfError::LimitExceeded, level);
  }

  PdfOutlineEntry entry{};
  entry.sourceReference = reference;
  entry.parentIndex = parentIndex;
  entry.sectionIndex = destination.sectionIndex;
  entry.anchorOrdinal = destination.anchorOrdinal;
  entry.sourcePageIndex = destination.sourcePageIndex;
  entry.level = level;
  PdfStatus status = copyBoundedUtf8(title, titleLength, entry.title, sizeof(entry.title), &entry.titleLength);
  if (status) {
    status = pdfFormatSemanticAnchor(entry.anchorOrdinal, entry.anchor);
  }
  if (!status) {
    return status;
  }
  workspace_.entries[count_++] = entry;
  return PdfStatus::success();
}

PdfStatus PdfOutlineBuilder::append(const PdfOutlineCandidate& candidate) {
  if (initialized_ && hasReference(candidate.reference)) {
    return PdfStatus::failure(PdfError::Malformed, candidate.reference.objectNumber);
  }
  const PdfStatus status = appendEntry(candidate.reference, candidate.parentIndex, candidate.title,
                                       candidate.titleLength, candidate.destination);
  if (status) {
    explicitOutline_ = true;
  }
  return status;
}

PdfStatus PdfOutlineBuilder::appendHeading(const uint8_t* const title, const size_t titleLength,
                                           const uint16_t sectionIndex, const uint32_t anchorOrdinal,
                                           const uint8_t sourceHeadingLevel) {
  if (explicitOutline_) {
    return PdfStatus::success();
  }
  int16_t parentIndex = -1;
  uint8_t level = 1;
  if (count_ != 0) {
    const uint8_t priorSourceLevel = static_cast<uint8_t>(workspace_.entries[count_ - 1].sourceReference.generation);
    if (sourceHeadingLevel > priorSourceLevel && workspace_.entries[count_ - 1].level < PdfOutlineLimits::MaxDepth) {
      parentIndex = static_cast<int16_t>(count_ - 1);
      level = static_cast<uint8_t>(workspace_.entries[count_ - 1].level + 1);
    } else {
      for (int16_t index = static_cast<int16_t>(count_ - 1); index >= 0; --index) {
        const uint8_t candidateLevel = static_cast<uint8_t>(workspace_.entries[index].sourceReference.generation);
        if (candidateLevel < sourceHeadingLevel) {
          parentIndex = index;
          level = static_cast<uint8_t>(workspace_.entries[index].level + 1);
          break;
        }
      }
    }
  }
  const PdfObjectReference synthetic{0x80000000U + count_, sourceHeadingLevel};
  return appendEntry(synthetic, parentIndex, title, titleLength, {sectionIndex, anchorOrdinal, 0, true}, level);
}

PdfStatus PdfOutlineBuilder::finish(const uint8_t* const fallbackTitle, const size_t fallbackTitleLength) {
  if (!initialized_ || finished_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (count_ == 0) {
    const PdfStatus status = appendEntry({0, 0}, -1, fallbackTitle, fallbackTitleLength, {0, 0, 0, true}, 1);
    if (!status) {
      return status;
    }
  }
  finished_ = true;
  return PdfStatus::success();
}

PdfStatus pdfResolveInternalAction(const PdfActionKind action, const PdfResolvedDestination& destination,
                                   char* const href, const size_t capacity, size_t* const length) {
  if (href == nullptr || capacity == 0 || length == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  href[0] = '\0';
  *length = 0;
  if (action != PdfActionKind::GoTo) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  if (!destination.resolved) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  char anchor[PdfOutlineLimits::AnchorBytes]{};
  PdfStatus status = pdfFormatSemanticAnchor(destination.anchorOrdinal, anchor);
  if (!status) {
    return status;
  }
  const int written = std::snprintf(href, capacity, "sections/%06u.xhtml#%s", destination.sectionIndex, anchor);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    href[0] = '\0';
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  *length = static_cast<size_t>(written);
  return PdfStatus::success();
}

PdfStatus PdfPageLabelMap::begin() {
  if (workspace_.ranges == nullptr || workspace_.capacity == 0 ||
      workspace_.capacity > PdfOutlineLimits::MaxPageLabelRanges) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  count_ = 0;
  initialized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPageLabelMap::add(const PdfPageLabelRange& range) {
  if (!initialized_ || count_ >= workspace_.capacity || range.startNumber == 0 ||
      range.prefixLength >= PdfOutlineLimits::PageLabelPrefixBytes ||
      (count_ != 0 && range.firstPageIndex <= workspace_.ranges[count_ - 1].firstPageIndex)) {
    return count_ >= workspace_.capacity ? PdfStatus::failure(PdfError::LimitExceeded)
                                         : PdfStatus::failure(PdfError::Malformed);
  }
  size_t offset = 0;
  while (offset < range.prefixLength) {
    uint32_t scalar = 0;
    const PdfStatus status =
        pdfDecodeUtf8Scalar(reinterpret_cast<const uint8_t*>(range.prefix), range.prefixLength, &offset, &scalar);
    if (!status) {
      return status;
    }
  }
  workspace_.ranges[count_++] = range;
  workspace_.ranges[count_ - 1].prefix[range.prefixLength] = '\0';
  return PdfStatus::success();
}

PdfStatus PdfPageLabelMap::format(const uint32_t pageIndex, char* const output, const size_t capacity,
                                  size_t* const length) const {
  if (!initialized_ || output == nullptr || capacity == 0 || length == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  output[0] = '\0';
  *length = 0;

  PdfPageLabelRange selected{};
  selected.firstPageIndex = 0;
  selected.startNumber = 1;
  selected.style = PdfPageLabelStyle::Decimal;
  for (uint8_t index = 0; index < count_ && workspace_.ranges[index].firstPageIndex <= pageIndex; ++index) {
    selected = workspace_.ranges[index];
  }
  const uint32_t delta = pageIndex - selected.firstPageIndex;
  if (selected.startNumber > std::numeric_limits<uint32_t>::max() - delta) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  const uint32_t value = selected.startNumber + delta;
  if (selected.prefixLength >= capacity) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  std::memcpy(output, selected.prefix, selected.prefixLength);
  *length = selected.prefixLength;
  output[*length] = '\0';

  if (selected.style == PdfPageLabelStyle::None) {
    return PdfStatus::success();
  }
  if (selected.style == PdfPageLabelStyle::UpperRoman || selected.style == PdfPageLabelStyle::LowerRoman) {
    return formatRoman(value, selected.style == PdfPageLabelStyle::UpperRoman, output, capacity, length);
  }
  if (selected.style == PdfPageLabelStyle::UpperAlpha || selected.style == PdfPageLabelStyle::LowerAlpha) {
    return formatAlpha(value, selected.style == PdfPageLabelStyle::UpperAlpha, output, capacity, length);
  }
  char number[16]{};
  const int written = std::snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(value));
  return written < 0 || static_cast<size_t>(written) >= sizeof(number) ? PdfStatus::failure(PdfError::LimitExceeded)
                                                                       : appendNumber(output, capacity, length, number);
}

PdfStepResult pdfStepEncodeOutline(
    const PdfOutlineEntrySource& entries,
    const PdfByteSink& destination,
    PdfOutlineEncodeRuntime& runtime,
    PdfOutlineEncodeWorkspace& workspace,
    PdfWorkBudget& budget) {
  if (!entries.valid() || !destination.valid() || entries.count == 0 ||
      entries.count > PdfOutlineLimits::MaxEntries) {
    return PdfStepResult::failure(
        PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (runtime.stage == PdfOutlineEncodeStage::Idle) {
    runtime.crc32 = 0;
    runtime.recordIndex = 0;
    runtime.stage = PdfOutlineEncodeStage::Header;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfOutlineEncodeStage::Header) {
    if (budget.bytesRemaining < kHeaderBytes ||
        !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kHeaderBytes);
    std::memset(workspace.encoded, 0, kHeaderBytes);
    std::memcpy(workspace.encoded, kMagic, sizeof(kMagic));
    putU16(workspace.encoded + 4, PdfOutlineLimits::CodecVersion);
    putU16(workspace.encoded + 6,
           PdfOutlineLimits::EncodedRecordBytes);
    putU16(workspace.encoded + 8, entries.count);
    putU16(workspace.encoded + 10, 0);
    putU32(
        workspace.encoded + 12,
        static_cast<uint32_t>(entries.count) *
            PdfOutlineLimits::EncodedRecordBytes);
    const uint32_t crc =
        pdfCacheCrc32(workspace.encoded, kHeaderBytes);
    const PdfStatus status =
        pdfWriteExact(destination, workspace.encoded, kHeaderBytes);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime.crc32 = crc;
    runtime.stage = PdfOutlineEncodeStage::Records;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfOutlineEncodeStage::Records) {
    if (runtime.recordIndex >= entries.count) {
      runtime.stage = PdfOutlineEncodeStage::Crc;
      return PdfStepResult::paused();
    }
    workspace.entry = {};
    PdfStatus status = entries.read(
        entries.context, runtime.recordIndex, &workspace.entry);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    uint8_t parentLevel = 0;
    if (workspace.entry.parentIndex >= 0) {
      workspace.parent = {};
      status = entries.read(
          entries.context,
          static_cast<uint16_t>(workspace.entry.parentIndex),
          &workspace.parent);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      parentLevel = workspace.parent.level;
    }
    if (!validEntry(workspace.entry, runtime.recordIndex,
                    parentLevel)) {
      return PdfStepResult::failure(PdfStatus::failure(
          PdfError::Malformed, runtime.recordIndex));
    }
    status = validateUtf8Title(workspace.entry);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (budget.bytesRemaining <
            PdfOutlineLimits::EncodedRecordBytes ||
        !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(PdfOutlineLimits::EncodedRecordBytes);
    encodeEntry(workspace.entry, workspace.encoded);
    const uint32_t crc = pdfCacheCrc32(
        workspace.encoded, PdfOutlineLimits::EncodedRecordBytes,
        runtime.crc32);
    status = pdfWriteExact(
        destination, workspace.encoded,
        PdfOutlineLimits::EncodedRecordBytes);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime.crc32 = crc;
    ++runtime.recordIndex;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfOutlineEncodeStage::Crc) {
    if (budget.bytesRemaining < kCrcBytes ||
        !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kCrcBytes);
    putU32(workspace.encoded, runtime.crc32);
    const PdfStatus status =
        pdfWriteExact(destination, workspace.encoded, kCrcBytes);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime.stage = PdfOutlineEncodeStage::Complete;
    return PdfStepResult::completed();
  }

  return runtime.stage == PdfOutlineEncodeStage::Complete
             ? PdfStepResult::completed()
             : PdfStepResult::failure(
                   PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus pdfEncodeOutline(const PdfOutlineEntrySource& entries,
                           const PdfByteSink& destination) {
  PdfOutlineEncodeRuntime runtime{};
  PdfOutlineEncodeWorkspace workspace{};
  PdfWorkBudget budget{UINT32_MAX, SIZE_MAX};
  for (;;) {
    const PdfStepResult result = pdfStepEncodeOutline(
        entries, destination, runtime, workspace, budget);
    if (result.failed()) {
      return result.status;
    }
    if (result.complete()) {
      return PdfStatus::success();
    }
  }
}

PdfStatus pdfDecodeOutline(const PdfByteSource& source, PdfOutlineHeader* const header,
                           const PdfOutlineEntryVisitor& entries) {
  if (!source.valid() || header == nullptr || !entries.valid() || source.size < kHeaderBytes + kCrcBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t encodedHeader[kHeaderBytes]{};
  PdfStatus status = pdfReadExact(source, 0, encodedHeader, sizeof(encodedHeader));
  if (!status) {
    return status;
  }
  const uint16_t count = getU16(encodedHeader + 8);
  const uint64_t expectedSize =
      kHeaderBytes + static_cast<uint64_t>(count) * PdfOutlineLimits::EncodedRecordBytes + kCrcBytes;
  if (std::memcmp(encodedHeader, kMagic, sizeof(kMagic)) != 0 ||
      getU16(encodedHeader + 4) != PdfOutlineLimits::CodecVersion ||
      getU16(encodedHeader + 6) != PdfOutlineLimits::EncodedRecordBytes || count == 0 ||
      count > PdfOutlineLimits::MaxEntries || getU16(encodedHeader + 10) != 0 ||
      getU32(encodedHeader + 12) != static_cast<uint32_t>(count) * PdfOutlineLimits::EncodedRecordBytes ||
      source.size != expectedSize) {
    return PdfStatus::failure(PdfError::Malformed);
  }

  uint32_t crc = pdfCacheCrc32(encodedHeader, sizeof(encodedHeader));
  uint64_t offset = sizeof(encodedHeader);
  uint8_t levels[PdfOutlineLimits::MaxEntries]{};
  for (uint16_t index = 0; index < count; ++index) {
    uint8_t encoded[PdfOutlineLimits::EncodedRecordBytes]{};
    status = pdfReadExact(source, offset, encoded, sizeof(encoded));
    if (!status) {
      return status;
    }
    offset += sizeof(encoded);
    crc = pdfCacheCrc32(encoded, sizeof(encoded), crc);
    PdfOutlineEntry entry = decodeEntry(encoded);
    const uint8_t parentLevel = entry.parentIndex < 0 ? 0 : levels[entry.parentIndex];
    if (!validEntry(entry, index, parentLevel)) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    status = validateUtf8Title(entry);
    if (!status) {
      return status;
    }
    levels[index] = entry.level;
    status = entries.accept(entries.context, index, entry);
    if (!status) {
      return status;
    }
  }
  uint8_t encodedCrc[kCrcBytes]{};
  status = pdfReadExact(source, offset, encodedCrc, sizeof(encodedCrc));
  if (!status) {
    return status;
  }
  if (getU32(encodedCrc) != crc) {
    return PdfStatus::failure(PdfError::Malformed, offset);
  }
  header->entryCount = count;
  return PdfStatus::success();
}

PdfStatus pdfReadOutlineEntry(const PdfByteSource& source, const uint16_t index, PdfOutlineEntry* const entry) {
  if (!source.valid() || entry == nullptr || source.size < kHeaderBytes + kCrcBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t header[kHeaderBytes]{};
  PdfStatus status = pdfReadExact(source, 0, header, sizeof(header));
  if (!status) {
    return status;
  }
  const uint16_t count = getU16(header + 8);
  const uint64_t expectedSize =
      kHeaderBytes + static_cast<uint64_t>(count) * PdfOutlineLimits::EncodedRecordBytes + kCrcBytes;
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0 || getU16(header + 4) != PdfOutlineLimits::CodecVersion ||
      getU16(header + 6) != PdfOutlineLimits::EncodedRecordBytes || count == 0 ||
      count > PdfOutlineLimits::MaxEntries || index >= count || getU16(header + 10) != 0 ||
      getU32(header + 12) != static_cast<uint32_t>(count) * PdfOutlineLimits::EncodedRecordBytes ||
      source.size != expectedSize) {
    return PdfStatus::failure(PdfError::Malformed, index);
  }

  uint8_t encoded[PdfOutlineLimits::EncodedRecordBytes]{};
  const uint64_t recordOffset = kHeaderBytes + static_cast<uint64_t>(index) * PdfOutlineLimits::EncodedRecordBytes;
  status = pdfReadExact(source, recordOffset, encoded, sizeof(encoded));
  if (!status) {
    return status;
  }
  PdfOutlineEntry decoded = decodeEntry(encoded);
  uint8_t parentLevel = 0;
  if (decoded.parentIndex >= 0) {
    if (decoded.parentIndex >= static_cast<int16_t>(index)) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    uint8_t parentEncoded[PdfOutlineLimits::EncodedRecordBytes]{};
    const uint64_t parentOffset =
        kHeaderBytes + static_cast<uint64_t>(decoded.parentIndex) * PdfOutlineLimits::EncodedRecordBytes;
    status = pdfReadExact(source, parentOffset, parentEncoded, sizeof(parentEncoded));
    if (!status) {
      return status;
    }
    parentLevel = parentEncoded[10];
  }
  if (!validEntry(decoded, index, parentLevel)) {
    return PdfStatus::failure(PdfError::Malformed, index);
  }
  status = validateUtf8Title(decoded);
  if (status) {
    *entry = decoded;
  }
  return status;
}
