#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;
struct Arena;
template <typename T>
class ArenaVector;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;          // true = word attaches to previous (no space before it)
  std::vector<bool> wordNoSpaceBefore;      // true = may break before token, but no synthetic space when joined
  std::vector<uint8_t> wordBionicBoundary;  // UTF-8 byte offset where the regular suffix starts; 0 = no split
  std::vector<bool> wordGuideDotBefore;     // true = virtual guide dot belongs between previous token and this one
  std::vector<uint8_t> wordBackgroundBlack;
  std::vector<std::string> rubyTexts;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  bool guideReadingEnabled;
  bool boundedTokenGrowth;
  uint8_t wordSpacing;
  BlockStyle blockStyle;
  bool hasRtlWord;
  // True after an intermediate flush leaves the rest of the same paragraph
  // buffered. The next layout pass must not apply first-line paragraph rules.
  bool isContinuation_ = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedBionicBoundaryScratch;
  std::vector<bool> reorderedGuideDotBeforeScratch;
  std::vector<uint8_t> reorderedBackgroundBlackScratch;
  std::vector<std::string> lineWordsScratch;
  std::vector<EpdFontFamily::Style> lineStylesScratch;
  std::vector<uint16_t> lineWidthsScratch;
  std::vector<uint8_t> lineBionicBoundaryScratch;
  std::vector<bool> lineGuideDotBeforeScratch;
  std::vector<uint8_t> lineBackgroundBlackScratch;
  std::vector<uint16_t> visualOrderScratch;

  void reserveTokenCapacity(size_t additionalTokens);
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  bool calculateGapMetrics(ArenaVector<int16_t>& naturalGaps, ArenaVector<uint8_t>& gapSlots,
                           const GfxRenderer& renderer, int fontId);
  template <bool SemanticWordTracking>
  bool layoutAndExtractLinesImpl(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                 const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                 bool includeLastLine);
  template <bool SemanticWordTracking>
  bool computeLineBreaks(Arena& scratchArena, const GfxRenderer& renderer, int fontId, int pageWidth,
                         ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                         std::vector<bool>& noSpaceBeforeVec, ArenaVector<int16_t>& naturalGaps,
                         ArenaVector<uint8_t>& gapSlots, ArenaVector<size_t>& lineBreakIndices);
  template <bool SemanticWordTracking>
  bool computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                   std::vector<bool>& noSpaceBeforeVec, ArenaVector<size_t>& lineBreakIndices);
  template <bool SemanticWordTracking>
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            ArenaVector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  template <bool SemanticWordTracking>
  bool splitPathologicalTokenAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                                     ArenaVector<uint16_t>& wordWidths);
  template <bool SemanticWordTracking>
  bool extractLine(Arena& scratchArena, size_t breakIndex, int pageWidth, const ArenaVector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const ArenaVector<int16_t>& naturalGaps, const ArenaVector<uint8_t>& gapSlots,
                   const ArenaVector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  bool calculateWordWidths(ArenaVector<uint16_t>& wordWidths, const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool forceParagraphIndents = false,
                       const bool hyphenationEnabled = false, const bool bionicReadingEnabled = false,
                       const bool guideReadingEnabled = false, const uint8_t wordSpacing = 0,
                       const BlockStyle& blockStyle = BlockStyle(), const bool boundedTokenGrowth = false)
      : extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        boundedTokenGrowth(boundedTokenGrowth),
        wordSpacing(wordSpacing),
        blockStyle(blockStyle),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool backgroundBlack = false, uint8_t linkId = 0);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, bool semanticWordTracking = false);
  bool layoutAndExtractLinesPreservingSource(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                             bool semanticWordTracking = false) const;
};
