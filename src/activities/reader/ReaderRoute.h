#pragma once

#include <cstdint>
#include <string_view>

enum class ReaderRoute : uint8_t {
  Library,
  Image,
  Xtc,
  Text,
  Pdf,
  Epub,
};

using ReaderRouteHandler = bool (*)(void* context);

struct ReaderRouteHandlers {
  void* context = nullptr;
  ReaderRouteHandler openLibrary = nullptr;
  ReaderRouteHandler openImage = nullptr;
  ReaderRouteHandler openXtc = nullptr;
  ReaderRouteHandler openText = nullptr;
  ReaderRouteHandler openPdf = nullptr;
  ReaderRouteHandler openEpub = nullptr;
};

ReaderRoute selectReaderRoute(std::string_view path);
bool dispatchReaderRoute(std::string_view path, const ReaderRouteHandlers& handlers);
