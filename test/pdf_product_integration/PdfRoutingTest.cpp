#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "ReaderRoute.h"

namespace {

struct RouteSpy {
  std::array<unsigned, 6> calls{};

  static bool library(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[0];
    return true;
  }

  static bool image(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[1];
    return true;
  }

  static bool xtc(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[2];
    return true;
  }

  static bool text(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[3];
    return true;
  }

  static bool pdf(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[4];
    return true;
  }

  static bool epub(void* context) {
    ++static_cast<RouteSpy*>(context)->calls[5];
    return true;
  }

  ReaderRouteHandlers handlers() {
    return {
        this, library, image, xtc, text, pdf, epub,
    };
  }
};

TEST(PdfRouting, SelectsPdfCaseInsensitively) {
  EXPECT_EQ(selectReaderRoute("/books/manual.pdf"), ReaderRoute::Pdf);
  EXPECT_EQ(selectReaderRoute("/books/manual.PDF"), ReaderRoute::Pdf);
  EXPECT_EQ(selectReaderRoute("/books/manual.PdF"), ReaderRoute::Pdf);
}

TEST(PdfRouting, DispatchesPdfWithoutCallingEpubLoader) {
  for (const std::string_view path : {"/manual.pdf", "/manual.PDF", "/manual.pDf"}) {
    RouteSpy spy;
    const auto handlers = spy.handlers();

    ASSERT_TRUE(dispatchReaderRoute(path, handlers));
    EXPECT_EQ(spy.calls[4], 1U);
    EXPECT_EQ(spy.calls[5], 0U);
  }
}

TEST(PdfRouting, PreservesExistingRoutesAndUnknownEpubFallback) {
  EXPECT_EQ(selectReaderRoute({}), ReaderRoute::Library);
  EXPECT_EQ(selectReaderRoute("/cover.bmp"), ReaderRoute::Image);
  EXPECT_EQ(selectReaderRoute("/cover.PNG"), ReaderRoute::Image);
  EXPECT_EQ(selectReaderRoute("/comic.xtch"), ReaderRoute::Xtc);
  EXPECT_EQ(selectReaderRoute("/notes.MD"), ReaderRoute::Text);
  EXPECT_EQ(selectReaderRoute("/book.epub"), ReaderRoute::Epub);
  EXPECT_EQ(selectReaderRoute("/legacy.unknown"), ReaderRoute::Epub);
}

}  // namespace
