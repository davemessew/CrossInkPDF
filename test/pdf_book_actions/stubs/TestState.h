#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct AddedRecent {
  std::string path;
  std::string title;
  std::string author;
  std::string thumbnail;
};

struct BookActionTestState {
  bool cacheClearResult = true;
  std::vector<std::string> cacheClears;

  bool resolverSucceeds = true;
  bool readOnlyFallback = false;
  uint64_t resolvedHash = 0;
  uint32_t resolverCalls = 0;
  uint32_t cacheFormats = 0;
  bool cacheRootExists = true;
  bool cacheRootMkdirResult = true;
  bool cacheRootIsDirectory = true;
  uint32_t cacheRootMkdirCalls = 0;
  uint32_t cacheRootOpenCalls = 0;

  std::unordered_map<std::string, bool> completedByCache;
  std::vector<std::string> statsLoads;
  std::vector<std::string> statsSaves;
  std::vector<std::string> statsRemoves;
  bool statsRemoveResult = true;
  uint32_t globalCompleted = 0;
  uint32_t globalLoads = 0;
  uint32_t globalSaves = 0;
  bool dateAvailable = false;

  std::vector<std::string> recentRemovals;
  std::vector<AddedRecent> recentAdds;

  uint32_t productLoads = 0;
  uint32_t sourceIdentityPasses = 0;
  uint8_t productKind = 0;
  std::string productTitle;
  std::string productAuthor;
  std::string productThumbnail;
  bool productHashOverrideSupplied = false;
  uint64_t productHashOverride = 0;

  uint32_t pdfMoveCalls = 0;
  std::string pdfMoveOldPath;
  std::string pdfMoveNewPath;
  bool pdfMoveKeepInRecents = true;
  uint8_t pdfMoveResult = 0;
  uint32_t pdfDeleteCalls = 0;
  uint32_t pdfDirectoryDeleteCalls = 0;
  std::string pdfDeletePath;
  uint8_t pdfDeleteResult = 0;
  std::string expectedStatsCachePath;
  bool statsDurableAtMove = false;

  uint32_t storageRenames = 0;
  std::string storageRenameOld;
  std::string storageRenameNew;
  bool storageRenameResult = true;

  uint32_t epubConstructs = 0;
  uint32_t xtcConstructs = 0;
  uint32_t epubSetups = 0;
  uint32_t xtcSetups = 0;
  uint32_t xtcLoads = 0;
  bool xtcLoadResult = true;
  uint32_t epubStateMigrations = 0;
  bool epubMigrationKeepInRecents = true;
  uint32_t owningMetadataPathCalls = 0;
  bool epubNoPathAllocResult = true;
  bool bookmarkNoPathAllocResult = true;
  bool clippingNoPathAllocResult = true;
  std::vector<std::string> metadataDeletes;
  uint32_t resetReaderSettingsCalls = 0;

  uint32_t productStateAllocations = 0;
  bool failProductStateAllocation = false;
};

extern BookActionTestState TEST_STATE;
void resetBookActionTestState();
