#include "ReaderRoute.h"

#include <cctype>

namespace {

bool hasExtension(const std::string_view path, const std::string_view extension) {
  if (path.size() < extension.size()) {
    return false;
  }

  const size_t offset = path.size() - extension.size();
  for (size_t index = 0; index < extension.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(path[offset + index])) !=
        std::tolower(static_cast<unsigned char>(extension[index]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

ReaderRoute selectReaderRoute(const std::string_view path) {
  if (path.empty()) {
    return ReaderRoute::Library;
  }
  if (hasExtension(path, ".bmp") || hasExtension(path, ".png")) {
    return ReaderRoute::Image;
  }
  if (hasExtension(path, ".xtc") || hasExtension(path, ".xtch")) {
    return ReaderRoute::Xtc;
  }
  if (hasExtension(path, ".txt") || hasExtension(path, ".md")) {
    return ReaderRoute::Text;
  }
  if (hasExtension(path, ".pdf")) {
    return ReaderRoute::Pdf;
  }
  return ReaderRoute::Epub;
}

bool dispatchReaderRoute(const std::string_view path, const ReaderRouteHandlers& handlers) {
  ReaderRouteHandler handler = nullptr;
  switch (selectReaderRoute(path)) {
    case ReaderRoute::Library:
      handler = handlers.openLibrary;
      break;
    case ReaderRoute::Image:
      handler = handlers.openImage;
      break;
    case ReaderRoute::Xtc:
      handler = handlers.openXtc;
      break;
    case ReaderRoute::Text:
      handler = handlers.openText;
      break;
    case ReaderRoute::Pdf:
      handler = handlers.openPdf;
      break;
    case ReaderRoute::Epub:
      handler = handlers.openEpub;
      break;
  }
  return handler != nullptr && handler(handlers.context);
}
