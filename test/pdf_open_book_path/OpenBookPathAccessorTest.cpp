#include "CrossPointState.h"

#include <string>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<CrossPointState&>().openBookPath()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const CrossPointState&>().openBookPath()), const std::string&>);
static_assert(noexcept(std::declval<CrossPointState&>().openBookPath()));
static_assert(noexcept(std::declval<const CrossPointState&>().openBookPath()));

int main() {
  static CrossPointState state;
  const char* const supportedPaths[] = {
      "/books/book.epub",
      "/books/book.txt",
      "/books/book.xtc",
      "/books/book.pdf",
  };

  for (const char* path : supportedPaths) {
    state.openEpubPath = path;
    if (&state.openBookPath() != &state.openEpubPath || state.openBookPath() != path) {
      return 1;
    }
  }

  state.openBookPath() = "/books/resumed.pdf";
  if (state.openEpubPath != "/books/resumed.pdf") {
    return 2;
  }

  const CrossPointState& constState = state;
  if (&constState.openBookPath() != &state.openEpubPath || constState.openBookPath() != "/books/resumed.pdf") {
    return 3;
  }

  return 0;
}
