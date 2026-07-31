#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "PdfContentInterpreter.h"

namespace {

struct FixedSource {
  const uint8_t* bytes = nullptr;
  size_t size = 0;

  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& source = *static_cast<FixedSource*>(context);
    if (offset > source.size) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t available = source.size - static_cast<size_t>(offset);
    const size_t count = requested < available ? requested : available;
    if (count != 0) {
      std::memcpy(destination, source.bytes + offset, count);
    }
    *bytesRead = count;
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, size, readAt}; }
};

struct ImageResource {
  PdfContentResources resources{};

  ImageResource() {
    resources.context = this;
    resources.resolveXObject = resolveXObject;
  }

  static PdfStatus resolveXObject(void* context, const uint8_t* name, const size_t length,
                                  PdfContentXObject* object) {
    if (context == nullptr || name == nullptr || object == nullptr || length != 1U || name[0] != 'M') {
      return PdfStatus::failure(PdfError::Malformed);
    }
    object->kind = PdfContentXObjectKind::Image;
    object->reference = {41, 0};
    object->pixelWidth = 8;
    object->pixelHeight = 8;
    return PdfStatus::success();
  }
};

struct InterpreterHarness {
  std::array<uint8_t, 256> sourceBuffer{};
  std::array<PdfContentOperand, 16> operands{};
  std::array<PdfContentArrayItem, 8> arrayItems{};
  std::array<uint8_t, 64> scratchText{};
  std::array<uint8_t, 64> markedText{};
  std::array<uint8_t, 16> pageText{};
  std::array<PdfTextRun, 1> runs{};
  std::array<PdfImagePlacement, 8> images{};
  uint32_t documentOperatorCount = 0;
  PdfPageModel model;
  PdfContentInterpreter interpreter;

  InterpreterHarness()
      : model({pageText.data(), pageText.size(), runs.data(), static_cast<uint16_t>(runs.size()), images.data(),
               static_cast<uint16_t>(images.size())}),
        interpreter({sourceBuffer.data(), sourceBuffer.size(), operands.data(), static_cast<uint8_t>(operands.size()),
                     arrayItems.data(), static_cast<uint8_t>(arrayItems.size()), scratchText.data(),
                     static_cast<uint16_t>(scratchText.size()), markedText.data(),
                     static_cast<uint16_t>(markedText.size()), &documentOperatorCount}) {}
};

PdfStepResult run(PdfContentInterpreter& interpreter) {
  PdfStepResult result;
  do {
    PdfWorkBudget budget{4, 64};
    result = interpreter.step(budget);
  } while (result.yielded());
  return result;
}

TEST(PdfImagePlacement, CarriesNonstrokingMaskPaintLuminanceAndRestoresItAcrossGraphicsState) {
  static constexpr char content[] =
      "0.75 g /M Do "
      "q "
      "1 0 0 rg /M Do "
      "0 1 0 0 k /M Do "
      "0.5 sc /M Do "
      "0 0 1 scn /M Do "
      "Q "
      "/M Do";
  FixedSource fixed{reinterpret_cast<const uint8_t*>(content), sizeof(content) - 1U};
  const PdfByteSource source = fixed.source();
  ImageResource imageResource;
  InterpreterHarness harness;

  ASSERT_TRUE(harness.interpreter.begin(&source, 1, imageResource.resources, harness.model).ok());
  const PdfStepResult result = run(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(harness.model.imageCount(), 6U);
  constexpr std::array<uint8_t, 6> expectedLuminance{191, 77, 106, 128, 29, 191};
  for (uint16_t index = 0; index < expectedLuminance.size(); ++index) {
    EXPECT_EQ(harness.model.images()[index].reference.objectNumber, 41U);
    EXPECT_EQ(harness.model.images()[index].imageMaskPaintLuminance, expectedLuminance[index]);
  }
}

}  // namespace
