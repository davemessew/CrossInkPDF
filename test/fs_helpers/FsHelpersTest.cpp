#include <gtest/gtest.h>

#include <FsHelpers.h>

TEST(FsHelpersNormalisePath, PreservesRootForBorrowedPdfGenerationResource) {
  EXPECT_EQ(FsHelpers::normalisePath(
                "/.crosspoint/pdf_123/gen_7/sections/../images/0123456789abcdef-89abcdef.pxc",
                /*preserveRoot=*/true),
            "/.crosspoint/pdf_123/gen_7/images/0123456789abcdef-89abcdef.pxc");
}

TEST(FsHelpersNormalisePath, KeepsAbsoluteEpubArchivePathRootlessByDefault) {
  EXPECT_EQ(FsHelpers::normalisePath("/OEBPS/text/../images/cover.png"), "OEBPS/images/cover.png");
}

TEST(FsHelpersNormalisePath, KeepsRelativeEpubPathsByteForByteCompatible) {
  EXPECT_EQ(FsHelpers::normalisePath("OEBPS/text/../images/cover.jpg"), "OEBPS/images/cover.jpg");
  EXPECT_EQ(FsHelpers::normalisePath("sections/000000.xhtml"), "sections/000000.xhtml");
  EXPECT_EQ(FsHelpers::normalisePath("../images/cover.jpg"), "images/cover.jpg");
  EXPECT_EQ(FsHelpers::normalisePath(""), "");
}

TEST(FsHelpersNormalisePath, KeepsRootOnlyAndCollapsedInputsCompatible) {
  EXPECT_EQ(FsHelpers::normalisePath("/"), "");
  EXPECT_EQ(FsHelpers::normalisePath("../../"), "");
  EXPECT_EQ(FsHelpers::normalisePath("folder//child/"), "folder/child");
}

TEST(FsHelpersNormalisePath, PreservesOptedInRootWhenNoComponentsRemain) {
  EXPECT_EQ(FsHelpers::normalisePath("/", /*preserveRoot=*/true), "/");
  EXPECT_EQ(FsHelpers::normalisePath("/..", /*preserveRoot=*/true), "/");
}

TEST(FsHelpersNormalisePath, KeepsCollapsedPathsRootlessWithoutOptIn) {
  EXPECT_EQ(FsHelpers::normalisePath("/"), "");
  EXPECT_EQ(FsHelpers::normalisePath(""), "");
  EXPECT_EQ(FsHelpers::normalisePath(".."), "");
}
