#include <gtest/gtest.h>

#include <cstdint>

#include "QemuStorageCapacityCache.h"

TEST(QemuStorageCapacityCacheTest, SnapshotsCappedPhysicalCapacityAndUsedBytes) {
  QemuStorageCapacityCache cache;
  cache.refresh(100, 20);

  EXPECT_EQ(cache.remaining(), 80U);
  EXPECT_TRUE(cache.canWrite(80));
  EXPECT_FALSE(cache.canWrite(81));
}

TEST(QemuStorageCapacityCacheTest, ChargesOnlyActualShortWriteBytesAndNoFailedBytes) {
  QemuStorageCapacityCache cache;
  cache.refresh(100, 20);
  ASSERT_TRUE(cache.canWrite(10));

  cache.charge(4);  // A ten-byte request returned a four-byte short write.
  EXPECT_EQ(cache.remaining(), 76U);
  cache.charge(0);  // A failed write returned zero bytes.
  EXPECT_EQ(cache.remaining(), 76U);
}

TEST(QemuStorageCapacityCacheTest, NeverCreditsSpaceUntilTheNextBeginRefresh) {
  QemuStorageCapacityCache cache;
  cache.refresh(100, 20);
  cache.charge(30);
  EXPECT_EQ(cache.remaining(), 50U);

  // Delete, truncate, overwrite, rename, and quota changes deliberately have
  // no cache operation. Only a new HalStorage::begin snapshot may recover
  // actual filesystem space.
  EXPECT_EQ(cache.remaining(), 50U);
  cache.refresh(100, 5);
  EXPECT_EQ(cache.remaining(), 95U);
}

TEST(QemuStorageCapacityCacheTest, SaturatesWithoutOverflowAndRecoversOnRefresh) {
  QemuStorageCapacityCache cache;
  cache.refresh(UINT64_MAX, UINT64_MAX - 2U);
  cache.charge(10);

  EXPECT_EQ(cache.remaining(), 0U);
  EXPECT_FALSE(cache.canWrite(1));

  cache.refresh(50, 60);
  EXPECT_EQ(cache.remaining(), 0U);
  EXPECT_FALSE(cache.canWrite(0));

  cache.refresh(50, 10);
  EXPECT_EQ(cache.remaining(), 40U);
  EXPECT_TRUE(cache.canWrite(40));
}
