import hashlib
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
QEMU_SOURCE = REPO_ROOT / "src" / "qemu" / "QemuAcceptance.cpp"
FRAMEBUFFER_GUARD_HEADER = REPO_ROOT / "src" / "PdfAcceptanceFramebufferGuard.h"
EPUB_SOURCE = REPO_ROOT / "lib" / "Epub" / "Epub.cpp"
ZIP_SOURCE = REPO_ROOT / "lib" / "ZipFile" / "ZipFile.cpp"
QEMU_DATA = REPO_ROOT / "test" / "qemu" / "data" / "qemu"
PDF_FIXTURES = REPO_ROOT / "test" / "pdf_reflow_core" / "fixtures"
EPUB_FIXTURES = REPO_ROOT / "test" / "epubs"

PREPARED_TYPOGRAPHY_XHTML = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<html xmlns="http://www.w3.org/1999/xhtml">'
    '<head><meta charset="UTF-8"/></head><body>'
    '<span id="p00000000" role="doc-pagebreak" aria-label="1"></span>'
    '<p id="b00000000">Typography uses device defaults.</p>'
    '</body></html>'
)
RAW_FIXTURE_SOURCES = {
    "classic_text.pdf": PDF_FIXTURES / "classic_text.pdf",
    "classic_text_fresh.pdf": PDF_FIXTURES / "classic_text.pdf",
    "font_size_6.pdf": PDF_FIXTURES / "font_size_6.pdf",
    "font_size_72.pdf": PDF_FIXTURES / "font_size_72.pdf",
    "navigation_outline.pdf": PDF_FIXTURES / "navigation_outline.pdf",
    "raster_cover_caption.pdf": PDF_FIXTURES / "raster_cover_caption.pdf",
    "hidden_ocr.pdf": PDF_FIXTURES / "hidden_ocr.pdf",
    "columns_table.pdf": PDF_FIXTURES / "columns_table.pdf",
    "jpeg_caption.pdf": PDF_FIXTURES / "jpeg_caption.pdf",
    "bad_startxref.pdf": PDF_FIXTURES / "bad_startxref.pdf",
    "oversized_length.pdf": PDF_FIXTURES / "oversized_length.pdf",
    "encrypted.pdf": PDF_FIXTURES / "encrypted.pdf",
    "lzw_required.pdf": PDF_FIXTURES / "lzw_required.pdf",
    "scan_only.pdf": PDF_FIXTURES / "scan_only.pdf",
    "flate_bomb.pdf": PDF_FIXTURES / "flate_bomb.pdf",
    "epub_oracle.epub": EPUB_FIXTURES / "test_synthetic_unicode_glyphs.epub",
}

ORDERED_STAGE_MARKERS = (
    "QEMU_PDF_RAW_PASS",
    "QEMU_PDF_CANCEL_PASS",
    "QEMU_PDF_RESUME_PASS",
    "QEMU_PDF_TYPOGRAPHY_PASS",
    "QEMU_PDF_NAV_PASS",
    "QEMU_PDF_IMAGE_PASS",
    "QEMU_PDF_POSITIVE_PASS",
    "QEMU_PDF_PROGRESS_PASS",
    "QEMU_PDF_CACHE_REOPEN_PASS",
    "QEMU_PDF_NEGATIVE_PASS",
    "QEMU_EPUB_ORACLE_PASS",
    "QEMU_PDF_TRACER_PASS",
    "QEMU_TRACER_PASS",
    "QEMU_TEST_PASS",
)

BOOT_TWO_ACCEPTANCE_STAGES = (
    (
        "BootTwoResume",
        "checkPdfResume(renderer, persistentAcceptance)",
        "BootTwoTypography",
    ),
    ("BootTwoTypography", "checkPdfTypography(renderer)", "BootTwoNavigation"),
    ("BootTwoNavigation", "checkPdfFullNavigation(renderer)", "BootTwoImage"),
    ("BootTwoImage", "checkPdfImage(renderer)", "BootTwoPositiveCorpus"),
    (
        "BootTwoPositiveCorpus",
        "checkPdfPositiveCorpus(renderer)",
        "BootTwoProgress",
    ),
    (
        "BootTwoProgress",
        "checkPdfProgressAndSavedItems(renderer)",
        "BootTwoCacheReopen",
    ),
    (
        "BootTwoCacheReopen",
        "checkPdfCompletedCacheReopen(renderer)",
        "BootTwoNegativeCorpus",
    ),
    (
        "BootTwoNegativeCorpus",
        "checkPdfNegativeCorpus(renderer)",
        "BootTwoEpub",
    ),
    ("BootTwoEpub", "checkEpubOracle(renderer)", "BootTwoProductTracer"),
    (
        "BootTwoProductTracer",
        "checkPdfProductTracer(renderer)",
        "BootTwoStorage",
    ),
    ("BootTwoStorage", "checkStorage()", "BootTwoFrame"),
    ("BootTwoFrame", "checkFrame()", "BootTwoInput"),
    ("BootTwoInput", "checkInput(input)", "WaitingForPowerSaving"),
)

def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fnv64(value: bytes) -> int:
    result = 0xCBF29CE484222325
    for byte in value:
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return result


def normalize_semantic_text(xhtml: bytes) -> bytes:
    normalized = bytearray()
    inside_tag = False
    inside_entity = False
    pending_space = False
    for value in xhtml:
        if inside_entity:
            normalized.append(value)
            inside_entity = value != ord(";")
            continue
        if value == ord("<"):
            inside_tag = True
            pending_space = bool(normalized)
            continue
        if inside_tag:
            inside_tag = value != ord(">")
            continue
        if value in b" \t\r\n\f":
            pending_space = bool(normalized)
            continue
        if pending_space:
            normalized.append(ord(" "))
            pending_space = False
        normalized.append(value)
        inside_entity = value == ord("&")
    return bytes(normalized)


def function_body(source: str, function_name: str) -> str:
    signature = source.index(f"{function_name}(")
    opening = source.index("{", signature)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : offset]
    raise AssertionError(f"unterminated function {function_name}")


def source_between_functions(source: str, function_name: str, next_function: str) -> str:
    start = source.index(f"{function_name}(")
    end = source.index(f"{next_function}(", start + len(function_name) + 1)
    return source[start:end]


def replace_between_functions(
    source: str,
    function_name: str,
    next_function: str,
    old: str,
    new: str,
) -> str:
    region = source_between_functions(source, function_name, next_function)
    if old not in region:
        raise AssertionError(f"missing mutation target in {function_name}: {old!r}")
    return source.replace(region, region.replace(old, new, 1), 1)


def move_cancellation_yield_before_measurement(source: str) -> str:
    body = source_between_functions(
        source, "checkPdfCancellation", "workCountersResumeLess"
    )
    step_then_measure = (
        "    result = stepTrackedPdfPreparation(*preparation);\n"
        "    const uint32_t elapsed = millis() - startedAt;"
    )
    step_then_yield_then_measure = (
        "    result = stepTrackedPdfPreparation(*preparation);\n"
        "    yieldAfterPdfPreparationStep(result);\n"
        "    const uint32_t elapsed = millis() - startedAt;"
    )
    branch_yield = "      yieldAfterPdfPreparationStep(result);\n"
    if step_then_measure not in body or branch_yield not in body:
        raise AssertionError("missing timed cancellation yield mutation target")
    body = body.replace(step_then_measure, step_then_yield_then_measure, 1)
    body = body.replace(branch_yield, "", 1)
    return source.replace(
        source_between_functions(
            source, "checkPdfCancellation", "workCountersResumeLess"
        ),
        body,
        1,
    )


def preparation_scheduler_failures(source: str) -> list[str]:
    failures: list[str] = []
    helper_name = "yieldAfterPdfPreparationStep"
    helper_call = "yieldAfterPdfPreparationStep(result);"
    if f"void {helper_name}(" not in source:
        return ["QEMU PDF preparation scheduler helper"]

    helper = source_between_functions(source, helper_name, "runPreparation")
    tracked_step = source_between_functions(
        source, "stepTrackedPdfPreparation", helper_name
    )
    run = source_between_functions(source, "runPreparation", "preparePdf")
    cancellation = source_between_functions(
        source, "checkPdfCancellation", "workCountersResumeLess"
    )
    forced_oom = source_between_functions(
        source, "checkPdfForcedOom", "checkPdfNegativeCorpus"
    )
    product_tracer = source_between_functions(
        source, "checkPdfProductTracer", "checkPdfNavigation"
    )
    navigation = source_between_functions(
        source, "checkPdfNavigation", "checkStorageOpenParity"
    )

    if "if (result.yielded()) {\n    yield();\n  }" not in helper:
        failures.append("yield only after yielded PDF preparation result")
    if source.count("yield();") != 1 or source.count(helper_call) != 5:
        failures.append("exact QEMU PDF preparation scheduler coverage")
    if helper_call in tracked_step or "yield();" in tracked_step:
        failures.append("no scheduler yield inside tracked PDF step interval")
    if helper_call in forced_oom:
        failures.append("no scheduler yield after terminal forced-OOM probe")
    if source.count("stepTrackedPdfPreparation(") != 7:
        failures.append("complete PdfPreparation step-site inventory")

    if (
        run.count(helper_call) != 1
        or "sampleRuntime();\n    yieldAfterPdfPreparationStep(result);\n"
        "    if (!result.yielded())" not in run
    ):
        failures.append("runPreparation post-accounting scheduler yield")

    setup_block = (
        "    ++setupSteps;\n"
        "    sampleRuntime();\n"
        "    yieldAfterPdfPreparationStep(result);\n"
        "  }\n"
        "  if (!status || !result.yielded()"
    )
    timed_branch = (
        "    if (result.yielded()) {\n"
        "      yieldAfterPdfPreparationStep(result);\n"
        "      continue;\n"
        "    }"
    )
    if cancellation.count(helper_call) != 2 or setup_block not in cancellation:
        failures.append("cancellation setup post-accounting scheduler yield")
    if timed_branch not in cancellation:
        failures.append("timed cancellation scheduler continuation")
    else:
        timed_step = cancellation.index(
            "result = stepTrackedPdfPreparation(*preparation);",
            cancellation.index("for (uint16_t slice"),
        )
        yielded_branch = cancellation.index("if (result.yielded()) {", timed_step)
        timed_yield = cancellation.index(helper_call, yielded_branch)
        continue_offset = cancellation.index("continue;", timed_yield)
        accounting_tokens = (
            "const uint32_t elapsed = millis() - startedAt;",
            "const uint32_t elapsedUs = micros() - startedAtUs;",
            "const TracedPdfCacheIo::SliceTrace sliceTrace = traced.sliceTrace();",
            "++persistent->cancellationSlices;",
            "sampleRuntime();",
            "const bool timingExceeded =",
            "const bool otherLimitExceeded =",
            "if (timingViolation || otherLimitExceeded)",
        )
        if not (
            timed_step
            < min(cancellation.index(token, timed_step) for token in accounting_tokens)
            and max(cancellation.index(token, timed_step) for token in accounting_tokens)
            < yielded_branch
            < timed_yield
            < continue_offset
        ):
            failures.append("timed cancellation yield after slice accounting")

    loop_block = (
        "    result = stepTrackedPdfPreparation(*preparation);\n"
        "    sampleRuntime();\n"
        "    yieldAfterPdfPreparationStep(result);\n"
        "  }"
    )
    if product_tracer.count(helper_call) != 1 or loop_block not in product_tracer:
        failures.append("product tracer post-accounting scheduler yield")
    if navigation.count(helper_call) != 1 or loop_block not in navigation:
        failures.append("navigation post-accounting scheduler yield")
    return failures


def boot_two_scheduler_failures(source: str) -> list[str]:
    failures: list[str] = []
    begin = function_body(source, "qemuAcceptanceBegin")
    tick = function_body(source, "qemuAcceptanceTick")

    if "persistentAcceptance = persistent;" not in begin:
        failures.append("boot-two resume expectation retention")
    if "state.phase = AcceptancePhase::BootTwoResume;" not in begin:
        failures.append("boot-two scheduler entry phase")
    for _, invocation, _ in BOOT_TWO_ACCEPTANCE_STAGES:
        if invocation == "checkStorage()":
            if begin.count(invocation) != 1:
                failures.append("boot-zero storage prerequisite only")
            continue
        if invocation in begin:
            failures.append(f"boot-two group ran synchronously in begin: {invocation}")

    phase_names = tuple(stage[0] for stage in BOOT_TWO_ACCEPTANCE_STAGES)
    for index, (phase, invocation, next_phase) in enumerate(
        BOOT_TWO_ACCEPTANCE_STAGES
    ):
        case_token = f"case AcceptancePhase::{phase}:"
        if case_token not in tick:
            failures.append(f"boot-two scheduler case: {phase}")
            continue
        start = tick.index(case_token)
        later_cases = [
            tick.find(f"case AcceptancePhase::{name}:", start + len(case_token))
            for name in phase_names[index + 1 :]
        ]
        later_cases = [offset for offset in later_cases if offset >= 0]
        waiting_case = tick.find(
            "case AcceptancePhase::WaitingForPowerSaving:",
            start + len(case_token),
        )
        if waiting_case >= 0:
            later_cases.append(waiting_case)
        end = min(later_cases) if later_cases else len(tick)
        region = tick[start:end]
        expected_transition = f"state.phase = AcceptancePhase::{next_phase};"
        if region.count(invocation) != 1:
            failures.append(f"one boot-two group invocation: {phase}")
        if region.count(expected_transition) != 1:
            failures.append(f"boot-two next phase: {phase}")
        if region.count("return;") != 1:
            failures.append(f"boot-two main-loop return boundary: {phase}")
        other_invocations = tuple(
            other
            for _, other, _ in BOOT_TWO_ACCEPTANCE_STAGES
            if other != invocation
        )
        if any(other in region for other in other_invocations):
            failures.append(f"multiple boot-two groups in one tick: {phase}")

    if "delay(" in tick or "yield();" in tick or "watchdog" in tick.lower():
        failures.append("boot-two scheduler uses normal loop boundary only")
    return failures


def epub_platform_smoke_failures(source: str) -> list[str]:
    failures: list[str] = []
    run_pass = function_body(source, "runEpubOraclePass")
    check = function_body(source, "checkEpubOracle")
    progress = (
        function_body(source, "verifyEpubProgressAndBookmark")
        if "verifyEpubProgressAndBookmark(" in source
        else ""
    )
    residue = (
        function_body(source, "epubCacheResidueAbsent")
        if "epubCacheResidueAbsent(" in source
        else ""
    )

    for token in (
        "constexpr uint32_t kExpectedEpubSourceBytes = 3616;",
        "constexpr uint64_t kExpectedEpubSourceHash = 0x92563E7E3D33C382ULL;",
        "Test: Synthetic Unicode Glyphs",
        "CrossPoint Test Fixture",
        '"OEBPS/chapter1.xhtml"',
        '"OEBPS/chapter2.xhtml"',
        "1670",
        "1920",
        "3590",
        '"Glyph Reference"',
        '"Block Stress Cases"',
        '"OEBPS/styles/test.css"',
        "0x46385061C46C2FE4ULL",
        "0x4060CB229041492DULL",
        "0x3CC3246E367521F4ULL",
    ):
        if token not in source:
            failures.append(f"source-locked EPUB fixture: {token}")
    if "epub->load(!cached, true)" not in run_pass:
        failures.append("EPUB metadata load must skip CSS and forbid cached rebuild")
    if (
        "EpubEmbeddedStyleScope embeddedStyleScope;" not in run_pass
        or "SETTINGS.embeddedStyle = 0;" not in source
        or "SETTINGS.embeddedStyle = saved_;" not in source
    ):
        failures.append("QEMU-only embedded style save and restore")
    for token in (
        "kExpectedEpubRepresentativePages",
        "kExpectedEpubSectionCacheHash",
        "kExpectedEpubFrameHash",
        "section->pageCount != kExpectedEpubRepresentativePages",
        "oracle->cache != kExpectedEpubSectionCacheHash",
        "oracle->frame != kExpectedEpubFrameHash",
        "loadOrCreateSection(epub, renderer, layout, 1, \"\", cached, &section)",
        "epub->streamSection(sectionIndex, stream, 1024)",
        "epub->streamResource(0, kEpubCssResource, css, 256)",
    ):
        if token not in run_pass and token not in source:
            failures.append(f"bounded EPUB render pin: {token}")
    if (
        "std::memcmp(&uncached, &cached, sizeof(uncached)) != 0" not in check
        or "verifyEpubProgressAndBookmark" not in check
        or "epubCacheResidueAbsent" not in check
    ):
        failures.append("cached equality, progress, and residue gates")
    for token in (
        "EpubReaderUtils::saveProgress",
        "loadReadingPosition",
        "EpubReaderUtils::loadProgress",
        "BOOKMARKS.addBookmark",
        "BOOKMARKS.unload();",
        "BOOKMARKS.loadForBook",
        "BOOKMARKS.loadForBook(reopened->getPath()",
        'std::strcmp(bookmark.chapterTitle, "Block Stress Cases")',
        'std::strcmp(bookmark.snippet, "qemu-epub-smoke")',
    ):
        if token not in progress:
            failures.append(f"EPUB progress/bookmark reopen: {token}")
    for token in (
        '"/css_rules.cache"',
        '"/css_rules.cache.tmp"',
        '"/css_rules.cache.bak"',
        '"/sections/1.bin.pwi"',
        '"/sections/1.bin.pwi.tmp"',
        '"/sections/1.bin.tmp"',
        '"/sections/1.bin.bak"',
        '"/html/.tmp_1.html"',
        '"/progress.bin.tmp"',
        '"/progress.bin.bak"',
        '"/spine.bin.tmp"',
        '"/toc.bin.tmp"',
    ):
        if token not in residue:
            failures.append(f"EPUB forbidden residue: {token}")
    return failures


def epub_oracle_marker_format_failures(source: str) -> list[str]:
    failures: list[str] = []
    oracle = function_body(source, "checkEpubOracle")
    for field in ("xhtml0", "xhtml1", "css", "cache", "frame"):
        if oracle.count(f"{field}=%08lX%08lX") != 2:
            failures.append(f"split 64-bit EPUB marker field: {field}")
    if "%llX" in oracle or "%016llX" in oracle:
        failures.append("ROM-unsafe 64-bit EPUB marker format")
    for pass_name in ("uncached", "cached"):
        if oracle.count(f"QEMU_EPUB_ORACLE_PASS pass={pass_name}") != 1:
            failures.append(f"EPUB marker pass coverage: {pass_name}")
        for field in ("xhtml0", "xhtml1", "css", "cache", "frame"):
            high_low = (
                f"static_cast<unsigned long>({pass_name}.{field} >> 32U), "
                f"static_cast<unsigned long>({pass_name}.{field})"
            )
            if oracle.count(high_low) != 1:
                failures.append(f"EPUB marker high-low order: {pass_name}.{field}")
    return failures


def pdf_image_section_lifetime_failures(source: str) -> list[str]:
    failures: list[str] = []
    image = function_body(source, "checkPdfImage")
    rendered = "!renderSectionPage(*withImage, 0, renderer, layout, &imageFrame)"
    released = "withImage.reset();"
    suppressed_section = "std::unique_ptr<Section> withoutImage;"
    for marker in (rendered, released, suppressed_section):
        if image.count(marker) != 1:
            failures.append(f"PDF image lifetime marker: {marker}")
    if failures:
        return failures

    rendered_offset = image.index(rendered)
    released_offset = image.index(released)
    suppressed_offset = image.index(suppressed_section)
    if not rendered_offset < released_offset < suppressed_offset:
        failures.append("PDF image section release order")
    if "withImage" in image[released_offset + len(released) :]:
        failures.append("PDF image section use after release")
    return failures


def positive_pdf_corpus_failures(source: str) -> list[str]:
    failures: list[str] = []
    check = function_body(source, "checkPdfPositiveCorpus")
    source_tokens = (
        'constexpr char PDF_OCR_FIXTURE_PATH[] = "/qemu/hidden_ocr.pdf";',
        'constexpr char PDF_COLUMNS_TABLE_FIXTURE_PATH[] = "/qemu/columns_table.pdf";',
        'constexpr char PDF_JPEG_FIXTURE_PATH[] = "/qemu/jpeg_caption.pdf";',
        "constexpr uint64_t kExpectedPdfOcrSemanticHash = 0xDFAE2740CD6F6513ULL;",
        "constexpr uint64_t kExpectedPdfColumnsSemanticHash = 0x715E72B598FFFFE3ULL;",
        "constexpr uint64_t kExpectedPdfTableSemanticHash = 0x4BD86B77E1579064ULL;",
        "constexpr uint64_t kExpectedPdfJpegSemanticHash = 0xE72D737B2BF7D6CFULL;",
        'constexpr char kExpectedPdfColumnsText[] = "Left one. Left two. Right one. Right two.";',
        'constexpr char kExpectedPdfTableText[] = "Name Value Alpha 10";',
    )
    for token in source_tokens:
        if token not in source:
            failures.append(f"positive corpus source pin: {token}")

    checks = (
        "ocrSemantic.hash() != kExpectedPdfOcrSemanticHash",
        "ocr->getTotalWordCount() != 4",
        "columnsHash != kExpectedPdfColumnsSemanticHash",
        "columns->getTotalWordCount() != 12",
        "tableHash != kExpectedPdfTableSemanticHash",
        "jpegSemantic.hash() != kExpectedPdfJpegSemanticHash",
        "jpeg->getTotalWordCount() != 2",
        "jpegContent.firstCount() != 1",
        "jpegFrame == 0",
        "jpegFrame == blankFrame",
    )
    for token in checks:
        if token not in check:
            failures.append(f"positive corpus live witness: {token}")

    failure_codes = {
        'fail("PDF_CORPUS_OCR",': 2,
        'fail("PDF_CORPUS_COLUMNS",': 3,
        'fail("PDF_CORPUS_TABLE",': 2,
        'fail("PDF_CORPUS_JPEG",': 4,
    }
    for code, expected_count in failure_codes.items():
        if check.count(code) != expected_count:
            failures.append(f"positive corpus distinct failure code: {code}")

    marker_fields = (
        "QEMU_PDF_POSITIVE_PASS ocr=%08lX%08lX ocr_words=%lu ",
        "columns=%08lX%08lX columns_words=%u table=%08lX%08lX table_words=%u ",
        "jpeg=%08lX%08lX jpeg_words=%lu retained=%u decoded=1 frame=%08lX blank=%08lX",
    )
    for field in marker_fields:
        if field not in check:
            failures.append(f"positive corpus marker field: {field}")
    if "%016llX" in check:
        failures.append("positive corpus marker uses ROM-unsafe 64-bit formatting")

    jpeg_diagnostic_fields = (
        "QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC document=%u layout=%u sections=%ld ",
        "semantic_stream=%u content_stream=%u words=%lu ",
        "semantic=%08lX%08lX expected=%08lX%08lX semantic_bytes=%lu ",
        "image_tags=%u captions=%u",
    )
    for field in jpeg_diagnostic_fields:
        if field not in check:
            failures.append(f"JPEG retained diagnostic field: {field}")
    diagnostic_offset = check.find("QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC")
    retained_failure_offset = check.find('fail("PDF_CORPUS_JPEG", "retained")')
    if not 0 <= diagnostic_offset < retained_failure_offset:
        failures.append("JPEG retained diagnostic order")
    return failures


def acceptance_source_failures(source: str) -> list[str]:
    failures: list[str] = []
    begin = function_body(source, "beginTrackedPdfPreparation")
    step = function_body(source, "stepTrackedPdfPreparation")
    resource = function_body(source, "recordPdfResourceEvent")
    cached = function_body(source, "checkPdfCompletedCacheReopen")
    forced_oom = function_body(source, "checkPdfForcedOom")
    begin_acceptance = function_body(source, "qemuAcceptanceBegin")
    tick_acceptance = function_body(source, "qemuAcceptanceTick")
    typography = function_body(source, "checkPdfTypography")
    typography_capture = function_body(source, "captureTypographySignature")
    progress = function_body(source, "checkPdfProgressAndSavedItems")
    epub_oracle = function_body(source, "runEpubOraclePass")
    check_epub_oracle = function_body(source, "checkEpubOracle")
    image = function_body(source, "checkPdfImage")
    product_tracer = function_body(source, "checkPdfProductTracer")
    cancellation = function_body(source, "checkPdfCancellation")

    for witness in (
        "ReflowReadingPosition nonTerminal = selected;",
        "nonTerminal.wordCursor = targetCursor;",
        "pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed)",
        "QEMU_PDF_PROGRESS_MID_PASS words=10 cursor=%lu percent=%lu resumed=1",
    ):
        if witness not in progress:
            failures.append(f"non-terminal progress witness: {witness}")

    if begin.count("++state.pdfParserEntries;") != 1:
        failures.append("parser counter increment")
    if step.count("++state.pdfExtractionEntries;") != 1:
        failures.append("extraction counter increment")
    if (
        "event.event != PdfResourceEventKind::Acquired" not in resource
        or "resources->acceptance->maxPdfAllocation =" not in resource
        or "std::max(resources->acceptance->maxPdfAllocation, bytes)"
        not in resource
    ):
        failures.append("maximum allocation assignment")
    for witness in (
        "sourceOpens != 1",
        "sourceReads == 0 || sourceReads > 2",
        "sourceMaximumReadRequest == 0",
        "sourceMaximumReadRequest > kMaximumIoRequestBytes",
        "resourcesBeforeTurns.freeHeap == 0",
        "resourcesBeforeTurns.largestBlock == 0",
        "resourcesBeforeTurns.stackMargin == 0",
        "resourcesAfterTurns.freeHeap == 0",
        "resourcesAfterTurns.largestBlock == 0",
        "resourcesAfterTurns.stackMargin == 0",
    ):
        if witness not in cached:
            failures.append(f"cached identity witness: {witness}")
    if (
        forced_oom.count("PdfError::InsufficientMemory") != 2
        or "const bool completedCache = initialized && document->loadCompletedCache();"
        not in forced_oom
        or "return exactInsufficientMemory && initialized && !completedCache;"
        not in forced_oom
    ):
        failures.append("forced OOM verification")
    if (
        tick_acceptance.count("checkPdfProductTracer(renderer)") != 1
        or "state.pdfTracerReady = true;" in begin_acceptance
    ):
        failures.append("boot 1 product tracer gate")
    if (
        "constexpr uint64_t kExpectedPdfTypographySemanticHash = "
        "0x95EE2813D71DFE2EULL;" not in source
        or "constexpr uint64_t kExpectedPdfTypographyTextHash = "
        "0xE1AC47B687F6E82AULL;" not in source
        or "six.semantic != kExpectedPdfTypographySemanticHash" not in typography
        or "six.text != kExpectedPdfTypographyTextHash" not in typography
        or "class SemanticTextFnvPrint final : public Print" not in source
        or "bool insideTag_ = false;" not in source
        or "bool insideEntity_ = false;" not in source
        or "bool pendingSpace_ = false;" not in source
        or "SemanticTextFnvPrint semantic;" not in typography_capture
        or "\n  FnvPrint semantic;" in typography_capture
        or "kExpectedPdfTypographyFrame" in source
        or ("7B63" + "F8FA") in source
        or "const uint32_t blankFrame = QemuHalControl::frameCrc32();"
        not in typography
        or "six.frame == 0" not in typography
        or "seventyTwo.frame == 0" not in typography
        or "six.frame != seventyTwo.frame" not in typography
        or "six.frame == blankFrame" not in typography
        or "seventyTwo.frame == blankFrame" not in typography
        or "frame_six=%08lX frame_seventy_two=%08lX" not in typography
        or "semantic_six=%08lX%08lX" not in typography
        or "semantic_seventy_two=%08lX%08lX" not in typography
        or "text_six=%08lX%08lX" not in typography
        or "text_seventy_two=%08lX%08lX" not in typography
        or "%016llX" in typography
        or "blank=%08lX words_six=%lu" not in typography
        or "words_seventy_two=%lu pages_six=%u pages_seventy_two=%u" not in typography
    ):
        failures.append("pinned PDF typography fixture")
    if (
        "kExpectedPdfProgressCursor" in source
        or "const uint32_t targetCursor = totalWords * 3U / 5U;" not in progress
        or "selected.wordCursor == 0 || selected.wordCursor > totalWords" not in progress
        or "positionSaved = document->saveReadingPosition(nonTerminal);" not in progress
        or "!progressDocument->saveReadingPosition(selected)" not in progress
        or "!pdfReadingPositionsEqualExact(selected, resumed)" not in progress
        or "resumed.wordCursor != selected.wordCursor" not in progress
        or "!pdfCalculateWordCursorProgress(resumed.wordCursor, totalWords, &wordProgress)"
        not in progress
        or "wordProgress * 100.0F + 0.5F" not in progress
        or "QEMU_PDF_PROGRESS_PAGE section=%u page=%u page_count=%u" not in progress
        or "found=%u valid=%u first=%lu last=%lu" not in progress
        or "cursor=%lu\\n" not in progress
        or "QEMU_PDF_PROGRESS_DIAGNOSTIC selected_range=%u section=%d page=%d" not in progress
        or "word_start=%lu word_cursor=%lu" not in progress
        or "total_words=%lu saved=%u progress_ok=%u millionths=%lu page_count=%d" not in progress
    ):
        failures.append("pinned PDF progress fixture")
    if (
        'emitEpubStage(false, "preclean");' not in check_epub_oracle
        or 'emitEpubStage(cached, "begin");' not in epub_oracle
        or 'emitEpubStage(cached, "load");' not in epub_oracle
        or 'emitEpubStage(cached, "section", 1, section->pageCount);'
        not in epub_oracle
        or 'emitEpubStage(cached, "xhtml", sectionIndex);' not in epub_oracle
        or 'emitEpubStage(cached, "css", 0);' not in epub_oracle
        or 'emitEpubStage(cached, "cache", 1);' not in epub_oracle
        or 'emitEpubStage(cached, "frame", 1);' not in epub_oracle
        or 'emitEpubStage(cached, "end");' not in epub_oracle
    ):
        failures.append("ordered QEMU EPUB stages")
    epub_pins = (
        ("kExpectedEpubSectionZeroHash", "0x46385061C46C2FE4ULL"),
        ("kExpectedEpubSectionOneHash", "0x4060CB229041492DULL"),
        ("kExpectedEpubCssHash", "0x3CC3246E367521F4ULL"),
        ("kExpectedEpubSectionCacheHash", "0xDEE723508F423F9AULL"),
        ("kExpectedEpubFrameHash", "0xE99DC1B84A90C006ULL"),
    )
    if any(
        f"constexpr uint64_t {name} = {value};" not in source
        or f"oracle->{field} != {name}" not in epub_oracle
        for field, (name, value) in zip(
            ("xhtml0", "xhtml1", "css", "cache", "frame"),
            epub_pins,
        )
    ):
        failures.append("independent EPUB fixture pins")
    if (
        'CountingPrint content("<img ", "Image cover caption.", "");'
        not in image
        or "content.firstCount() != 1" not in image
        or '"QEMU_PDF_IMAGE_PASS retained=%u ' not in image
    ):
        failures.append("counted PDF image fixture")
    if (
        "renderedFrame != 0" not in product_tracer
        or "renderedFrame != blankFrame" not in product_tracer
    ):
        failures.append("live nonblank product tracer")
    if (
        source.count("owner.beginSliceOperation(") != 12
        or source.count("owner.finishSliceOperation(") != 12
        or "void resetSliceTrace()" not in source
        or "SliceTrace sliceTrace() const" not in source
    ):
        failures.append("per-callback QEMU IO timing")
    if "constexpr uint32_t kMaximumCancellationSliceMicroseconds = 8000;" not in source:
        failures.append("exact cancellation microsecond ceiling")
    failures.extend(preparation_scheduler_failures(source))
    for token in (
        "constexpr uint32_t kQemuSlowAtomicWriteMicroseconds = 30000;",
        "constexpr uint32_t kQemuSlowAtomicRenameMicroseconds = 24000;",
        "constexpr uint32_t kQemuSlowAtomicOpenReadMicroseconds = 12000;",
        "constexpr uint32_t kQemuSlowAtomicNonIoMicroseconds = 500;",
        "constexpr uint32_t kQemuSlowAtomicAggregateRequestBytes = 3072;",
        "constexpr uint32_t kQemuSlowAtomicAggregateCallbackMicroseconds = 550000;",
        "constexpr uint32_t kQemuSlowAtomicAggregateNonIoMicroseconds = 5000;",
        "constexpr uint16_t kQemuSlowAtomicWriteCount = 22;",
        "constexpr uint16_t kQemuSlowAtomicRenameCount = 2;",
        "constexpr uint16_t kQemuSlowAtomicOpenReadCount = 2;",
        "constexpr uint16_t kQemuSlowAtomicTotalCount = 26;",
        "bool qemuSlowAtomicAllowed(",
        "trace.calls != 1",
        "trace.recursive",
        "trace.callbackElapsedUs > elapsedUs",
        "nonIoUs > kQemuSlowAtomicNonIoMicroseconds",
        "TracedPdfCacheIo::Operation::Write",
        "trace.openMode == TracedPdfCacheIo::OpenMode::None",
        "trace.requestBytes >= 1",
        "trace.requestBytes <= 1024",
        "trace.callbackElapsedUs <= kQemuSlowAtomicWriteMicroseconds",
        "TracedPdfCacheIo::Operation::Rename",
        "trace.requestBytes == 0",
        "trace.callbackElapsedUs <= kQemuSlowAtomicRenameMicroseconds",
        "TracedPdfCacheIo::Operation::Open",
        "trace.openMode == TracedPdfCacheIo::OpenMode::Read",
        "trace.callbackElapsedUs <= kQemuSlowAtomicOpenReadMicroseconds",
        '"QEMU_PDF_SLOW_ATOMIC index=%u slice=%u calls=%lu kind=%s mode=%s "',
        '"recursive=%u request=%lu total_us=%lu callback_us=%lu nonio_us=%lu\\n"',
        '"QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=%lu slices=%u total=%u write=%u "',
        '"rename=%u open_read=%u request_bytes=%lu callback_us=%lu nonio_us=%lu "',
        '"max_total_us=%lu max_callback_us=%lu\\n"',
    ):
        if token not in source:
            failures.append(f"QEMU slow atomic oracle: {token}")
    for field in (
        "uint32_t cancellationMaxSliceUs = 0;",
        "traced.resetSliceTrace();",
        "const TracedPdfCacheIo::SliceTrace sliceTrace = traced.sliceTrace();",
        "elapsedUs > kMaximumCancellationSliceMicroseconds",
        "sliceTrace.calls",
        "sliceTrace.operation",
        "sliceTrace.requestBytes",
        "sliceTrace.callbackElapsedUs",
        "sliceTrace.recursive",
        '"QEMU_PDF_CANCEL_FAIL reason=slice_budget elapsed_ms=%lu "',
        '"elapsed_us=%lu io_calls=%lu io_kind=%s io_mode=%s io_recursive=%u "',
        '"io_request=%lu callback_us=%lu nonio_us=%lu "',
        '"max_io_request=%lu generation=%lu "',
        '"expected_generation=%lu\\n"',
        '"max_slice_ms=%lu max_slice_us=%lu max_callback_us=%lu "',
        "static_cast<unsigned long>(elapsed)",
        "static_cast<unsigned long>(elapsedUs)",
        "static_cast<unsigned long>(ioCalls)",
        "static_cast<unsigned long>(traced.maximumRequest)",
        "static_cast<unsigned long>(preparation->generation())",
        "static_cast<unsigned long>(persistent->generation)",
    ):
        if field not in cancellation:
            failures.append(f"cancellation slice diagnostic: {field}")
    failures.extend(epub_platform_smoke_failures(source))
    return failures


def framebuffer_guard_symbol_failures(source: str, header: str) -> list[str]:
    failures: list[str] = []
    # C++ source text only proves that the acceptance-only seam is wired and
    # published. Behavior and mutation controls execute the shared function in
    # PdfAcceptanceBehaviorTest.
    for witness in (
        "const PdfAcceptanceFramebufferSnapshot framebufferBefore =",
        "const PdfAcceptanceFramebufferSnapshot framebufferAfter =",
        "pdfAcceptanceObserveFramebuffer(",
        "QEMU_PDF_FRAMEBUFFER_GUARD_PASS bytes=%lu checks=%lu violations=0 controls=%lu rejected=%lu",
        "state.pdfFramebufferGuardChecks",
        "state.pdfFramebufferGuardFailures",
        "state.pdfFramebufferGuardControls",
        "state.pdfFramebufferGuardRejections",
    ):
        if witness not in source:
            failures.append(f"runtime framebuffer guard symbol: {witness}")
    for witness in (
        "#if defined(SIMULATOR) || defined(CROSSINK_QEMU)",
        "CROSSINK_PDF_ACCEPTANCE_FRAMEBUFFER_GUARD_ENABLED",
        "pdfAcceptanceObserveFramebuffer(",
    ):
        if witness not in header:
            failures.append(f"framebuffer guard header symbol: {witness}")
    return failures


class QemuPdfAcceptanceContractTest(unittest.TestCase):
    def test_bounded_epub_fixture_is_source_locked(self) -> None:
        source = RAW_FIXTURE_SOURCES["epub_oracle.epub"]
        raw = source.read_bytes()
        self.assertEqual(len(raw), 3616)
        self.assertEqual(
            hashlib.sha256(raw).hexdigest(),
            "6d8d8949ffcb0c2df58b276915defe353251e98c05fc20b5a393210989eae462",
        )
        self.assertEqual(fnv64(raw), 0x92563E7E3D33C382)

        with zipfile.ZipFile(source) as archive:
            expected_entries = {
                "OEBPS/chapter1.xhtml": (1670, 0x46385061C46C2FE4),
                "OEBPS/chapter2.xhtml": (1920, 0x4060CB229041492D),
                "OEBPS/styles/test.css": (162, 0x3CC3246E367521F4),
            }
            for name, (expected_bytes, expected_hash) in expected_entries.items():
                with self.subTest(entry=name):
                    payload = archive.read(name)
                    self.assertEqual(len(payload), expected_bytes)
                    self.assertEqual(fnv64(payload), expected_hash)

            opf = ET.fromstring(archive.read("OEBPS/content.opf"))
            package = {"opf": "http://www.idpf.org/2007/opf"}
            dc = {"dc": "http://purl.org/dc/elements/1.1/"}
            self.assertEqual(
                opf.findtext("opf:metadata/dc:title", namespaces=package | dc),
                "Test: Synthetic Unicode Glyphs",
            )
            self.assertEqual(
                opf.findtext("opf:metadata/dc:creator", namespaces=package | dc),
                "CrossPoint Test Fixture",
            )
            self.assertEqual(
                opf.findtext("opf:metadata/dc:language", namespaces=package | dc),
                "en",
            )
            self.assertEqual(
                [
                    item.attrib["idref"]
                    for item in opf.findall("opf:spine/opf:itemref", package)
                ],
                ["chapter1", "chapter2"],
            )

            nav = ET.fromstring(archive.read("OEBPS/nav.xhtml"))
            xhtml = {"x": "http://www.w3.org/1999/xhtml"}
            links = nav.findall(".//x:nav//x:a", xhtml)
            self.assertEqual(
                [(link.text, link.attrib["href"]) for link in links],
                [
                    ("Glyph Reference", "chapter1.xhtml"),
                    ("Block Stress Cases", "chapter2.xhtml"),
                ],
            )

    def test_qemu_epub_diagnostics_do_not_instrument_production_epub_or_zip(self) -> None:
        epub = EPUB_SOURCE.read_text(encoding="utf-8")
        zip_source = ZIP_SOURCE.read_text(encoding="utf-8")
        qemu = QEMU_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("CROSSINK_QEMU", epub)
        self.assertNotIn("QEMU_EPUB_LOAD_STAGE", epub)
        self.assertNotIn("QEMU_ZIP_ZERO_PROGRESS", zip_source)
        self.assertNotIn("qemuZeroProgressIterations", zip_source)
        self.assertIn("QEMU_EPUB_STAGE pass=%s stage=%s", qemu)

    def test_typography_semantic_hash_normalizes_exact_prepared_xhtml(self) -> None:
        prepared = PREPARED_TYPOGRAPHY_XHTML.encode("utf-8")
        normalized = normalize_semantic_text(prepared)

        self.assertEqual(len(prepared), 255)
        self.assertEqual(fnv64(prepared), 0xAC50C9E1349FC6A8)
        self.assertEqual(normalized, b"Typography uses device defaults.")
        self.assertEqual(fnv64(normalized), 0x95EE2813D71DFE2E)
        self.assertNotEqual(fnv64(prepared), fnv64(normalized))

    def test_virtual_sd_fixtures_are_raw_byte_identical_copies(self) -> None:
        for destination_name, source in RAW_FIXTURE_SOURCES.items():
            with self.subTest(destination=destination_name):
                destination = QEMU_DATA / destination_name
                self.assertTrue(source.is_file(), source)
                self.assertTrue(destination.is_file(), destination)
                self.assertEqual(sha256(destination), sha256(source))

    def test_firmware_contract_covers_two_boot_full_acceptance(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")

        missing_markers = [
            marker for marker in ORDERED_STAGE_MARKERS if marker not in source
        ]
        self.assertEqual(missing_markers, [])
        marker_offsets = [source.find(marker) for marker in ORDERED_STAGE_MARKERS]
        self.assertEqual(marker_offsets, sorted(marker_offsets))
        for token in (
            "QEMU_EXPECT_RESET seq=0",
            "ESP.restart()",
            "requestCancel()",
            "resumedFromCheckpoint()",
            "workCounters()",
            "PdfSavedItemKind::Bookmark",
            "PdfSavedItemKind::Clipping",
            "kCachedPageTurns = 100",
            "retainedWriteTruncate",
            "retainedRemove",
            "font_size_6.pdf",
            "font_size_72.pdf",
            "raster_cover_caption.pdf",
            "epub_oracle.epub",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_qemu_negative_corpus_is_explicit_and_bounded(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        for fixture in (
            "bad_startxref.pdf",
            "oversized_length.pdf",
            "encrypted.pdf",
            "lzw_required.pdf",
            "scan_only.pdf",
            "flate_bomb.pdf",
        ):
            with self.subTest(fixture=fixture):
                self.assertIn(fixture, source)
        self.assertIn("kMaximumPreparationSteps", source)
        self.assertIn("PdfError::BudgetExhausted", source)
        self.assertIn("PdfError::IoFailure", source)
        self.assertIn("PdfError::InsufficientMemory", source)

    def test_completed_cache_reopen_tracks_the_exact_pdf_without_hal_growth(
        self,
    ) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")

        for token in (
            "trackSourcePath",
            "sourceReadOpenCount",
            "sourceReadCount",
            "trackedSourceHandles_",
            "std::strcmp(path, owner.trackedSourcePath_)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)

    def test_qemu_acceptance_source_contract(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(acceptance_source_failures(source), [])

    def test_qemu_runtime_framebuffer_guard_uses_shared_observation(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        header = FRAMEBUFFER_GUARD_HEADER.read_text(encoding="utf-8")
        self.assertEqual(framebuffer_guard_symbol_failures(source, header), [])

    def test_qemu_page_tree_memory_store_enforces_balanced_access(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        for witness in (
            "bool accessRequired = false;",
            "bool accessOpen = false;",
            "uint32_t accessOpenCount = 0;",
            "uint32_t accessCloseCount = 0;",
            "records.accessRequired && !records.accessOpen",
            "PdfStatus setMemoryRecordAccess(void* context, const bool required)",
            "traversalRecords.accessRequired = true;",
            "setMemoryRecordAccess, &traversalRecords, &workspace->pageScratch",
            "traversalRecords.accessOpenCount == 0",
            "traversalRecords.accessOpenCount != traversalRecords.accessCloseCount",
            "traversalRecords.accessOpen",
        ):
            with self.subTest(witness=witness):
                self.assertIn(witness, source)

    def test_qemu_nonterminal_progress_has_mutation_witnesses(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(acceptance_source_failures(source), [])
        for label, mutation in {
            "terminal cursor substituted": source.replace(
                "nonTerminal.wordCursor = targetCursor;",
                "nonTerminal.wordCursor = totalWords;",
                1,
            ),
            "reopen equality bypassed": source.replace(
                "pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed)",
                "true",
                1,
            ),
            "mid marker removed": source.replace(
                "QEMU_PDF_PROGRESS_MID_PASS",
                "QEMU_PDF_PROGRESS_REMOVED",
                1,
            ),
        }.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(acceptance_source_failures(mutation), [])
        self.assertEqual(source.count("emitRuntimeSample();"), 2)
        self.assertLess(
            source.index("emitRuntimeSample();"),
            source.index('esp_rom_printf("QEMU_EXPECT_RESET seq=0'),
        )
        self.assertNotIn("NO_ALLOCATION_SAMPLE", source)
        self.assertNotIn('"QEMU_PDF_CANCEL_DIAG ', source)

    def test_qemu_image_oracle_releases_display_section_before_suppressed(
        self,
    ) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(pdf_image_section_lifetime_failures(source), [])
        release = "  withImage.reset();\n"
        suppressed_section = "  std::unique_ptr<Section> withoutImage;\n"
        mutations = {
            "removed display section release": source.replace(release, "", 1),
            "moved release after suppressed section": source.replace(
                release,
                "",
                1,
            ).replace(
                suppressed_section,
                suppressed_section + release,
                1,
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(
                    pdf_image_section_lifetime_failures(mutation),
                    [],
                )

    def test_positive_pdf_corpus_has_four_independent_live_witnesses(
        self,
    ) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(positive_pdf_corpus_failures(source), [])

    def test_positive_pdf_corpus_witness_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(positive_pdf_corpus_failures(source), [])
        mutations = {
            "OCR text layer bypassed": source.replace(
                "ocrSemantic.hash() != kExpectedPdfOcrSemanticHash",
                "false",
                1,
            ),
            "column order bypassed": source.replace(
                "columnsHash != kExpectedPdfColumnsSemanticHash",
                "false",
                1,
            ),
            "row-major table bypassed": source.replace(
                "tableHash != kExpectedPdfTableSemanticHash",
                "false",
                1,
            ),
            "JPEG decode frame bypassed": source.replace(
                "jpegFrame == blankFrame",
                "false",
                1,
            ),
            "OCR failure code collapsed": source.replace(
                'fail("PDF_CORPUS_OCR",',
                'fail("PDF_CORPUS",',
                1,
            ),
            "column failure code collapsed": source.replace(
                'fail("PDF_CORPUS_COLUMNS",',
                'fail("PDF_CORPUS",',
                1,
            ),
            "table failure code collapsed": source.replace(
                'fail("PDF_CORPUS_TABLE",',
                'fail("PDF_CORPUS",',
                1,
            ),
            "JPEG failure code collapsed": source.replace(
                'fail("PDF_CORPUS_JPEG",',
                'fail("PDF_CORPUS",',
                1,
            ),
            "JPEG retained diagnostic image count removed": source.replace(
                "image_tags=%u captions=%u",
                "captions=%u",
                1,
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(positive_pdf_corpus_failures(mutation), [])

    def test_bounded_epub_platform_smoke_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(epub_platform_smoke_failures(source), [])
        mutations = {
            "changed source pin": source.replace(
                "0x92563E7E3D33C382ULL",
                "0x92563E7E3D33C380ULL",
                1,
            ),
            "CSS enabled": source.replace(
                "epub->load(!cached, true)",
                "epub->load(!cached, false)",
                1,
            ),
            "embedded styles enabled": source.replace(
                "SETTINGS.embeddedStyle = 0;",
                "SETTINGS.embeddedStyle = 1;",
                1,
            ),
            "cached recreation": source.replace(
                'loadOrCreateSection(epub, renderer, layout, 1, "", cached, &section)',
                'loadOrCreateSection(epub, renderer, layout, 1, "", false, &section)',
                1,
            ),
            "removed progress reopen": source.replace(
                "EpubReaderUtils::loadProgress",
                "EpubReaderUtils::ignoredProgress",
                1,
            ),
            "removed bookmark reopen": source.replace(
                "BOOKMARKS.loadForBook(reopened->getPath()",
                "BOOKMARKS.ignoreForBook(reopened->getPath()",
                1,
            ),
            "weakened sidecar check": source.replace(
                '"/sections/1.bin.pwi.tmp"',
                '"/sections/1.bin.allowed"',
                1,
            ),
            "weakened temp check": source.replace(
                '"/css_rules.cache.tmp"',
                '"/css_rules.cache.allowed"',
                1,
            ),
        }
        for mutation, mutated_source in mutations.items():
            with self.subTest(mutation=mutation):
                self.assertNotEqual(mutated_source, source)
                self.assertNotEqual(
                    epub_platform_smoke_failures(mutated_source),
                    [],
                )

    def test_epub_oracle_marker_preserves_every_64_bit_hash(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(epub_oracle_marker_format_failures(source), [])

    def test_epub_oracle_marker_format_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(epub_oracle_marker_format_failures(source), [])
        uncached_pair = (
            "static_cast<unsigned long>(uncached.xhtml0 >> 32U), "
            "static_cast<unsigned long>(uncached.xhtml0)"
        )
        mutations = {
            "ROM-truncated llX": source.replace(
                "xhtml0=%08lX%08lX", "xhtml0=%016llX", 1
            ),
            "uncached low half only": source.replace(
                "xhtml0=%08lX%08lX", "xhtml0=%08lX", 1
            ),
            "cached low half only": source.replace(
                '      "cache=%08lX%08lX frame=%08lX%08lX\\n",\n'
                "      static_cast<unsigned long>(cached.xhtml0 >> 32U)",
                '      "cache=%08lX%08lX frame=%08lX\\n",\n'
                "      static_cast<unsigned long>(cached.xhtml0 >> 32U)",
                1,
            ),
            "reversed high-low arguments": source.replace(
                uncached_pair,
                "static_cast<unsigned long>(uncached.xhtml0), "
                "static_cast<unsigned long>(uncached.xhtml0 >> 32U)",
                1,
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(epub_oracle_marker_format_failures(mutation), [])

    def test_qemu_acceptance_source_contract_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(acceptance_source_failures(source), [])
        mutations = {
            "parser increment": source.replace(
                "++state.pdfParserEntries;",
                "(void)state.pdfParserEntries;",
                1,
            ),
            "extraction increment": source.replace(
                "++state.pdfExtractionEntries;",
                "(void)state.pdfExtractionEntries;",
                1,
            ),
            "maximum allocation assignment": source.replace(
                "resources->acceptance->maxPdfAllocation = "
                "std::max(resources->acceptance->maxPdfAllocation, bytes);",
                "(void)bytes;",
                1,
            ),
            "forced OOM verification": source.replace(
                "return exactInsufficientMemory && initialized && "
                "!completedCache;",
                "return initialized && !completedCache;",
                1,
            ),
            "product tracer call": source.replace(
                "if (checkPdfProductTracer(renderer)) {",
                "if (true) {",
                1,
            ),
            "typography fixture pin": source.replace(
                "six.semantic != kExpectedPdfTypographySemanticHash",
                "false",
                1,
            ),
            "typography raw XHTML sink": source.replace(
                "SemanticTextFnvPrint semantic;",
                "FnvPrint semantic;",
                1,
            ),
            "typography truncated ROM hash formatting": source.replace(
                "semantic_six=%08lX%08lX",
                "semantic_six=%016llX",
                1,
            ),
            "typography live blank witness": source.replace(
                "six.frame == blankFrame",
                "false",
                1,
            ),
            "typography self-derived frame constant": source.replace(
                "six.frame == blankFrame",
                "six.frame != 0x" + "7B63" + "F8FAU",
                1,
            ),
            "progress hardcoded cursor": source.replace(
                "constexpr uint64_t kExpectedPdfTypographyTextHash = "
                "0xE1AC47B687F6E82AULL;",
                "constexpr uint64_t kExpectedPdfTypographyTextHash = "
                "0xE1AC47B687F6E82AULL;\n"
                "constexpr uint32_t kExpectedPdfProgressCursor = 6;",
                1,
            ),
            "EPUB section stage": source.replace(
                'emitEpubStage(cached, "section", 1, section->pageCount);',
                "(void)section;",
                1,
            ),
            "EPUB fixture pin": source.replace(
                "oracle->xhtml0 != kExpectedEpubSectionZeroHash",
                "false",
                1,
            ),
            "image count pin": source.replace(
                "content.firstCount() != 1",
                "false",
                1,
            ),
            "product tracer nonzero frame": source.replace(
                "renderedFrame != 0",
                "true",
                1,
            ),
            "exact standard microsecond limit": source.replace(
                "elapsedUs > kMaximumCancellationSliceMicroseconds",
                "false",
                1,
            ),
            "native eight millisecond ceiling": source.replace(
                "kMaximumCancellationSliceMicroseconds = 8000",
                "kMaximumCancellationSliceMicroseconds = 8001",
                1,
            ),
            "slow write ceiling": source.replace(
                "kQemuSlowAtomicWriteMicroseconds = 30000",
                "kQemuSlowAtomicWriteMicroseconds = 30001",
                1,
            ),
            "slow rename ceiling": source.replace(
                "kQemuSlowAtomicRenameMicroseconds = 24000",
                "kQemuSlowAtomicRenameMicroseconds = 24001",
                1,
            ),
            "slow open ceiling": source.replace(
                "kQemuSlowAtomicOpenReadMicroseconds = 12000",
                "kQemuSlowAtomicOpenReadMicroseconds = 12001",
                1,
            ),
            "slow nonio ceiling": source.replace(
                "kQemuSlowAtomicNonIoMicroseconds = 500",
                "kQemuSlowAtomicNonIoMicroseconds = 501",
                1,
            ),
            "slow request aggregate": source.replace(
                "kQemuSlowAtomicAggregateRequestBytes = 3072",
                "kQemuSlowAtomicAggregateRequestBytes = 3073",
                1,
            ),
            "slow callback aggregate": source.replace(
                "kQemuSlowAtomicAggregateCallbackMicroseconds = 550000",
                "kQemuSlowAtomicAggregateCallbackMicroseconds = 550001",
                1,
            ),
            "slow nonio aggregate": source.replace(
                "kQemuSlowAtomicAggregateNonIoMicroseconds = 5000",
                "kQemuSlowAtomicAggregateNonIoMicroseconds = 5001",
                1,
            ),
            "slow write count": source.replace(
                "kQemuSlowAtomicWriteCount = 22",
                "kQemuSlowAtomicWriteCount = 23",
                1,
            ),
            "slow rename count": source.replace(
                "kQemuSlowAtomicRenameCount = 2",
                "kQemuSlowAtomicRenameCount = 3",
                1,
            ),
            "slow open count": source.replace(
                "kQemuSlowAtomicOpenReadCount = 2",
                "kQemuSlowAtomicOpenReadCount = 3",
                1,
            ),
            "slow total count": source.replace(
                "kQemuSlowAtomicTotalCount = 26",
                "kQemuSlowAtomicTotalCount = 27",
                1,
            ),
            "per-slice trace reset": source.replace(
                "traced.resetSliceTrace();",
                "(void)traced;",
                1,
            ),
        }
        for mutation, mutated_source in mutations.items():
            with self.subTest(mutation=mutation):
                self.assertNotEqual(mutated_source, source)
                self.assertNotEqual(
                    acceptance_source_failures(mutated_source),
                    [],
                )

    def test_qemu_pdf_preparation_scheduler_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(preparation_scheduler_failures(source), [])

        removal_targets = {
            "runPreparation": (
                "    yieldAfterPdfPreparationStep(result);\n"
                "    if (!result.yielded())",
                "    if (!result.yielded())",
            ),
            "cancellation setup": (
                "    yieldAfterPdfPreparationStep(result);\n"
                "  }\n"
                "  if (!status || !result.yielded()",
                "  }\n  if (!status || !result.yielded()",
            ),
            "timed cancellation": (
                "      yieldAfterPdfPreparationStep(result);\n"
                "      continue;",
                "      continue;",
            ),
            "product tracer": (
                "    yieldAfterPdfPreparationStep(result);\n  }",
                "  }",
            ),
            "navigation": (
                "    yieldAfterPdfPreparationStep(result);\n  }",
                "  }",
            ),
        }
        function_regions = {
            "runPreparation": ("runPreparation", "preparePdf"),
            "cancellation setup": (
                "checkPdfCancellation",
                "workCountersResumeLess",
            ),
            "timed cancellation": (
                "checkPdfCancellation",
                "workCountersResumeLess",
            ),
            "product tracer": ("checkPdfProductTracer", "checkPdfNavigation"),
            "navigation": ("checkPdfNavigation", "checkStorageOpenParity"),
        }
        mutations = {
            f"removed {path} yield": replace_between_functions(
                source, *function_regions[path], old, new
            )
            for path, (old, new) in removal_targets.items()
        }
        for path in (
            "runPreparation",
            "cancellation setup",
            "product tracer",
            "navigation",
        ):
            mutations[f"moved {path} yield before accounting"] = replace_between_functions(
                source,
                *function_regions[path],
                "    sampleRuntime();\n    yieldAfterPdfPreparationStep(result);",
                "    yieldAfterPdfPreparationStep(result);\n    sampleRuntime();",
            )
        mutations["moved timed cancellation yield before measurement"] = (
            move_cancellation_yield_before_measurement(source)
        )
        mutations["unconditional helper yield"] = replace_between_functions(
            source,
            "yieldAfterPdfPreparationStep",
            "runPreparation",
            "  if (result.yielded()) {",
            "  if (true) {",
        )
        mutations["delay instead of scheduler yield"] = replace_between_functions(
            source,
            "yieldAfterPdfPreparationStep",
            "runPreparation",
            "    yield();",
            "    delay(1);",
        )
        mutations["yield inside tracked step interval"] = replace_between_functions(
            source,
            "stepTrackedPdfPreparation",
            "yieldAfterPdfPreparationStep",
            "  return preparation.step();",
            "  PdfStepResult result = preparation.step();\n"
            "  yieldAfterPdfPreparationStep(result);\n"
            "  return result;",
        )
        for mutation, mutated_source in mutations.items():
            with self.subTest(mutation=mutation):
                self.assertNotEqual(mutated_source, source)
                self.assertNotEqual(
                    preparation_scheduler_failures(mutated_source),
                    [],
                )

    def test_boot_two_acceptance_returns_to_main_loop_between_groups(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(boot_two_scheduler_failures(source), [])

    def test_boot_two_acceptance_scheduler_mutation_controls(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        mutations = {
            "old synchronous begin chain": source.replace(
                "  state.phase = AcceptancePhase::BootTwoResume;",
                "  if (!checkPdfResume(renderer, persistentAcceptance) ||\n"
                "      !checkPdfTypography(renderer)) {\n"
                "    return;\n"
                "  }\n"
                "  state.phase = AcceptancePhase::BootTwoNavigation;",
                1,
            ),
            "skipped typography phase": source.replace(
                "state.phase = AcceptancePhase::BootTwoTypography;",
                "state.phase = AcceptancePhase::BootTwoNavigation;",
                1,
            ),
            "fallthrough between groups": source.replace(
                "    return;\n    case AcceptancePhase::BootTwoTypography:",
                "    case AcceptancePhase::BootTwoTypography:",
                1,
            ),
            "two groups in resume tick": source.replace(
                "    state.phase = AcceptancePhase::BootTwoTypography;",
                "    checkPdfTypography(renderer);\n"
                "    state.phase = AcceptancePhase::BootTwoTypography;",
                1,
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(boot_two_scheduler_failures(mutation), [])

    def test_qemu_only_acceptance_does_not_add_hardware_operations(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        for forbidden in (
            "upload",
            "erase",
            "monitor",
            "esptool",
            "USB",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_timing_diagnostic_is_fixed_size_and_cannot_signal_acceptance(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        for token in (
            "#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC",
            "std::array<TimingDiagnosticRecord, kMaximumCancellationSlices>",
            "QEMU_PDF_TIMING_SAMPLE",
            "QEMU_PDF_CANCEL_DIAGNOSTIC",
            "QEMU_TIMING_DIAGNOSTIC_COMPLETE",
        ):
            with self.subTest(token=token):
                self.assertIn(token, source)
        self.assertNotIn("std::vector<TimingDiagnosticRecord", source)
        diagnostic_branch = source.index('esp_rom_printf("QEMU_TIMING_DIAGNOSTIC_COMPLETE')
        acceptance_branch = source.index('esp_rom_printf("QEMU_TEST_PASS')
        self.assertNotEqual(diagnostic_branch, acceptance_branch)


if __name__ == "__main__":
    unittest.main()
