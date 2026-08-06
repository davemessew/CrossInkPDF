#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "Reflow/ReflowDocument.h"

namespace {

class FakeSectionSource final : public ReflowSectionSource {
 public:
  bool exposeSection = false;
  bool exposeResource = false;
  ReflowResource section;
  ReflowResource resource;
  mutable int localSectionQueries = 0;
  mutable int sectionStreams = 0;
  mutable int resourceQueries = 0;
  mutable int resourceStreams = 0;

  bool getLocalSectionPath(const int, ReflowResource& out) const override {
    ++localSectionQueries;
    out = section;
    return exposeSection;
  }

  bool streamSection(const int, Print&, const size_t) const override {
    ++sectionStreams;
    return true;
  }

  bool resolveResource(const int, const std::string&, ReflowResource& out) const override {
    ++resourceQueries;
    out = resource;
    return exposeResource;
  }

  bool streamResource(const int, const std::string&, Print&, const size_t) const override {
    ++resourceStreams;
    return true;
  }

  bool getResourceSize(const int, const std::string&, size_t* size) const override {
    if (size == nullptr) {
      return false;
    }
    *size = 0;
    return true;
  }

  CssParser* getCssParser() const override { return nullptr; }
};

TEST(ReflowSectionSource, SelectsBorrowedLocalSectionWithoutStreamingCopy) {
  FakeSectionSource source;
  source.exposeSection = true;
  source.section = ReflowResource::borrowedLocalFile("/generated/section-0.xhtml", ReflowImageKind::EncodedImage);

  ReflowResource selected;
  ASSERT_TRUE(source.getImmutableLocalSection(0, selected));
  EXPECT_EQ(selected.kind, ReflowResourceKind::BorrowedLocalFile);
  EXPECT_EQ(selected.localPath, "/generated/section-0.xhtml");
  EXPECT_FALSE(selected.paginatorMayDelete);
  EXPECT_EQ(source.localSectionQueries, 1);
  EXPECT_EQ(source.sectionStreams, 0);
}

TEST(ReflowSectionSource, RejectsBorrowedSectionThatClaimsDeletePermission) {
  FakeSectionSource source;
  source.exposeSection = true;
  source.section = ReflowResource::borrowedLocalFile("/generated/section-0.xhtml", ReflowImageKind::EncodedImage);
  source.section.paginatorMayDelete = true;

  ReflowResource selected = ReflowResource::borrowedLocalFile("/stale", ReflowImageKind::EncodedImage);
  EXPECT_FALSE(source.getImmutableLocalSection(0, selected));
  EXPECT_EQ(selected.kind, ReflowResourceKind::Streamed);
  EXPECT_TRUE(selected.localPath.empty());
  EXPECT_FALSE(selected.paginatorMayDelete);
}

TEST(ReflowSectionSource, KeepsStreamedSectionOnTheStreamingPath) {
  FakeSectionSource source;
  source.section = ReflowResource::streamed();

  ReflowResource selected = ReflowResource::borrowedLocalFile("/stale/section.xhtml", ReflowImageKind::EncodedImage);
  EXPECT_FALSE(source.getImmutableLocalSection(0, selected));
  EXPECT_EQ(selected.kind, ReflowResourceKind::Streamed);
  EXPECT_TRUE(selected.localPath.empty());
  EXPECT_FALSE(selected.paginatorMayDelete);
  EXPECT_EQ(source.localSectionQueries, 1);
  EXPECT_EQ(source.sectionStreams, 0);
}

TEST(ReflowSectionSource, SelectsBorrowedEncodedImageWithoutGrantingDeletePermission) {
  FakeSectionSource source;
  source.exposeResource = true;
  source.resource =
      ReflowResource::borrowedLocalFile("/generated/image-0.jpg", ReflowImageKind::EncodedImage, 640, 480);

  ReflowResource selected;
  ASSERT_TRUE(source.getImmutableLocalResource(0, "image-0.jpg", selected));
  EXPECT_EQ(selected.kind, ReflowResourceKind::BorrowedLocalFile);
  EXPECT_EQ(selected.imageKind, ReflowImageKind::EncodedImage);
  EXPECT_EQ(selected.localPath, "/generated/image-0.jpg");
  EXPECT_EQ(selected.width, 640);
  EXPECT_EQ(selected.height, 480);
  EXPECT_FALSE(selected.paginatorMayDelete);
  EXPECT_EQ(source.resourceQueries, 1);
  EXPECT_EQ(source.resourceStreams, 0);
}

TEST(ReflowSectionSource, RequiresDimensionsForBorrowedPixelCache) {
  FakeSectionSource source;
  source.exposeResource = true;
  source.resource = ReflowResource::borrowedLocalFile("/generated/image-0.pxc", ReflowImageKind::PixelCache);

  ReflowResource selected;
  EXPECT_FALSE(source.getImmutableLocalResource(0, "image-0.pxc", selected));
  EXPECT_EQ(selected.kind, ReflowResourceKind::Streamed);

  source.resource = ReflowResource::borrowedLocalFile("/generated/image-0.pxc", ReflowImageKind::PixelCache, 320, 200);
  ASSERT_TRUE(source.getImmutableLocalResource(0, "image-0.pxc", selected));
  EXPECT_EQ(selected.imageKind, ReflowImageKind::PixelCache);
  EXPECT_EQ(selected.width, 320);
  EXPECT_EQ(selected.height, 200);
  EXPECT_FALSE(selected.paginatorMayDelete);
}

}  // namespace
