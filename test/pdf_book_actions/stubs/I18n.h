#pragma once

#include <cstdint>

enum class StrId : uint16_t {
  STR_DELETE,
  STR_DELETE_CACHE,
  STR_EPUB_RENDER_MODE,
  STR_RESET_BOOK_READER_SETTINGS,
  STR_DELETE_BOOK_STATS,
  STR_MARK_UNFINISHED,
  STR_MARK_FINISHED,
  STR_REMOVE_FROM_RECENTS_ACTION,
  STR_RENDER_MODE_CROSSINK_DEFAULT,
  STR_RENDER_MODE_BALANCED,
  STR_RENDER_MODE_LIGHT,
  STR_CONFIRM,
  STR_MOVE_TO_READ_FAILED_TITLE,
  STR_MOVE_TO_READ_FAILED_BODY,
  STR_STATS_LESS_THAN_MIN,
};

inline const char* tr(const StrId id) {
  switch (id) {
    case StrId::STR_CONFIRM:
      return "Confirm";
    case StrId::STR_MOVE_TO_READ_FAILED_TITLE:
      return "Move failed";
    case StrId::STR_MOVE_TO_READ_FAILED_BODY:
      return "Could not move %s";
    default:
      return "text";
  }
}

#define STR_CONFIRM StrId::STR_CONFIRM
#define STR_MOVE_TO_READ_FAILED_TITLE StrId::STR_MOVE_TO_READ_FAILED_TITLE
#define STR_MOVE_TO_READ_FAILED_BODY StrId::STR_MOVE_TO_READ_FAILED_BODY
#define STR_STATS_LESS_THAN_MIN StrId::STR_STATS_LESS_THAN_MIN

struct FakeI18n {
  const char* get(StrId id) const { return tr(id); }
};

extern FakeI18n I18N;
