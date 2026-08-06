#include "ImageBlock.h"

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
#include <algorithm>
#include <cstring>
#include <limits>
#include <Memory.h>

#if defined(__GNUC__) || defined(__clang__)
#define PDF_IMAGE_BLOCK_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PDF_IMAGE_BLOCK_NOINLINE __declspec(noinline)
#else
#define PDF_IMAGE_BLOCK_NOINLINE
#endif
#else
#define PDF_IMAGE_BLOCK_NOINLINE
#endif

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdlib>
#include <utility>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height)
    : imagePath(std::move(imagePath)), sourcePath(std::move(sourcePath)), width(width), height(height) {}

void* ImageBlock::extractContext = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* context, ExtractFn fn) {
  extractContext = context;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
class PdfPixelCacheWorkspaceLease {
 public:
  PdfPixelCacheWorkspaceLease(const bool requested, PdfPixelCacheRenderWorkspace* const workspace) {
    if (requested && workspace != nullptr && !workspace->inUse) {
      workspace->inUse = true;
      workspace_ = workspace;
    }
  }

  ~PdfPixelCacheWorkspaceLease() {
    if (workspace_ != nullptr) {
      workspace_->inUse = false;
    }
  }

  uint8_t* data() const { return workspace_ == nullptr ? nullptr : workspace_->readBuffer; }
  size_t size() const { return workspace_ == nullptr ? 0 : sizeof(workspace_->readBuffer); }
  char* path() const { return workspace_ == nullptr ? nullptr : workspace_->path; }
  size_t pathSize() const { return workspace_ == nullptr ? 0 : sizeof(workspace_->path); }

  PdfPixelCacheWorkspaceLease(const PdfPixelCacheWorkspaceLease&) = delete;
  PdfPixelCacheWorkspaceLease& operator=(const PdfPixelCacheWorkspaceLease&) = delete;

 private:
  PdfPixelCacheRenderWorkspace* workspace_ = nullptr;
};
#endif

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
bool endsWith(const std::string& value, const char* const suffix) {
  const size_t suffixLength = std::strlen(suffix);
  return value.size() >= suffixLength &&
         std::memcmp(value.data() + value.size() - suffixLength, suffix, suffixLength) == 0;
}

bool consumeCanonicalDecimal(const std::string& path, size_t& offset, const uint64_t maximum) {
  if (offset >= path.size() || path[offset] < '0' || path[offset] > '9') {
    return false;
  }
  if (path[offset] == '0' && offset + 1U < path.size() && path[offset + 1U] >= '0' && path[offset + 1U] <= '9') {
    return false;
  }
  uint64_t value = 0;
  do {
    const uint8_t digit = static_cast<uint8_t>(path[offset] - '0');
    if (value > (maximum - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
    ++offset;
  } while (offset < path.size() && path[offset] >= '0' && path[offset] <= '9');
  return true;
}

bool isCanonicalLowerHex(const char* const bytes, const size_t length) {
  if (bytes == nullptr) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!((bytes[index] >= '0' && bytes[index] <= '9') || (bytes[index] >= 'a' && bytes[index] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool isCanonicalPdfImageLeaf(const char* const leaf, const size_t length) {
  constexpr size_t hashDigits = 16;
  constexpr size_t crcDigits = 8;
  constexpr size_t encodedLengthDigits = 16;
  constexpr char pixelSuffix[] = ".pxc";
  constexpr char jpegSuffix[] = ".jpg";
  constexpr size_t commonLength = hashDigits + 1U + crcDigits;
  if (leaf == nullptr || length < commonLength || leaf[hashDigits] != '-' ||
      !isCanonicalLowerHex(leaf, hashDigits) ||
      !isCanonicalLowerHex(leaf + hashDigits + 1U, crcDigits)) {
    return false;
  }
  if (length == commonLength + sizeof(pixelSuffix) - 1U) {
    return std::memcmp(leaf + commonLength, pixelSuffix, sizeof(pixelSuffix) - 1U) == 0;
  }
  return length == commonLength + 1U + encodedLengthDigits + sizeof(jpegSuffix) - 1U &&
         leaf[commonLength] == '-' &&
         isCanonicalLowerHex(leaf + commonLength + 1U, encodedLengthDigits) &&
         std::memcmp(leaf + commonLength + 1U + encodedLengthDigits, jpegSuffix,
                     sizeof(jpegSuffix) - 1U) == 0;
}

PDF_IMAGE_BLOCK_NOINLINE bool isCanonicalPdfCachedImagePath(const std::string& path) {
  constexpr char root[] = "/.crosspoint/pdf_";
  constexpr char generation[] = "/gen_";
  constexpr char images[] = "/images/";
  if (path.size() <= sizeof(root) - 1U || std::memcmp(path.data(), root, sizeof(root) - 1U) != 0) {
    return false;
  }
  size_t offset = sizeof(root) - 1U;
  if (!consumeCanonicalDecimal(path, offset, std::numeric_limits<uint64_t>::max()) ||
      offset + sizeof(generation) - 1U >= path.size() ||
      std::memcmp(path.data() + offset, generation, sizeof(generation) - 1U) != 0) {
    return false;
  }
  offset += sizeof(generation) - 1U;
  if (!consumeCanonicalDecimal(path, offset, std::numeric_limits<uint32_t>::max()) ||
      offset + sizeof(images) - 1U >= path.size() ||
      std::memcmp(path.data() + offset, images, sizeof(images) - 1U) != 0) {
    return false;
  }
  offset += sizeof(images) - 1U;
  return isCanonicalPdfImageLeaf(path.data() + offset, path.size() - offset);
}

PDF_IMAGE_BLOCK_NOINLINE const char* getPdfPixelCachePath(const std::string& imagePath,
                                                          const PdfPixelCacheWorkspaceLease& workspace,
                                                          std::string& fallbackPath) {
  if (endsWith(imagePath, ".pxc")) {
    return imagePath.c_str();
  }
  char* const path = workspace.path();
  const size_t pathBytes = workspace.pathSize();
  if (path == nullptr) {
    fallbackPath = getCachePath(imagePath);
    return fallbackPath.c_str();
  }
  const size_t dot = imagePath.rfind('.');
  const size_t stemLength = dot == std::string::npos ? imagePath.size() : dot;
  constexpr char suffix[] = ".pxc";
  if (stemLength + sizeof(suffix) > pathBytes) {
    return nullptr;
  }
  std::memcpy(path, imagePath.data(), stemLength);
  std::memcpy(path + stemLength, suffix, sizeof(suffix));
  return path;
}
#endif

void clampCachedRowsToLandscapeStrip(const GfxRenderer& renderer, const int imageY, int& rowStart, int& rowEnd) {
  if (!renderer.isStripTargetActive()) {
    return;
  }

  const int stripY0 = renderer.getWriteOriginY();
  const int stripY1Exclusive = stripY0 + renderer.getWriteRows();
  int logicalY0;
  int logicalY1Exclusive;

  switch (renderer.getOrientation()) {
    case GfxRenderer::LandscapeCounterClockwise:
      logicalY0 = stripY0;
      logicalY1Exclusive = stripY1Exclusive;
      break;
    case GfxRenderer::LandscapeClockwise:
      logicalY0 = renderer.getDisplayHeight() - stripY1Exclusive;
      logicalY1Exclusive = renderer.getDisplayHeight() - stripY0;
      break;
    default:
      return;
  }

  const int stripRowStart = logicalY0 - imageY;
  const int stripRowEnd = logicalY1Exclusive - imageY;
  if (rowStart < stripRowStart) rowStart = stripRowStart;
  if (rowEnd > stripRowEnd) rowEnd = stripRowEnd;
}

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
PDF_IMAGE_BLOCK_NOINLINE bool renderScaledPdfCache(GfxRenderer& renderer, FsFile& cacheFile,
                                                   const uint16_t cachedWidth, const uint16_t cachedHeight,
                                                   const int x, const int y, const int outputWidth,
                                                   const int outputHeight, uint8_t* const readBuffer,
                                                   const size_t readBufferBytes) {
  if (outputWidth <= 0 || outputHeight <= 0 || cachedWidth == 0 || cachedHeight == 0) {
    return false;
  }
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = std::max(0, -x);
  int clipYStart = std::max(0, -y);
  int clipXEnd = std::min<int>(outputWidth, screenWidth - x);
  int clipYEnd = std::min<int>(outputHeight, screenHeight - y);
  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    return true;
  }
  clampCachedRowsToLandscapeStrip(renderer, y, clipYStart, clipYEnd);
  if (clipYStart >= clipYEnd) {
    return true;
  }

  const size_t bytesPerRow = (static_cast<size_t>(cachedWidth) + 3U) / 4U;
  if (readBuffer == nullptr || bytesPerRow == 0 || bytesPerRow > readBufferBytes) {
    return false;
  }
  const int rowsPerRead = std::max<int>(1, static_cast<int>(readBufferBytes / bytesPerRow));
  DirectPixelWriter pixelWriter;
  pixelWriter.init(renderer);
  int bufferedSourceStart = -1;
  int bufferedSourceRows = 0;

  for (int destinationRow = clipYStart; destinationRow < clipYEnd; ++destinationRow) {
    const int sourceRow =
        std::min<int>(cachedHeight - 1,
                      static_cast<int>((static_cast<uint32_t>(destinationRow) * cachedHeight) / outputHeight));
    if (sourceRow < bufferedSourceStart || sourceRow >= bufferedSourceStart + bufferedSourceRows) {
      bufferedSourceStart = sourceRow;
      bufferedSourceRows = std::min<int>(rowsPerRead, cachedHeight - sourceRow);
      const size_t sourceOffset = 4U + static_cast<size_t>(sourceRow) * bytesPerRow;
      const size_t bytes = static_cast<size_t>(bufferedSourceRows) * bytesPerRow;
      if (!cacheFile.seek(sourceOffset) ||
          cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        return false;
      }
    }
    const uint8_t* const source =
        readBuffer + static_cast<size_t>(sourceRow - bufferedSourceStart) * bytesPerRow;
    pixelWriter.beginRow(y + destinationRow);
    for (int destinationColumn = clipXStart; destinationColumn < clipXEnd; ++destinationColumn) {
      const int sourceColumn =
          std::min<int>(cachedWidth - 1,
                        static_cast<int>((static_cast<uint32_t>(destinationColumn) * cachedWidth) / outputWidth));
      const int byteIndex = sourceColumn >> 2;
      const int bitShift = 6 - (sourceColumn & 3) * 2;
      pixelWriter.writePixel(x + destinationColumn,
                             static_cast<uint8_t>((source[byteIndex] >> bitShift) & 0x03U));
    }
  }
  return true;
}

PDF_IMAGE_BLOCK_NOINLINE bool renderPdfFromCache(GfxRenderer& renderer, const char* const cachePath, int x, int y,
                                                 int expectedWidth, int expectedHeight,
                                                 const PdfPixelCacheWorkspaceLease& pdfWorkspace) {
  if (cachePath == nullptr || cachePath[0] == '\0') {
    return false;
  }
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    auto ownedScaledBuffer = std::unique_ptr<uint8_t[]>{};
    uint8_t* scaledBuffer = pdfWorkspace.data();
    size_t scaledBufferBytes = pdfWorkspace.size();
    if (scaledBuffer == nullptr) {
      ownedScaledBuffer = makeUniqueNoThrow<uint8_t[]>(PdfPixelCacheRenderWorkspace::READ_BUFFER_BYTES);
      scaledBuffer = ownedScaledBuffer.get();
      scaledBufferBytes = scaledBuffer == nullptr ? 0 : PdfPixelCacheRenderWorkspace::READ_BUFFER_BYTES;
    }
    const bool rendered = renderScaledPdfCache(renderer, cacheFile, cachedWidth, cachedHeight, x, y, expectedWidth,
                                               expectedHeight, scaledBuffer, scaledBufferBytes);
    cacheFile.close();
    return rendered;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath, cachedWidth, cachedHeight);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = 0;
  int clipYStart = 0;
  int clipXEnd = cachedWidth;
  int clipYEnd = cachedHeight;
  if (x < 0) clipXStart = -x;
  if (y < 0) clipYStart = -y;
  if (screenWidth - x < clipXEnd) clipXEnd = screenWidth - x;
  if (screenHeight - y < clipYEnd) clipYEnd = screenHeight - y;

  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    LOG_DBG("IMG", "Cached image is outside screen after clipping");
    cacheFile.close();
    return true;
  }

  int renderRowStart = clipYStart;
  int renderRowEnd = clipYEnd;
  clampCachedRowsToLandscapeStrip(renderer, y, renderRowStart, renderRowEnd);
  if (renderRowStart >= renderRowEnd) {
    cacheFile.close();
    return true;
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const int rowsToRender = renderRowEnd - renderRowStart;
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  auto ownedReadBuffer = std::unique_ptr<uint8_t[]>{};
  uint8_t* readBuffer = nullptr;
  if (pdfWorkspace.data() != nullptr && static_cast<size_t>(bytesPerRow) <= pdfWorkspace.size()) {
    rowsPerRead = std::min<int>(rowsToRender, static_cast<int>(pdfWorkspace.size() / bytesPerRow));
    readBuffer = pdfWorkspace.data();
  } else {
    ownedReadBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
    readBuffer = ownedReadBuffer.get();
    if (!readBuffer) {
      // Fall back to a single-row buffer under memory pressure.
      rowsPerRead = 1;
      ownedReadBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
      readBuffer = ownedReadBuffer.get();
    }
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  const size_t dataOffset = 4U + static_cast<size_t>(renderRowStart) * static_cast<size_t>(bytesPerRow);
  if (!cacheFile.seek(dataOffset)) {
    LOG_ERR("IMG", "Cache seek error at row %d", renderRowStart);
    cacheFile.close();
    return false;
  }

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = renderRowStart; row < renderRowEnd; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (renderRowEnd - row < rowsPerRead) ? (renderRowEnd - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    if (row < clipYStart) continue;
    if (row >= clipYEnd) break;

    const int destY = y + row;
    pw.beginRow(destY);
    // Walk only the on-screen columns: writePixel drops off-band rows but does
    // not clip X, so this range is what keeps a partially off-screen image
    // inside the framebuffer.
    for (int col = clipXStart; col < clipXEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}
#endif

PDF_IMAGE_BLOCK_NOINLINE bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y,
                                              int expectedWidth, int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = 0;
  int clipYStart = 0;
  int clipXEnd = cachedWidth;
  int clipYEnd = cachedHeight;
  if (x < 0) clipXStart = -x;
  if (y < 0) clipYStart = -y;
  if (screenWidth - x < clipXEnd) clipXEnd = screenWidth - x;
  if (screenHeight - y < clipYEnd) clipYEnd = screenHeight - y;

  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    LOG_DBG("IMG", "Cached image is outside screen after clipping");
    cacheFile.close();
    return true;
  }

  int renderRowStart = clipYStart;
  int renderRowEnd = clipYEnd;
  clampCachedRowsToLandscapeStrip(renderer, y, renderRowStart, renderRowEnd);
  if (renderRowStart >= renderRowEnd) {
    cacheFile.close();
    return true;
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const int rowsToRender = renderRowEnd - renderRowStart;
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  const size_t dataOffset = 4U + static_cast<size_t>(renderRowStart) * static_cast<size_t>(bytesPerRow);
  if (!cacheFile.seek(dataOffset)) {
    LOG_ERR("IMG", "Cache seek error at row %d", renderRowStart);
    free(readBuffer);
    cacheFile.close();
    return false;
  }

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = renderRowStart; row < renderRowEnd; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (renderRowEnd - row < rowsPerRead) ? (renderRowEnd - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    if (row < clipYStart) continue;
    if (row >= clipYEnd) break;

    const int destY = y + row;
    pw.beginRow(destY);
    // Walk only the on-screen columns: writePixel drops off-band rows but does
    // not clip X, so this range is what keeps a partially off-screen image
    // inside the framebuffer.
    for (int col = clipXStart; col < clipXEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  cacheFile.close();
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) { render(renderer, x, y, nullptr); }

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y,
                        PdfPixelCacheRenderWorkspace* const pdfWorkspace) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image size: %dx%d", width, height);
    return;
  }

  // Reject only fully off-screen images. Decoders and cache rendering clip
  // partially visible images to the logical screen bounds.
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }
  const bool fullyOnScreen = x >= 0 && y >= 0 && x + width <= screenWidth && y + height <= screenHeight;

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  // Try to render from cache first
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  const bool pdfCachedImage = isCanonicalPdfCachedImagePath(imagePath);
  // The lease spans path derivation, cache reads, and first-time JPEG decode so
  // a nested render cannot overwrite either shared scratch region.
  PdfPixelCacheWorkspaceLease workspaceLease(pdfCachedImage, pdfWorkspace);
  std::string legacyCachePath;
  const char* cachePath = nullptr;
  if (pdfCachedImage) {
    cachePath = getPdfPixelCachePath(imagePath, workspaceLease, legacyCachePath);
    if (renderPdfFromCache(renderer, cachePath, x, y, width, height, workspaceLease)) {
      return;  // Successfully rendered from the immutable PDF cache
    }
  } else {
    legacyCachePath = getCachePath(imagePath);
    cachePath = legacyCachePath.c_str();
    if (renderFromCache(renderer, legacyCachePath, x, y, width, height)) {
      return;  // Successfully rendered from cache
    }
  }
#else
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }
#endif

  if (!sourcePath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    if (!extractFn(extractContext, sourcePath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", sourcePath.c_str());
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  if (fullyOnScreen && cachePath != nullptr) {
    config.cachePath = cachePath;  // Enable caching during decode
  }
#else
  if (fullyOnScreen) {
    config.cachePath = cachePath;  // Enable caching during decode
  }
#endif

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y, foregroundBlack);
    return;
  }
}

bool ImageBlock::serialize(FsFile& file) {
  return serialization::tryWriteString(file, imagePath) && serialization::tryWriteString(file, sourcePath) &&
         serialization::tryWritePod(file, width) && serialization::tryWritePod(file, height);
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  if (!serialization::tryReadString(file, path)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image path");
    return nullptr;
  }
  std::string source;
  if (!serialization::tryReadString(file, source)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image source path");
    return nullptr;
  }
  int16_t w, h;
  if (!serialization::tryReadPod(file, w) || !serialization::tryReadPod(file, h)) {
    LOG_ERR("IMG", "Deserialization failed: truncated image metadata");
    return nullptr;
  }

  auto* imageBlock = new (std::nothrow) ImageBlock(std::move(path), std::move(source), w, h);
  if (!imageBlock) {
    LOG_ERR("IMG", "Deserialization failed: could not allocate ImageBlock");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(imageBlock);
}
