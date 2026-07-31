#include "BookMoveDurableFile.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace BookMoveDurableFile {
namespace {

bool validPathSet(const char* const canonical, const char* const temporary, const char* const backup) {
  return canonical != nullptr && temporary != nullptr && backup != nullptr && canonical[0] != '\0' &&
         temporary[0] != '\0' && backup[0] != '\0' && std::strcmp(canonical, temporary) != 0 &&
         std::strcmp(canonical, backup) != 0 && std::strcmp(temporary, backup) != 0;
}

bool removeIfPresent(const char* const path) { return !Storage.exists(path) || Storage.remove(path); }

bool restoreBackup(const char* const canonical, const char* const backup) {
  if (!Storage.exists(backup)) return true;
  if (Storage.exists(canonical) && !Storage.remove(canonical)) return false;
  return Storage.rename(backup, canonical);
}

bool publishTemporary(const char* const canonical, const char* const temporary, const char* const backup,
                      const Payload& payload) {
  const bool hadCanonical = Storage.exists(canonical);
  if (hadCanonical && !Storage.rename(canonical, backup)) {
    LOG_ERR("BookMove", "Failed to preserve prior state before activation");
    return false;
  }
  if (!Storage.rename(temporary, canonical)) {
    LOG_ERR("BookMove", "Failed to promote durable book-move state");
    if (!restoreBackup(canonical, backup)) {
      LOG_ERR("BookMove", "Failed to restore prior book-move state");
    }
    (void)removeIfPresent(temporary);
    return false;
  }
  if (!payload.verify(payload.context, canonical)) {
    LOG_ERR("BookMove", "Promoted book-move state failed readback");
    if (!restoreBackup(canonical, backup)) {
      LOG_ERR("BookMove", "Failed to roll back unreadable book-move state");
    }
    return false;
  }
  if (!removeIfPresent(backup)) {
    LOG_ERR("BookMove", "Failed to remove committed book-move backup");
    return false;
  }
  return true;
}

bool recoverArtifacts(const char* const canonical, const char* const temporary, const char* const backup,
                      const Payload& payload, bool* const complete) {
  *complete = false;

  if (Storage.exists(canonical) && payload.verify(payload.context, canonical)) {
    if (!removeIfPresent(temporary) || !removeIfPresent(backup)) return false;
    *complete = true;
    return true;
  }

  const bool hasBackup = Storage.exists(backup);
  const bool hasTemporary = Storage.exists(temporary);
  if (!Storage.exists(canonical) && hasTemporary && payload.verify(payload.context, temporary)) {
    if (!publishTemporary(canonical, temporary, backup, payload)) return false;
    *complete = true;
    return true;
  }

  if (hasBackup && !restoreBackup(canonical, backup)) {
    LOG_ERR("BookMove", "Failed to recover interrupted book-move state");
    return false;
  }
  if (!removeIfPresent(temporary)) {
    LOG_ERR("BookMove", "Failed to remove stale book-move temporary");
    return false;
  }
  return true;
}

}  // namespace

bool restoreCanonicalForRead(const char* const canonical, const char* const temporary, const char* const backup) {
  if (!validPathSet(canonical, temporary, backup)) {
    LOG_ERR("BookMove", "Invalid durable state recovery paths");
    return false;
  }
  if (Storage.exists(canonical)) return true;

  if (Storage.exists(backup)) {
    if (!Storage.rename(backup, canonical)) {
      LOG_ERR("BookMove", "Failed to restore durable state before load");
      return false;
    }
    if (!removeIfPresent(temporary)) {
      // The canonical backup is readable again. Its owning journal will retry
      // cleanup before publishing the desired replacement.
      LOG_ERR("BookMove", "Failed to remove stale durable-state temporary");
    }
    return true;
  }

  if (Storage.exists(temporary) && !Storage.rename(temporary, canonical)) {
    LOG_ERR("BookMove", "Failed to promote durable state before load");
    return false;
  }
  return true;
}

bool replace(const char* const canonical, const char* const temporary, const char* const backup,
             const Payload& payload) {
  if (!validPathSet(canonical, temporary, backup) || !payload.valid()) {
    LOG_ERR("BookMove", "Invalid durable book-move state replacement");
    return false;
  }

  bool complete = false;
  if (!recoverArtifacts(canonical, temporary, backup, payload, &complete)) {
    return false;
  }
  if (complete) return true;

  FsFile file;
  if (!Storage.openFileForWrite("BookMove", temporary, file)) {
    LOG_ERR("BookMove", "Failed to open book-move temporary state");
    return false;
  }
  bool written = payload.write(payload.context, &file);
  if (written) {
    file.flush();
    written = file.sync();
  }
  const bool closed = file.close();
  if (!written || !closed) {
    LOG_ERR("BookMove", "Failed to durably write book-move temporary state");
    (void)removeIfPresent(temporary);
    return false;
  }
  if (!payload.verify(payload.context, temporary)) {
    LOG_ERR("BookMove", "Book-move temporary state failed readback");
    (void)removeIfPresent(temporary);
    return false;
  }
  return publishTemporary(canonical, temporary, backup, payload);
}

}  // namespace BookMoveDurableFile
