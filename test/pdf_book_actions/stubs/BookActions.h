#pragma once

#include <string>
#include <vector>

#include "I18n.h"

class GfxRenderer;

enum class FileBrowserAction : int {
  Delete = 0,
  PinFavorite = 1,
  UnpinFavorite = 2,
  SetSleepFolder = 3,
  ClearSleepFolder = 4,
  DeleteCache = 5,
  ToggleCompleted = 6,
  RemoveFromRecents = 7,
  DeleteStats = 8,
  ViewBookmarks = 9,
  ViewClippings = 10,
  DeleteBookmarks = 11,
  DeleteClippings = 12,
  EpubRenderMode = 13,
  ResetReaderSettings = 14,
};

class FileBrowserActionActivity {
 public:
  struct MenuItem {
    FileBrowserAction action;
    StrId labelId;
  };
};

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
bool hasClearableBookCache(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool deletePdfBook(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);
void drawToast(const GfxRenderer& renderer, const char* msg);

}  // namespace BookActions
