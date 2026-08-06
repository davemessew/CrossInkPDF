import contextlib
import configparser
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "run_pdf_simulator_acceptance.py"
SMOKE_SOURCE = REPO_ROOT / "src" / "simulator" / "SimulatorSmokeTest.cpp"
ACCEPTANCE_SOURCE = (
    REPO_ROOT / "src" / "simulator" / "PdfSimulatorAcceptance.cpp"
)
FRAMEBUFFER_GUARD_HEADER = REPO_ROOT / "src" / "PdfAcceptanceFramebufferGuard.h"
FRAMEBUFFER_BEHAVIOR_TEST = (
    REPO_ROOT
    / "test"
    / "pdf_simulator_acceptance"
    / "PdfAcceptanceBehaviorTest.cpp"
)
SIMULATOR_CMAKE = REPO_ROOT / "test" / "pdf_simulator_acceptance" / "CMakeLists.txt"
FILE_FORMATS = REPO_ROOT / "docs" / "file-formats.md"
SEMANTIC_WRITER_SOURCE = (
    REPO_ROOT / "lib" / "PdfReflow" / "PdfSemanticWriter.cpp"
)
EPUB_RUNNER_SOURCE = REPO_ROOT / "scripts" / "run_simulator_smoke_test.py"
EPUB_REGRESSION_SOURCE = (
    REPO_ROOT / "src" / "simulator" / "EpubReflowRegressionOracle.cpp"
)
CHECKPOINT_HEADER = (
    REPO_ROOT / "lib" / "PdfReflow" / "PdfBuildCheckpoint.h"
)
PREPARATION_SOURCE = (
    REPO_ROOT / "lib" / "PdfReflow" / "PdfPreparation.cpp"
)
REFLOW_DOCUMENT_HEADER = (
    REPO_ROOT / "lib" / "Reflow" / "ReflowDocument.h"
)
SECTION_SOURCE = REPO_ROOT / "lib" / "Epub" / "Epub" / "Section.cpp"
PLATFORMIO_CONFIG = REPO_ROOT / "platformio.ini"
ROOT_TEST_CMAKE = REPO_ROOT / "test" / "CMakeLists.txt"
PDF_NAVIGATION_CMAKE = REPO_ROOT / "test" / "pdf_navigation" / "CMakeLists.txt"
PDF_NAVIGATION_TEST = (
    REPO_ROOT / "test" / "pdf_navigation" / "PdfNavigationTest.cpp"
)
CI_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci.yml"
CHANGELOG = REPO_ROOT / "CHANGELOG.md"

PDF_WORD_INDEX_GATE = re.compile(
    r"bool\s+Section\s*::\s*usesPdfWordIndex\s*\(\s*\)\s*const\s*"
    r"\{\s*return\s+(?P<condition>[^;]+?)\s*;\s*\}",
    flags=re.DOTALL,
)
PDF_WORD_INDEX_CONDITION = (
    "document->getFormat() == ReflowDocumentFormat::Pdf && "
    'filePath.find("_fn_") == std::string::npos'
)


def validate_loose_local_resource_contract(source: str) -> None:
    if source.count("imageHref_(imagePath_)") != 1:
        raise AssertionError(
            "borrowed PDF oracle must keep its canonical absolute image href"
        )
    if 'href != imageHref_' not in source:
        raise AssertionError(
            "borrowed PDF oracle must compare the resolved canonical href"
        )
    if "ReflowResource::borrowedLocalFile(imagePath_" not in source:
        raise AssertionError(
            "borrowed PDF oracle must return the immutable physical image"
        )


def validate_portable_cmake_source(source: str) -> None:
    script_path = (
        '"${CMAKE_CURRENT_SOURCE_DIR}/test_pdf_simulator_acceptance.py" -v'
    )
    if script_path not in source:
        raise AssertionError(
            "simulator acceptance CTest must invoke the test script by source path"
        )
    if "-m unittest" in source or (
        "test.pdf_simulator_acceptance.test_pdf_simulator_acceptance" in source
    ):
        raise AssertionError(
            "simulator acceptance CTest must not use the stdlib-colliding test namespace"
        )


def validate_semantic_anchor_documentation(
    documentation: str, source: str
) -> None:
    if 'HEX_DIGITS[] = "0123456789abcdef"' not in source:
        raise AssertionError("semantic anchor encoder must use lowercase hexadecimal")
    if not re.search(
        r"`b` followed by eight lowercase\s+hexadecimal digits", documentation
    ):
        raise AssertionError(
            "file-format documentation must describe lowercase semantic anchors"
        )


def validate_cooperative_cancellation_source(source: str) -> None:
    if re.search(
        r"requestCancel\(\);\s*"
        r"const PdfStepResult cancelledResult = cancelled->step\(\);",
        source,
    ):
        raise AssertionError(
            "native acceptance still has the one-step terminal assumption"
        )

    required = {
        "bounded cooperative cancellation loop": (
            "cancellationSlice < kMaximumCancellationSlices"
        ),
        "per-slice elapsed cap": (
            "cancellationElapsedMs > "
            "kMaximumCancellationSliceMilliseconds"
        ),
        "per-slice operation cap": (
            "cancellationIoCalls > "
            "kMaximumCancellationSliceOperations"
        ),
        "yielded cancellation continuation": (
            "if (cancelledResult.yielded())"
        ),
        "terminal cancellation witness": (
            "cancelledResult.status.error != PdfError::Cancelled"
        ),
        "cap exhaustion failure": (
            'error = "preparation cancellation exhausted bounded slices";'
        ),
        "same-generation resume witness": (
            "preparation->generation() != expectedResume->generation"
        ),
    }
    for label, needle in required.items():
        if needle not in source:
            raise AssertionError(f"native acceptance lacks {label}")


def validate_measured_step_budget_source(source: str) -> None:
    required = {
        "steady clock measurement": "std::chrono::steady_clock::now()",
        "microsecond measurement": "std::chrono::duration_cast<std::chrono::microseconds>",
        "actual eight millisecond gate": (
            "actualElapsedUs > kMaximumPreparationStepMicroseconds"
        ),
        "deterministic synthetic tick": "return clock.nowMs++;",
        "nonadvancing clock guard": "clock.nowMs <= syntheticStartedAtMs",
    }
    for label, needle in required.items():
        if needle not in source:
            raise AssertionError(f"native acceptance lacks {label}")
    if source.count("preparation.step()") != 1 or "preparation->step()" in source:
        raise AssertionError(
            "native acceptance must use the measured step gateway exclusively"
        )


def validate_oracle_publish_order_source(source: str) -> None:
    run_source = source[source.index("def run(arguments:") :]
    epub_replay = run_source.find("        _run_epub_regression(")
    oracle_publish = run_source.find("\n        if arguments.update_oracle:\n")
    if epub_replay < 0 or oracle_publish < 0 or oracle_publish < epub_replay:
        raise AssertionError(
            "locked PDF oracle must publish only after the EPUB oracle passes"
        )


def validate_epub_program_contract(
    pdf_runner_source: str,
    epub_runner_source: str,
) -> None:
    if (
        '"--program",\n        str(program),' not in pdf_runner_source
        or "custom --program cannot run" in pdf_runner_source
    ):
        raise AssertionError(
            "PDF runner must forward the exact isolated program to EPUB"
        )
    epub_invocation = (
        "        _run_epub_regression(\n"
        "            program, arguments.headless, arguments.timeout\n"
        "        )"
    )
    if epub_invocation not in pdf_runner_source:
        raise AssertionError("PDF acceptance must invoke the EPUB oracle")
    required_epub = (
        'parser.add_argument("--program"',
        "program = Path(args.program).resolve()",
        "if not program.exists():",
        "[str(program)]",
    )
    if any(needle not in epub_runner_source for needle in required_epub):
        raise AssertionError(
            "EPUB runner must execute its explicit --program argument"
        )


def validate_internal_link_target_source(source: str) -> None:
    required = {
        "internal href parser": "validateEveryInternalLinkTarget",
        "section resolver": "document.resolveHrefToSectionIndex(href)",
        "fragment parser": "href.find('#')",
        "target anchor lookup": (
            "target.find(anchor) == std::string::npos"
        ),
        "resolver invocation": (
            "validateEveryInternalLinkTarget(*document, sectionContents, "
            "resolvedInternalLinks, error)"
        ),
        "resolved link oracle": (
            'navigation["resolved_internal_links"] = resolvedInternalLinks;'
        ),
    }
    for label, needle in required.items():
        if needle not in source:
            raise AssertionError(f"native acceptance lacks {label}")


def validate_page_image_pixel_source(source: str) -> None:
    required = {
        "PageImage inspection": "element->getTag() == TAG_PageImage",
        "image rectangle width": "pageImage.getImageBlock().getWidth()",
        "image rectangle height": "pageImage.getImageBlock().getHeight()",
        "framebuffer region copy": "renderer.copyRegionToBuffer(",
        "non-white pixel count": "nonWhitePixels",
        "PageImage oracle": 'image["page_image_found"] = true;',
        "pixel oracle": 'image["non_white_pixels"] = nonWhitePixels;',
        "image capture invocation": (
            "if (!imageDocument ||\n"
            "      !captureImage(imageDocument, renderer, cachedPass, "
            "counters, oracle[\"image\"].to<JsonObject>(), error))"
        ),
    }
    for label, needle in required.items():
        if needle not in source:
            raise AssertionError(f"native acceptance lacks {label}")


def validate_production_progress_source(source: str) -> None:
    required = {
        "production word progress call": (
            "pdfCalculateWordCursorProgress(nonTerminalResumed.wordCursor, "
            "totalWords, "
            "&wordProgress)"
        ),
        "exact non-terminal cursor": (
            "nonTerminal.wordCursor = targetCursor;"
        ),
        "non-terminal persistence": (
            "pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed)"
        ),
        "non-terminal save": "document->saveReadingPosition(nonTerminal)",
        "non-terminal persistence reload": (
            "document->loadReadingPosition(nonTerminalResumed)"
        ),
        "terminal persistence": (
            "pdfReadingPositionsEqualExact(selected, resumed)"
        ),
        "non-terminal saved cursor": (
            'progress["nonterminal_saved_cursor"] = nonTerminal.wordCursor;'
        ),
        "non-terminal resumed cursor": (
            'progress["nonterminal_resumed_cursor"] = '
            "nonTerminalResumed.wordCursor;"
        ),
        "production progress emission": (
            'progress["nonterminal_percent_millionths"] = '
            "static_cast<uint32_t>(std::lround(wordProgress * 1000000.0F));"
        ),
        "progress capture invocation": (
            "if (!captureNavigation(navigationDocument, renderer, cachedPass, "
            "counters, navigation, progress, error))"
        ),
    }
    normalized_source = re.sub(r"\s+", " ", source)
    for label, needle in required.items():
        if re.sub(r"\s+", " ", needle) not in normalized_source:
            raise AssertionError(f"native acceptance lacks {label}")
    if normalized_source.count(
        "pdfReadingPositionsEqualExact(selected, resumed)"
    ) != 2:
        raise AssertionError("native acceptance lacks terminal persistence coverage")
    if "nonTerminalResumed = nonTerminal;" in source:
        raise AssertionError(
            "native acceptance uses a self-assignment instead of persisted reload"
        )
    cached_branch = normalized_source.find(
        "if (cachedPass) {",
        normalized_source.find("ReflowReadingPosition resumed;"),
    )
    persistence = normalized_source.find(
        "if (!document->saveReadingPosition(nonTerminal)", cached_branch
    )
    cached_else = normalized_source.find(
        "} else {", cached_branch, persistence + 1
    )
    if cached_branch < 0 or persistence < 0 or cached_else >= 0:
        raise AssertionError(
            "native acceptance does not persist and reload 6/10 in every branch"
        )


def validate_runtime_framebuffer_guard_symbols(
    source: str, header: str
) -> None:
    # Source text is only suitable for checking that the acceptance-only seam is
    # wired and published. Its causal behavior is compiled and executed below.
    required_source_symbols = {
        "allocation-free framebuffer snapshot": (
            "PdfAcceptanceFramebufferSnapshot preparationFramebufferSnapshot("
        ),
        "shared observation call": "pdfAcceptanceObserveFramebuffer(",
        "published check counter": 'counterJson["framebuffer_guard_checks"]',
        "published violation counter": (
            'counterJson["framebuffer_guard_failures"]'
        ),
        "published control counter": 'counterJson["framebuffer_guard_controls"]',
        "published rejection counter": (
            'counterJson["framebuffer_guard_rejections"]'
        ),
    }
    for label, symbol in required_source_symbols.items():
        if symbol not in source:
            raise AssertionError(f"native acceptance lacks {label}")

    build_guard = "#if defined(SIMULATOR) || defined(CROSSINK_QEMU)"
    for label, symbol in {
        "acceptance-only build guard": build_guard,
        "guard availability marker": (
            "CROSSINK_PDF_ACCEPTANCE_FRAMEBUFFER_GUARD_ENABLED"
        ),
        "shared observation function": "pdfAcceptanceObserveFramebuffer(",
    }.items():
        if symbol not in header:
            raise AssertionError(f"framebuffer guard header lacks {label}")


def compile_and_run_framebuffer_behavior_variants(
    header_variants: dict[str, str], temporary_root: Path
) -> list[subprocess.CompletedProcess[str]]:
    source_path = FRAMEBUFFER_BEHAVIOR_TEST.as_posix()
    source_include = (REPO_ROOT / "src").as_posix()
    i18n_include = (REPO_ROOT / "lib" / "I18n").as_posix()
    pdf_include = (REPO_ROOT / "lib" / "PdfReflow").as_posix()
    target_lines: list[str] = []
    for name, header in header_variants.items():
        include_directory = temporary_root / name
        include_directory.mkdir(parents=True)
        (include_directory / FRAMEBUFFER_GUARD_HEADER.name).write_text(
            header, encoding="utf-8"
        )
        target_lines.extend(
            (
                f'add_executable({name} "{source_path}")',
                f"target_compile_features({name} PRIVATE cxx_std_20)",
                f"target_compile_definitions({name} PRIVATE SIMULATOR=1)",
                f'target_include_directories({name} PRIVATE '
                f'"{include_directory.as_posix()}" "{source_include}" '
                f'"{i18n_include}" "{pdf_include}")',
                f"add_test(NAME {name} COMMAND {name})",
            )
        )

    default_source = temporary_root / "default_guard.cpp"
    default_source.write_text(
        '#include "PdfAcceptanceFramebufferGuard.h"\n'
        "#ifdef CROSSINK_PDF_ACCEPTANCE_FRAMEBUFFER_GUARD_ENABLED\n"
        '#error "acceptance framebuffer guard leaked into the default device build"\n'
        "#endif\n"
        "int main() { return 0; }\n",
        encoding="utf-8",
    )
    baseline_include = (temporary_root / "framebuffer_baseline").as_posix()
    target_lines.extend(
        (
            f'add_executable(framebuffer_default_guard "{default_source.as_posix()}")',
            "target_compile_features(framebuffer_default_guard PRIVATE cxx_std_20)",
            "target_include_directories(framebuffer_default_guard PRIVATE "
            f'"{baseline_include}")',
            "add_test(NAME framebuffer_default_guard COMMAND framebuffer_default_guard)",
            "set_tests_properties(",
            "  framebuffer_suppressed_increment framebuffer_hardwired_predicate",
            "  PROPERTIES WILL_FAIL TRUE",
            ")",
        )
    )
    (temporary_root / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.16)",
                "project(framebuffer_behavior_variants LANGUAGES CXX)",
                "enable_testing()",
                *target_lines,
                "",
            )
        ),
        encoding="utf-8",
    )

    build_directory = temporary_root / "build"
    commands = (
        (
            "cmake",
            "-S",
            str(temporary_root),
            "-B",
            str(build_directory),
            "-DCMAKE_BUILD_TYPE=Release",
        ),
        (
            "cmake",
            "--build",
            str(build_directory),
            "--config",
            "Release",
            "--parallel",
            "4",
        ),
        (
            "ctest",
            "--test-dir",
            str(build_directory),
            "-C",
            "Release",
            "--output-on-failure",
        ),
    )
    return [
        subprocess.run(
            command,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        for command in commands
    ]


def validate_cross_process_native_source(source: str) -> None:
    required = {
        "phase environment": "CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PHASE",
        "nonce environment": "CROSSINK_SIMULATOR_PDF_ACCEPTANCE_NONCE",
        "reset marker": "SIM_PDF_ACCEPTANCE_RESET ",
        "cancel phase": 'kCancelPhase[] = "cancel"',
        "resume phase": 'kResumePhase[] = "resume"',
        "persisted cancel evidence": "writeResumeEvidence(",
        "persisted resume evidence": "readResumeEvidence(",
        "durable named checkpoint": (
            "durableResumePhase() != PdfBuildResumePhase::AfterImageRepair"
        ),
        "selected named checkpoint": (
            "resumedPhase() != PdfBuildResumePhase::AfterImageRepair"
        ),
        "checkpoint decode": "loadCheckpointSlots(",
        "selected checkpoint continuity": (
            "checkpointEvidenceEqual(resumeSelection.checkpoint, "
            "expectedResume->checkpoint)"
        ),
        "source continuity": "pdfSourceIdentityEqual(",
    }
    for label, needle in required.items():
        if needle not in source:
            raise AssertionError(f"native acceptance lacks {label}")
    if "const bool cancelAndResume" in source:
        raise AssertionError(
            "native acceptance still resumes inside the cancellation process"
        )


def validate_checkpoint_and_resume_documentation(
    documentation: str,
    checkpoint_header: str,
    preparation_source: str,
) -> None:
    enum_match = re.search(
        r"enum class PdfBuildResumePhase\s*:\s*uint8_t\s*\{([^}]*)\}",
        checkpoint_header,
        flags=re.DOTALL,
    )
    if enum_match is None:
        raise AssertionError("checkpoint source lacks the resume phase enum")
    phases = [
        entry.strip().rstrip(",")
        for entry in enum_match.group(1).splitlines()
        if entry.strip()
    ]
    expected_phases = [
        "None",
        "CommitManifest",
        "AfterEmitSections",
        "AfterPage",
        "AfterImage",
        "AfterImageRepair",
    ]
    if phases != expected_phases:
        raise AssertionError("checkpoint source resume phase values changed")

    source_contract = {
        "checkpoint codec 3": (
            "PDF_BUILD_CHECKPOINT_CODEC_VERSION = 3"
        ),
        "96-byte checkpoint": "constexpr size_t kCheckpointBytes = 96;",
        "512-byte page record": (
            "constexpr size_t kPageResumeRecordBytes = 512;"
        ),
        "192-byte discovery header": (
            "constexpr size_t kDiscoveryHeaderBytes = 192;"
        ),
        "24-byte sorted xref": (
            "constexpr size_t kDiscoveryXrefRecordBytes = 24;"
        ),
        "244-byte explicit page": (
            "constexpr size_t kDiscoveryPageRecordBytes = 244;"
        ),
        "72-byte discovery trailer": (
            "constexpr size_t kDiscoveryTrailerBytes = 72;"
        ),
        "discovery codec 1": "constexpr uint16_t kDiscoveryVersion = 1;",
        "page record codec 2": (
            "constexpr uint16_t kPageResumeRecordVersion = 2;"
        ),
        "discovery header magic": 'std::memcpy(output, "PDRH", 4);',
        "discovery trailer magic": 'std::memcpy(output, "PDRT", 4);',
        "page record magic": 'std::memcpy(output, "PRJR", 4);',
        "aggregate CRC": "writeLe32Bmp(output + 28, cacheSetupCrc32_);",
        "aggregate FNV ledger": (
            "writeLe64(output + 32, cacheSetupDecodedLedger_);"
        ),
        "source identity validation": (
            "sourceIdentity_.headFingerprint != readLe64Prep(input + 36)"
        ),
        "generation validation": (
            "recordGeneration != checkpointSelection_.checkpoint.generation"
        ),
        "rejected resume fallback": "void PdfPreparation::rejectResumeState()",
    }
    combined_source = checkpoint_header + "\n" + preparation_source
    for label, needle in source_contract.items():
        if needle not in combined_source:
            raise AssertionError(f"frozen source lacks {label}")

    documentation_contract = {
        "checkpoint codec": "PRCP codec 3",
        "alternating checkpoint controls": (
            "two alternating fixed 96-byte control records"
        ),
        "journal byte field": (
            "`[21-23]` committed resume-data byte count"
        ),
        "resume phase byte": "`[53]` resume phase",
        "resume phase none": "`0=None`",
        "resume phase commit": "`1=CommitManifest`",
        "resume phase emit sections": "`2=AfterEmitSections`",
        "resume phase page": "`3=AfterPage`",
        "resume phase image": "`4=AfterImage`",
        "resume phase image repair": "`5=AfterImageRepair`",
        "resume journal path": "`gen_<generation>/resume.journal`",
        "discovery header": "`PDRH` version 1, 192 bytes",
        "sorted xref": "sorted 24-byte xref records",
        "explicit pages": "244-byte explicit page records",
        "discovery trailer": "`PDRT` version 1, 72 bytes",
        "page records": "`PRJR` version 2, 512 bytes each",
        "aggregate integrity": "aggregate CRC-32 and FNV-1a ledger",
        "source validation": "source identity",
        "generation validation": "generation",
        "committed prefix": "checkpoint's `journalBytes` prefix",
        "fallback": "falls back to a clean on-device preparation",
        "cleanup": "later cleanup removes the rejected generation",
    }
    normalized_documentation = re.sub(r"\s+", " ", documentation)
    for label, needle in documentation_contract.items():
        if re.sub(r"\s+", " ", needle) not in normalized_documentation:
            raise AssertionError(f"file-format documentation lacks {label}")


def find_pdf_word_index_gate(section_source: str):
    word_index_gate = PDF_WORD_INDEX_GATE.search(section_source)
    if word_index_gate is None:
        raise AssertionError("layout word index is not guarded to PDF documents")
    return word_index_gate


def replace_pdf_word_index_condition(
    section_source: str, replacement: str
) -> str:
    word_index_gate = find_pdf_word_index_gate(section_source)
    condition_start, condition_end = word_index_gate.span("condition")
    return (
        section_source[:condition_start]
        + replacement
        + section_source[condition_end:]
    )


def validate_pdf_word_index_scope(
    documentation: str,
    reflow_document_header: str,
    section_source: str,
) -> None:
    word_index_gate = find_pdf_word_index_gate(section_source)
    if re.sub(
        r"\s+", " ", word_index_gate.group("condition")
    ).strip() != PDF_WORD_INDEX_CONDITION:
        raise AssertionError("layout word index is not guarded to PDF documents")
    if (
        "default\n  // implementations keep EPUB documents entirely outside"
        not in reflow_document_header
    ):
        raise AssertionError("reflow interface lacks the EPUB no-op contract")
    for needle in (
        "These fixed-record indexes are PDF-only",
        "EPUB cache formats are unchanged",
    ):
        if needle not in documentation:
            raise AssertionError("file-format documentation lacks PDF-only scope")


def validate_release_wiring(
    platformio_source: str,
    root_cmake_source: str,
    ci_source: str,
    changelog_source: str,
    suite_names: set[str],
) -> None:
    parser = configparser.RawConfigParser(interpolation=None, strict=False)
    parser.read_string(platformio_source)
    if parser.get("crosspoint", "crossink_version").strip() != "1.5.0":
        raise AssertionError("release version is not 1.5.0")

    pdf_flag = "-DCROSSINK_ENABLE_PDF=1"
    enabled_environments = {
        section.removeprefix("env:")
        for section in parser.sections()
        if section.startswith("env:")
        and pdf_flag in parser.get(section, "build_flags", fallback="")
    }
    expected_environments = {
        "default",
        "tiny",
        "xlarge",
        "qemu-esp32c3",
        "simulator",
    }
    if enabled_environments != expected_environments:
        raise AssertionError("PDF feature flag environment scope changed")
    if pdf_flag in parser.get("base", "build_flags", fallback=""):
        raise AssertionError("PDF feature flag leaked into base")
    if pdf_flag in parser.get("env:debug", "build_flags", fallback=""):
        raise AssertionError("PDF feature flag leaked into debug")

    registrations = re.findall(
        r"add_subdirectory\(\s*([^\s)]+)\s*\)", root_cmake_source
    )
    if set(registrations) != suite_names:
        missing = sorted(suite_names - set(registrations))
        extra = sorted(set(registrations) - suite_names)
        raise AssertionError(
            f"root CMake suite registration mismatch: missing={missing}, extra={extra}"
        )
    duplicates = sorted(
        name for name in suite_names if registrations.count(name) != 1
    )
    if duplicates:
        raise AssertionError(f"root CMake suites not registered once: {duplicates}")

    ci_contract = {
        "host test job": "  host-tests:",
        "host configure": "cmake -S test -B build/host-tests",
        "host build": "cmake --build build/host-tests --parallel 2",
        "host CTest": (
            "ctest --test-dir build/host-tests --output-on-failure"
        ),
        "real no-flash gate": (
            "python scripts/verify_qemu_no_flash.py --pio pio"
        ),
        "required host status": "      - host-tests",
        "required QEMU status": "      - qemu-tracer",
    }
    for label, needle in ci_contract.items():
        if ci_source.count(needle) != 1:
            raise AssertionError(f"CI must contain exactly one {label}")

    match = re.search(
        r"\A## \[v1\.5\.0\] - 2026-07-31\n(.*?)(?=\n## \[)",
        changelog_source,
        flags=re.DOTALL,
    )
    if match is None:
        raise AssertionError("changelog lacks the v1.5.0 release entry")
    release_copy = match.group(1)
    for required in (
        "reflowed entirely on the device",
        "selected font, font size, margins, and orientation",
        "chapters, contents, internal links",
        "follows words read across the whole book",
        "Existing EPUB and other book formats keep their existing behavior",
    ):
        if required not in release_copy:
            raise AssertionError("changelog lacks a customer-visible PDF fact")
    for banned in (
        "QA",
        "release gate",
        "source inspected",
        "placeholder",
        "TODO",
        "scaffold",
    ):
        if banned.lower() in release_copy.lower():
            raise AssertionError("changelog contains internal process language")


def validate_pdf_navigation_dependencies(cmake_source: str) -> None:
    source_block = re.search(
        r"add_executable\(PdfNavigationTest(.*?)\n\)",
        cmake_source,
        flags=re.DOTALL,
    )
    if source_block is None:
        raise AssertionError("PdfNavigationTest lacks its source block")
    for required_source in (
        "${REPO_ROOT}/lib/PixelCache/PixelCache.cpp",
        "${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp",
        "${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp",
    ):
        if required_source not in source_block.group(1):
            raise AssertionError(
                f"PdfNavigationTest lacks required source {required_source}"
            )

    include_block = re.search(
        r"target_include_directories\(PdfNavigationTest PRIVATE(.*?)\n\)",
        cmake_source,
        flags=re.DOTALL,
    )
    if include_block is None:
        raise AssertionError("PdfNavigationTest lacks its private include block")
    if "${REPO_ROOT}/lib/PixelCache" not in include_block.group(1):
        raise AssertionError(
            "PdfNavigationTest lacks the PixelCache production dependency"
        )


def validate_pdf_navigation_cache_fixture(test_source: str) -> None:
    required_calls = (
        'addRequired("gen_7/cover.bmp", "BMcover");',
        'addRequired("gen_7/thumb.bmp", "BMthumb");',
    )
    positions = []
    for required_call in required_calls:
        position = test_source.find(required_call)
        if position < 0:
            raise AssertionError(
                "navigation cache fixture lacks a nonempty cover/thumbnail artifact"
            )
        positions.append(position)

    section_position = test_source.find(
        'addRequired("gen_7/sections/000001.xhtml", section1);'
    )
    metadata_position = test_source.find('addRequired("gen_7/metadata.bin"')
    if (
        section_position < 0
        or metadata_position < 0
        or not section_position < positions[0] < positions[1] < metadata_position
    ):
        raise AssertionError(
            "navigation cache fixture artifacts are not in manifest order"
        )


def validate_psit_item_id_contract(documentation: str) -> None:
    psit_start = documentation.find("### PSIT Version 1")
    psit_end = documentation.find("\n## ", psit_start)
    if psit_start < 0 or psit_end < 0:
        raise AssertionError("file-format documentation lacks the PSIT section")
    psit = documentation[psit_start:psit_end]
    for fact in (
        "unique within a slot",
        "range `1` through `65534`",
        "`0` and `65535` are invalid",
    ):
        if fact not in psit:
            raise AssertionError("PSIT item-ID validity contract is incomplete")


def valid_result(pass_name: str) -> dict:
    runner_nonce = "nonce-acceptance"
    source_identity = {
        "size": 1024,
        "modification_time": 7,
        "head_fingerprint": 11,
        "tail_fingerprint": 13,
    }
    checkpoint = {
        "name": "after_image_repair",
        "sequence": 9,
        "resume_phase": "after_image_repair",
        "last_verified_page": 2,
        "last_verified_object": 17,
        "emitted_sections": 2,
        "emitted_images": 1,
        "cumulative_words": 10,
        "output_bytes": 2048,
        "warning_flags": 0,
    }
    counter_snapshot = {
        "preparation_steps": 64,
        "parser_steps": 20,
        "page_steps": 30,
        "image_steps": 10,
        "xref_steps": 12,
        "pages_walked": 3,
        "content_tokens": 22,
        "sections_emitted": 2,
        "images_emitted": 1,
        "source_bytes_read": 900,
    }
    common_oracle = {
        "typography": {
            "pdf_6": {
                "text_hash": "0000000000000001",
                "page_count": 2,
                "first_frame": "0000000000000011",
                "middle_frame": "0000000000000012",
                "last_frame": "0000000000000013",
            },
            "pdf_72": {
                "text_hash": "0000000000000001",
                "page_count": 2,
                "first_frame": "0000000000000011",
                "middle_frame": "0000000000000012",
                "last_frame": "0000000000000013",
            },
            "device_font_positive": {
                "text_hash": "0000000000000001",
                "page_count": 3,
                "first_frame": "0000000000000021",
                "middle_frame": "0000000000000022",
                "last_frame": "0000000000000023",
            },
            "fixed_page_canvas": False,
        },
        "navigation": {
            "sections": 2,
            "toc_entries": 3,
            "internal_links": 2,
            "resolved_internal_links": 2,
            "publisher_labels": 2,
            "toc_hash": "0000000000000031",
        },
        "image": {
            "retained": 1,
            "frame_hash": "0000000000000041",
            "blank_hash": "0000000000000042",
            "page_image_found": True,
            "rect": [12, 24, 100, 80],
            "non_white_pixels": 200,
            "region_hash": "0000000000000043",
        },
        "progress": {
            "total_words": 10,
            "nonterminal_saved_cursor": 6,
            "nonterminal_resumed_cursor": 6,
            "nonterminal_percent_millionths": 600000,
            "saved_cursor": 10,
            "resumed_cursor": 10,
            "percent_millionths": 1000000,
        },
        "layout_controls": {
            "portrait_frame": "0000000000000011",
            "landscape_frame": "0000000000000051",
            "wide_margin_frame": "0000000000000052",
        },
        "route": {
            "raw_pdf": True,
            "on_device_preparation": True,
            "cancelled": True,
            "resumed": True,
            "cached_reopen": True,
        },
    }
    result = {
        "schema_version": 1,
        "pass": pass_name,
        "phase": "resume" if pass_name == "uncached" else "cached",
        "runner_nonce": runner_nonce,
        "oracle": common_oracle,
        "negative": {"checked": 4, "rejected": 4},
        "counters": {
            "preparation_steps": 100 if pass_name == "uncached" else 0,
            "extraction_runs": 4 if pass_name == "uncached" else 0,
            "parser_calls": 100 if pass_name == "uncached" else 0,
            "source_open_calls": 1,
            "source_read_calls": 2,
            "source_read_bytes": 8192,
            "source_max_read": 4096,
            "cached_page_turns": 100 if pass_name == "cached" else 0,
            "page_turn_source_opens": 0,
            "page_turn_source_reads": 0,
            "io_calls": 500,
            "max_io_request": 4096,
            "yielded_slices": 50 if pass_name == "uncached" else 0,
            "cancelled_runs": 0,
            "resumed_runs": 1 if pass_name == "uncached" else 0,
            "cancellation_steps": 0,
            "cancellation_elapsed_ms": 0,
            "cancellation_max_slice_ms": 0,
            "cancellation_max_slice_io_calls": 0,
            "fresh_baseline_steps": 80 if pass_name == "uncached" else 0,
            "fresh_baseline_parser_steps": (
                20 if pass_name == "uncached" else 0
            ),
            "fresh_baseline_page_steps": (
                30 if pass_name == "uncached" else 0
            ),
            "fresh_baseline_image_steps": (
                10 if pass_name == "uncached" else 0
            ),
            "resumed_preparation_steps": (
                24 if pass_name == "uncached" else 0
            ),
            "resumed_parser_steps": 4 if pass_name == "uncached" else 0,
            "resumed_page_steps": 8 if pass_name == "uncached" else 0,
            "resumed_image_steps": 2 if pass_name == "uncached" else 0,
            "max_actual_step_us": 7000 if pass_name == "uncached" else 0,
            "framebuffer_guard_checks": 5 if pass_name == "uncached" else 0,
            "framebuffer_guard_failures": 0,
            "framebuffer_guard_controls": 10 if pass_name == "uncached" else 0,
            "framebuffer_guard_rejections": 10 if pass_name == "uncached" else 0,
        },
    }
    if pass_name == "uncached":
        result["continuity"] = {
            "checkpoint_name": "after_image_repair",
            "source_identity": source_identity,
            "generation": 42,
            "resumed_checkpoint": checkpoint,
            "counter_snapshot": counter_snapshot,
            "resumed_from_checkpoint": True,
        }
    return result


def valid_cancel_result() -> dict:
    return {
        "schema_version": 1,
        "pass": "uncached",
        "phase": "cancel",
        "runner_nonce": "nonce-acceptance",
        "continuity": {
            "checkpoint_name": "after_image_repair",
            "source_identity": {
                "size": 1024,
                "modification_time": 7,
                "head_fingerprint": 11,
                "tail_fingerprint": 13,
            },
            "generation": 42,
            "checkpoint": {
                "name": "after_image_repair",
                "sequence": 9,
                "resume_phase": "after_image_repair",
                "last_verified_page": 2,
                "last_verified_object": 17,
                "emitted_sections": 2,
                "emitted_images": 1,
                "cumulative_words": 10,
                "output_bytes": 2048,
                "warning_flags": 0,
            },
            "counter_snapshot": {
                "preparation_steps": 64,
                "parser_steps": 20,
                "page_steps": 30,
                "image_steps": 10,
                "xref_steps": 12,
                "pages_walked": 3,
                "content_tokens": 22,
                "sections_emitted": 2,
                "images_emitted": 1,
                "source_bytes_read": 900,
            },
        },
        "counters": {
            "cancellation_steps": 12,
            "cancellation_elapsed_ms": 48,
            "cancellation_max_slice_ms": 8,
            "cancellation_max_slice_io_calls": 4,
            "max_actual_step_us": 7000,
            "framebuffer_guard_checks": 1,
            "framebuffer_guard_failures": 0,
            "framebuffer_guard_controls": 2,
            "framebuffer_guard_rejections": 2,
        },
    }


def validate_pair(
    module,
    uncached: dict,
    cached: dict,
    expected_oracle: dict,
    cancelled: dict | None = None,
) -> None:
    module.validate_acceptance(
        valid_cancel_result() if cancelled is None else cancelled,
        uncached,
        cached,
        expected_oracle,
    )


class PdfSimulatorAcceptanceTest(unittest.TestCase):
    def test_borrowed_pdf_oracle_keeps_canonical_absolute_image_href(
        self,
    ) -> None:
        source = EPUB_REGRESSION_SOURCE.read_text(encoding="utf-8")
        validate_loose_local_resource_contract(source)

        root_stripping_mutation = source.replace(
            "imageHref_(imagePath_)",
            "imageHref_(imagePath_.empty() || imagePath_.front() != '/' "
            "? imagePath_ : imagePath_.substr(1))",
            1,
        )
        self.assertNotEqual(root_stripping_mutation, source)
        with self.assertRaisesRegex(AssertionError, "canonical absolute"):
            validate_loose_local_resource_contract(root_stripping_mutation)

    def test_native_framebuffer_guard_has_only_safe_static_contracts(self) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        header = FRAMEBUFFER_GUARD_HEADER.read_text(encoding="utf-8")
        validate_runtime_framebuffer_guard_symbols(source, header)

    def test_native_framebuffer_guard_executes_baseline_and_mutation_controls(
        self,
    ) -> None:
        header = FRAMEBUFFER_GUARD_HEADER.read_text(encoding="utf-8")
        suppressed_increment = header.replace(
            "++violations;", "(void)violations;", 1
        )
        hardwired_predicate = header.replace(
            "pdfAcceptanceFramebufferUnchanged(expected, observed)", "true", 1
        )
        self.assertNotEqual(suppressed_increment, header)
        self.assertNotEqual(hardwired_predicate, header)

        variants = {
            "framebuffer_baseline": header,
            "framebuffer_suppressed_increment": suppressed_increment,
            "framebuffer_hardwired_predicate": hardwired_predicate,
        }
        with tempfile.TemporaryDirectory() as temporary_directory:
            completed = compile_and_run_framebuffer_behavior_variants(
                variants, Path(temporary_directory)
            )
        for phase, result in zip(("configure", "build", "ctest"), completed):
            with self.subTest(phase=phase):
                self.assertEqual(
                    result.returncode,
                    0,
                    result.stdout + "\n" + result.stderr,
                )

    def test_release_documents_match_frozen_checkpoint_and_resume_codecs(
        self,
    ) -> None:
        documentation = FILE_FORMATS.read_text(encoding="utf-8")
        checkpoint_header = CHECKPOINT_HEADER.read_text(encoding="utf-8")
        preparation_source = PREPARATION_SOURCE.read_text(encoding="utf-8")
        validate_checkpoint_and_resume_documentation(
            documentation, checkpoint_header, preparation_source
        )

        stale_codec = documentation.replace("PRCP codec 3", "PRCP codec 2", 1)
        self.assertNotEqual(stale_codec, documentation)
        with self.assertRaisesRegex(AssertionError, "checkpoint codec"):
            validate_checkpoint_and_resume_documentation(
                stale_codec, checkpoint_header, preparation_source
            )

        wrong_phase = documentation.replace(
            "`5=AfterImageRepair`", "`6=AfterImageRepair`"
        )
        self.assertNotEqual(wrong_phase, documentation)
        with self.assertRaisesRegex(AssertionError, "image repair"):
            validate_checkpoint_and_resume_documentation(
                wrong_phase, checkpoint_header, preparation_source
            )

        wrong_page_size = documentation.replace(
            "244-byte explicit page records",
            "245-byte explicit page records",
            1,
        )
        self.assertNotEqual(wrong_page_size, documentation)
        with self.assertRaisesRegex(AssertionError, "explicit pages"):
            validate_checkpoint_and_resume_documentation(
                wrong_page_size, checkpoint_header, preparation_source
            )

        source_drift = checkpoint_header.replace(
            "PDF_BUILD_CHECKPOINT_CODEC_VERSION = 3",
            "PDF_BUILD_CHECKPOINT_CODEC_VERSION = 4",
            1,
        )
        self.assertNotEqual(source_drift, checkpoint_header)
        with self.assertRaisesRegex(AssertionError, "checkpoint codec 3"):
            validate_checkpoint_and_resume_documentation(
                documentation, source_drift, preparation_source
            )

    def test_fixed_layout_word_index_is_pdf_only(self) -> None:
        documentation = FILE_FORMATS.read_text(encoding="utf-8")
        reflow_header = REFLOW_DOCUMENT_HEADER.read_text(encoding="utf-8")
        section_source = SECTION_SOURCE.read_text(encoding="utf-8")
        validate_pdf_word_index_scope(
            documentation, reflow_header, section_source
        )

        production_gate = find_pdf_word_index_gate(section_source)
        whitespace_gate = (
            "bool Section :: usesPdfWordIndex ( ) const\n"
            "{\n"
            "  return document->getFormat() == ReflowDocumentFormat::Pdf\n"
            "      && filePath.find(\"_fn_\") == std::string::npos\n"
            "      ;\n"
            "}"
        )
        whitespace_variant = (
            section_source[:production_gate.start()]
            + whitespace_gate
            + section_source[production_gate.end():]
        )
        self.assertNotEqual(whitespace_variant, section_source)
        mutations = {
            "removed": 'filePath.find("_fn_") == std::string::npos',
            "widened to EPUB": (
                "(document->getFormat() == ReflowDocumentFormat::Pdf || "
                "document->getFormat() == ReflowDocumentFormat::Epub) && "
                'filePath.find("_fn_") == std::string::npos'
            ),
        }
        for source_label, source_variant in (
            ("production", section_source),
            ("whitespace variant", whitespace_variant),
        ):
            with self.subTest(source=source_label):
                validate_pdf_word_index_scope(
                    documentation, reflow_header, source_variant
                )
            for mutation_label, mutation in mutations.items():
                with self.subTest(
                    source=source_label, mutation=mutation_label
                ):
                    mutated_source = replace_pdf_word_index_condition(
                        source_variant, mutation
                    )
                    self.assertNotEqual(mutated_source, source_variant)
                    with self.assertRaisesRegex(
                        AssertionError, "guarded to PDF"
                    ):
                        validate_pdf_word_index_scope(
                            documentation, reflow_header, mutated_source
                        )

    def test_pdf_navigation_declares_production_dependencies(
        self,
    ) -> None:
        cmake_source = PDF_NAVIGATION_CMAKE.read_text(encoding="utf-8")
        validate_pdf_navigation_dependencies(cmake_source)

        for required_source in (
            "${REPO_ROOT}/lib/PixelCache/PixelCache.cpp",
            "${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp",
            "${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp",
        ):
            removed_source = cmake_source.replace(
                f"  {required_source}\n", "", 1
            )
            self.assertNotEqual(removed_source, cmake_source)
            with self.assertRaisesRegex(AssertionError, "required source"):
                validate_pdf_navigation_dependencies(removed_source)

        removed_dependency = cmake_source.replace(
            "  ${REPO_ROOT}/lib/PixelCache\n", "", 1
        )
        self.assertNotEqual(removed_dependency, cmake_source)
        with self.assertRaisesRegex(AssertionError, "PixelCache"):
            validate_pdf_navigation_dependencies(removed_dependency)

    def test_pdf_navigation_fixture_matches_manifest_artifact_order(
        self,
    ) -> None:
        test_source = PDF_NAVIGATION_TEST.read_text(encoding="utf-8")
        validate_pdf_navigation_cache_fixture(test_source)

        for artifact in (
            '  addRequired("gen_7/cover.bmp", "BMcover");\n',
            '  addRequired("gen_7/thumb.bmp", "BMthumb");\n',
        ):
            removed_artifact = test_source.replace(artifact, "", 1)
            with self.assertRaisesRegex(AssertionError, "nonempty"):
                validate_pdf_navigation_cache_fixture(removed_artifact)

    def test_psit_item_ids_document_live_validation_limits(self) -> None:
        documentation = FILE_FORMATS.read_text(encoding="utf-8")
        validate_psit_item_id_contract(documentation)

        for fact in (
            "unique within a slot",
            "range `1` through `65534`",
            "`0` and `65535` are invalid",
        ):
            mutated = documentation.replace(fact, "removed", 1)
            self.assertNotEqual(mutated, documentation)
            with self.assertRaisesRegex(AssertionError, "validity contract"):
                validate_psit_item_id_contract(mutated)

    def test_release_wiring_registers_every_suite_and_required_ci_gate(
        self,
    ) -> None:
        platformio_source = PLATFORMIO_CONFIG.read_text(encoding="utf-8")
        root_cmake_source = ROOT_TEST_CMAKE.read_text(encoding="utf-8")
        ci_source = CI_WORKFLOW.read_text(encoding="utf-8")
        changelog_source = CHANGELOG.read_text(encoding="utf-8")
        suite_names = {
            path.parent.name
            for path in (REPO_ROOT / "test").glob("*/CMakeLists.txt")
        }
        validate_release_wiring(
            platformio_source,
            root_cmake_source,
            ci_source,
            changelog_source,
            suite_names,
        )

        missing_flag = platformio_source.replace(
            "[env:default]\nextends = base\nbuild_flags =\n"
            "  ${base.build_flags}\n  -DCROSSINK_ENABLE_PDF=1\n",
            "[env:default]\nextends = base\nbuild_flags =\n"
            "  ${base.build_flags}\n",
            1,
        )
        self.assertNotEqual(missing_flag, platformio_source)
        with self.assertRaisesRegex(AssertionError, "environment scope"):
            validate_release_wiring(
                missing_flag,
                root_cmake_source,
                ci_source,
                changelog_source,
                suite_names,
            )

        missing_suite = root_cmake_source.replace(
            "add_subdirectory(book_state_migration)\n", "", 1
        )
        self.assertNotEqual(missing_suite, root_cmake_source)
        with self.assertRaisesRegex(AssertionError, "missing"):
            validate_release_wiring(
                platformio_source,
                missing_suite,
                ci_source,
                changelog_source,
                suite_names,
            )

        no_flash_gap = ci_source.replace(
            "python scripts/verify_qemu_no_flash.py",
            "python -m unittest test.qemu.test_no_flash",
            1,
        )
        self.assertNotEqual(no_flash_gap, ci_source)
        with self.assertRaisesRegex(AssertionError, "no-flash gate"):
            validate_release_wiring(
                platformio_source,
                root_cmake_source,
                no_flash_gap,
                changelog_source,
                suite_names,
            )

    def test_runner_requires_cross_process_resume_continuity_and_real_savings(
        self,
    ) -> None:
        module = self._load_runner()
        cancelled = valid_cancel_result()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        module.validate_acceptance(
            cancelled,
            uncached,
            cached,
            uncached["oracle"],
        )

        mutations = (
            (
                "source identity",
                lambda result: result["continuity"]["source_identity"].__setitem__(
                    "head_fingerprint", 99
                ),
            ),
            (
                "generation",
                lambda result: result["continuity"].__setitem__(
                    "generation", 99
                ),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("sequence", 10),
            ),
            (
                "counter snapshot",
                lambda result: result["continuity"]["counter_snapshot"].__setitem__(
                    "preparation_steps", 65
                ),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("last_verified_page", 3),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("last_verified_object", 18),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("emitted_sections", 3),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("emitted_images", 2),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("cumulative_words", 11),
            ),
            (
                "checkpoint",
                lambda result: result["continuity"][
                    "resumed_checkpoint"
                ].__setitem__("output_bytes", 2049),
            ),
        )
        for expected_error, mutate in mutations:
            with self.subTest(expected_error=expected_error):
                changed = valid_result("uncached")
                mutate(changed)
                with self.assertRaisesRegex(ValueError, expected_error):
                    module.validate_acceptance(
                        valid_cancel_result(),
                        changed,
                        valid_result("cached"),
                        changed["oracle"],
                    )

        weak_resume = valid_result("uncached")
        weak_resume["counters"]["fresh_baseline_parser_steps"] = 20
        weak_resume["counters"]["resumed_parser_steps"] = 19
        weak_resume["counters"]["fresh_baseline_page_steps"] = 30
        weak_resume["counters"]["resumed_page_steps"] = 30
        weak_resume["counters"]["fresh_baseline_image_steps"] = 10
        weak_resume["counters"]["resumed_image_steps"] = 10
        with self.assertRaisesRegex(ValueError, "every completed work category"):
            module.validate_acceptance(
                valid_cancel_result(),
                weak_resume,
                valid_result("cached"),
                weak_resume["oracle"],
            )

        missing_cursor = valid_cancel_result()
        del missing_cursor["continuity"]["checkpoint"]["last_verified_page"]
        matching_resume = valid_result("uncached")
        del matching_resume["continuity"]["resumed_checkpoint"][
            "last_verified_page"
        ]
        with self.assertRaisesRegex(ValueError, "checkpoint cursor"):
            module.validate_acceptance(
                missing_cursor,
                matching_resume,
                valid_result("cached"),
                matching_resume["oracle"],
            )

        missing_work = valid_cancel_result()
        del missing_work["continuity"]["counter_snapshot"]["images_emitted"]
        matching_resume = valid_result("uncached")
        del matching_resume["continuity"]["counter_snapshot"][
            "images_emitted"
        ]
        with self.assertRaisesRegex(ValueError, "checkpoint work snapshot"):
            module.validate_acceptance(
                missing_work,
                matching_resume,
                valid_result("cached"),
                matching_resume["oracle"],
            )

    def test_runner_contract_rejects_a_cached_parser_call(self) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        validate_pair(module, uncached, cached, uncached["oracle"])

        cached["counters"]["parser_calls"] = 1
        with self.assertRaisesRegex(
            ValueError, "cached pass invoked the PDF parser"
        ):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_has_frame_and_font_positive_controls(self) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")

        cached["oracle"]["typography"]["pdf_72"]["last_frame"] = (
            "0000000000000099"
        )
        with self.assertRaisesRegex(ValueError, "uncached/cached oracle"):
            validate_pair(module, uncached, cached, uncached["oracle"])

        cached = valid_result("cached")
        for result in (uncached, cached):
            result["oracle"]["typography"]["device_font_positive"] = dict(
                result["oracle"]["typography"]["pdf_6"]
            )
        with self.assertRaisesRegex(
            ValueError, "device font positive control"
        ):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_rejects_fixed_canvas_and_unbounded_identity_io(
        self,
    ) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        cached["oracle"]["typography"]["fixed_page_canvas"] = True
        uncached["oracle"]["typography"]["fixed_page_canvas"] = True
        with self.assertRaisesRegex(ValueError, "fixed PDF page canvas"):
            validate_pair(module, uncached, cached, uncached["oracle"])

        uncached = valid_result("uncached")
        cached = valid_result("cached")
        cached["counters"]["source_read_calls"] = 3
        with self.assertRaisesRegex(ValueError, "identity reads"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_requires_reopened_sixty_percent_and_terminal_progress(
        self,
    ) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        validate_pair(module, uncached, cached, uncached["oracle"])

        for field, value in (
            ("nonterminal_resumed_cursor", 5),
            ("nonterminal_percent_millionths", 500000),
            ("saved_cursor", 9),
        ):
            changed_uncached = valid_result("uncached")
            changed_cached = valid_result("cached")
            changed_uncached["oracle"]["progress"][field] = value
            changed_cached["oracle"]["progress"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(
                ValueError, "word progress"
            ):
                validate_pair(
                    module,
                    changed_uncached,
                    changed_cached,
                    changed_uncached["oracle"],
                )

    def test_runner_requires_runtime_framebuffer_guard_evidence(self) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        validate_pair(module, uncached, cached, uncached["oracle"])

        uncached["counters"]["framebuffer_guard_checks"] = 0
        with self.assertRaisesRegex(ValueError, "framebuffer"):
            validate_pair(module, uncached, cached, uncached["oracle"])

        for field, value in (
            ("framebuffer_guard_controls", 9),
            ("framebuffer_guard_rejections", 9),
        ):
            with self.subTest(field=field):
                uncached = valid_result("uncached")
                cached = valid_result("cached")
                uncached["counters"][field] = value
                with self.assertRaisesRegex(ValueError, "framebuffer positive controls"):
                    validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_requires_route_and_layout_positive_controls(
        self,
    ) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        for result in (uncached, cached):
            result["oracle"]["route"]["on_device_preparation"] = False
        with self.assertRaisesRegex(ValueError, "on-device PDF route"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_requires_every_internal_href_fragment_to_resolve(
        self,
    ) -> None:
        module = self._load_runner()
        cancelled = valid_cancel_result()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        uncached["oracle"]["navigation"]["resolved_internal_links"] = 1
        cached["oracle"]["navigation"]["resolved_internal_links"] = 1
        with self.assertRaisesRegex(ValueError, "href/fragment"):
            module.validate_acceptance(
                cancelled,
                uncached,
                cached,
                uncached["oracle"],
            )

        uncached = valid_result("uncached")
        cached = valid_result("cached")
        for result in (uncached, cached):
            controls = result["oracle"]["layout_controls"]
            controls["landscape_frame"] = controls["portrait_frame"]
        with self.assertRaisesRegex(ValueError, "orientation"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_requires_page_image_pixels_not_layout_only(
        self,
    ) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        for result in (uncached, cached):
            result["oracle"]["image"]["page_image_found"] = False
        with self.assertRaisesRegex(ValueError, "PageImage"):
            validate_pair(module, uncached, cached, uncached["oracle"])

        uncached = valid_result("uncached")
        cached = valid_result("cached")
        for result in (uncached, cached):
            result["oracle"]["image"]["non_white_pixels"] = 0
        with self.assertRaisesRegex(ValueError, "non-white"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_requires_cancel_resume_witness(self) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        uncached["counters"]["resumed_runs"] = 0
        with self.assertRaisesRegex(ValueError, "cancel/resume"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_resume_process_cannot_claim_prior_cancel_process_work(self) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        validate_pair(module, uncached, cached, uncached["oracle"])

        uncached["counters"]["cancelled_runs"] = 1
        uncached["counters"]["cancellation_steps"] = 12
        with self.assertRaisesRegex(ValueError, "resume process.*cancellation"):
            validate_pair(module, uncached, cached, uncached["oracle"])

    def test_runner_contract_requires_bounded_cooperative_cancellation(
        self,
    ) -> None:
        module = self._load_runner()
        uncached = valid_result("uncached")
        cached = valid_result("cached")
        validate_pair(module, uncached, cached, uncached["oracle"])

        one_step = valid_cancel_result()
        one_step["counters"]["cancellation_steps"] = 1
        with self.assertRaisesRegex(
            ValueError, "cooperative cancellation"
        ):
            validate_pair(
                module, uncached, cached, uncached["oracle"], one_step
            )

        exhausted = valid_cancel_result()
        exhausted["counters"]["cancellation_steps"] = 257
        with self.assertRaisesRegex(ValueError, "cancellation slice cap"):
            validate_pair(
                module, uncached, cached, uncached["oracle"], exhausted
            )

        over_time = valid_cancel_result()
        over_time["counters"]["cancellation_max_slice_ms"] = 9
        with self.assertRaisesRegex(ValueError, "8 ms"):
            validate_pair(
                module, uncached, cached, uncached["oracle"], over_time
            )

        over_operations = valid_cancel_result()
        over_operations["counters"][
            "cancellation_max_slice_io_calls"
        ] = 33
        with self.assertRaisesRegex(ValueError, "32 operations"):
            validate_pair(
                module,
                uncached,
                cached,
                uncached["oracle"],
                over_operations,
            )

        full_rebuild = valid_result("uncached")
        for resumed_name, fresh_name in (
            ("resumed_preparation_steps", "fresh_baseline_steps"),
            ("resumed_parser_steps", "fresh_baseline_parser_steps"),
            ("resumed_page_steps", "fresh_baseline_page_steps"),
            ("resumed_image_steps", "fresh_baseline_image_steps"),
        ):
            full_rebuild["counters"][resumed_name] = full_rebuild[
                "counters"
            ][fresh_name]
        with self.assertRaisesRegex(ValueError, "every completed work category"):
            validate_pair(module, full_rebuild, cached, full_rebuild["oracle"])

    def test_native_cancellation_driver_is_cooperative_and_bounded(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_cooperative_cancellation_source(source)

    def test_native_cancellation_mutations_reject_one_step_and_cap_escape(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_cooperative_cancellation_source(source)

        one_step = source.replace(
            "cancellationSlice < kMaximumCancellationSlices",
            "cancellationSlice < 1",
            1,
        )
        self.assertNotEqual(one_step, source)
        with self.assertRaisesRegex(
            AssertionError, "bounded cooperative cancellation loop"
        ):
            validate_cooperative_cancellation_source(one_step)

        cap_escape = source.replace(
            'error = "preparation cancellation exhausted bounded slices";',
            'error = "preparation cancellation unexpectedly accepted";',
            1,
        )
        self.assertNotEqual(cap_escape, source)
        with self.assertRaisesRegex(
            AssertionError, "cap exhaustion failure"
        ):
            validate_cooperative_cancellation_source(cap_escape)

    def test_native_step_budget_uses_actual_time_and_rejects_frozen_clock(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_measured_step_budget_source(source)

        nonadvancing_clock = source.replace(
            "return clock.nowMs++;",
            "return clock.nowMs;",
            1,
        )
        self.assertNotEqual(nonadvancing_clock, source)
        with self.assertRaisesRegex(
            AssertionError, "deterministic synthetic tick"
        ):
            validate_measured_step_budget_source(nonadvancing_clock)

        synthetic_only = source.replace(
            "std::chrono::steady_clock::now()",
            "syntheticClockOnly()",
        )
        self.assertNotEqual(synthetic_only, source)
        with self.assertRaisesRegex(AssertionError, "steady clock measurement"):
            validate_measured_step_budget_source(synthetic_only)

        bypassed_gateway = source.replace(
            (
                "if (!measuredPreparationStep(preparation, clock, counters, "
                "result, error, work))"
            ),
            "result = preparation.step();\n    if (false)",
            1,
        )
        self.assertNotEqual(bypassed_gateway, source)
        with self.assertRaisesRegex(AssertionError, "measured step gateway"):
            validate_measured_step_budget_source(bypassed_gateway)

    def test_native_navigation_resolves_each_href_fragment_to_a_real_anchor(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_internal_link_target_source(source)

        count_only = source.replace(
            "target.find(anchor) == std::string::npos",
            "false",
            1,
        )
        self.assertNotEqual(count_only, source)
        with self.assertRaisesRegex(AssertionError, "target anchor lookup"):
            validate_internal_link_target_source(count_only)

        unused_validator = source.replace(
            (
                "validateEveryInternalLinkTarget(*document, sectionContents, "
                "resolvedInternalLinks, error)"
            ),
            "true",
            1,
        )
        self.assertNotEqual(unused_validator, source)
        with self.assertRaisesRegex(AssertionError, "resolver invocation"):
            validate_internal_link_target_source(unused_validator)

    def test_native_image_witness_inspects_page_image_rectangle_pixels(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_page_image_pixel_source(source)

        layout_only = source.replace(
            "element->getTag() == TAG_PageImage",
            "element->getTag() == TAG_PageLine",
            1,
        )
        self.assertNotEqual(layout_only, source)
        with self.assertRaisesRegex(AssertionError, "PageImage inspection"):
            validate_page_image_pixel_source(layout_only)

        unused_capture = source.replace(
            "!captureImage(imageDocument, renderer, cachedPass, counters,",
            "false && captureImage(imageDocument, renderer, cachedPass, counters,",
            1,
        )
        self.assertNotEqual(unused_capture, source)
        with self.assertRaisesRegex(AssertionError, "image capture invocation"):
            validate_page_image_pixel_source(unused_capture)

    def test_native_progress_oracle_calls_production_word_progress(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_production_progress_source(source)

        hand_rolled = source.replace(
            (
                "pdfCalculateWordCursorProgress(nonTerminalResumed.wordCursor, totalWords, "
                "&wordProgress)"
            ),
            "nonTerminalResumed.wordCursor <= totalWords",
            1,
        )
        self.assertNotEqual(hand_rolled, source)
        with self.assertRaisesRegex(
            AssertionError, "production word progress call"
        ):
            validate_production_progress_source(hand_rolled)

        unused_progress = source.replace(
            "!captureNavigation(navigationDocument, renderer, cachedPass, counters,",
            "false && captureNavigation(navigationDocument, renderer, cachedPass, counters,",
            1,
        )
        self.assertNotEqual(unused_progress, source)
        with self.assertRaisesRegex(AssertionError, "progress capture invocation"):
            validate_production_progress_source(unused_progress)

        mutations = {
            "cursor forced terminal": source.replace(
                "nonTerminal.wordCursor = targetCursor;",
                "nonTerminal.wordCursor = totalWords;",
                1,
            ),
            "non-terminal equality bypassed": source.replace(
                "pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed)",
                "true",
                1,
            ),
            "terminal equality bypassed": source.replace(
                "pdfReadingPositionsEqualExact(selected, resumed)",
                "true",
                1,
            ),
            "non-terminal reload replaced by self-assignment": source.replace(
                "!document->loadReadingPosition(nonTerminalResumed) ||",
                "(nonTerminalResumed = nonTerminal, false) ||",
                1,
            ),
        }
        for label, mutation in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutation, source)
                with self.assertRaisesRegex(AssertionError, "progress|cursor|persistence"):
                    validate_production_progress_source(mutation)

    def test_native_cancel_and_resume_are_separate_fs_only_process_phases(
        self,
    ) -> None:
        source = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        validate_cross_process_native_source(source)

        same_process_resume = source.replace(
            'kResumePhase[] = "resume"',
            'kResumePhase[] = "cancel"',
            1,
        )
        self.assertNotEqual(same_process_resume, source)
        with self.assertRaisesRegex(AssertionError, "resume phase"):
            validate_cross_process_native_source(same_process_resume)

        no_persisted_evidence = source.replace(
            "writeResumeEvidence(",
            "discardResumeEvidence(",
        )
        self.assertNotEqual(no_persisted_evidence, source)
        with self.assertRaisesRegex(AssertionError, "persisted cancel evidence"):
            validate_cross_process_native_source(no_persisted_evidence)

    def test_marker_parser_requires_exactly_one_matching_result(self) -> None:
        module = self._load_runner()
        result = valid_result("cached")
        result["phase"] = "cached"
        result["runner_nonce"] = "nonce-1234"
        output = (
            "normal simulator log\n"
            f'{module.RESET_MARKER}{{"phase":"cached","runner_nonce":"nonce-1234"}}\n'
            f"{module.RESULT_MARKER}{json.dumps(result)}\n"
        )
        self.assertEqual(
            module.parse_result(output, "cached", "cached", "nonce-1234"),
            result,
        )
        with self.assertRaisesRegex(ValueError, "exactly one"):
            module.parse_result(output + output, "cached", "cached", "nonce-1234")
        with self.assertRaisesRegex(ValueError, "pass mismatch"):
            module.parse_result(output, "uncached", "cached", "nonce-1234")

        evidence_before_reset = (
            f"{module.RESULT_MARKER}{json.dumps(result)}\n"
            f'{module.RESET_MARKER}{{"phase":"cached","runner_nonce":"nonce-1234"}}\n'
        )
        with self.assertRaisesRegex(ValueError, "before reset"):
            module.parse_result(
                evidence_before_reset,
                "cached",
                "cached",
                "nonce-1234",
            )

        wrong_phase = output.replace('"phase":"cached"', '"phase":"resume"', 1)
        with self.assertRaisesRegex(ValueError, "reset phase"):
            module.parse_result(
                wrong_phase,
                "cached",
                "cached",
                "nonce-1234",
            )

        wrong_nonce = output.replace("nonce-1234", "stale-nonce", 1)
        with self.assertRaisesRegex(ValueError, "reset nonce"):
            module.parse_result(
                wrong_nonce,
                "cached",
                "cached",
                "nonce-1234",
            )

    def test_staging_preserves_raw_pdf_bytes(self) -> None:
        module = self._load_runner()
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            source = root / "source.pdf"
            destination = root / "fs_" / "books" / "source.pdf"
            payload = b"%PDF-1.7\r\nraw\x00fixture\r\n%%EOF\r\n"
            source.write_bytes(payload)
            digest = module.stage_raw_fixture(source, destination)
            self.assertEqual(destination.read_bytes(), payload)
            self.assertEqual(
                module.sha256_file(destination),
                digest,
            )
            destination.write_bytes(payload + b"changed")
            with self.assertRaisesRegex(
                ValueError, "raw staged PDF changed"
            ):
                module.verify_staged_fixture(destination, digest)

    def test_fresh_resume_baseline_is_a_byte_identical_second_path(self) -> None:
        module = self._load_runner()
        self.assertEqual(
            module.FRESH_RESUME_BASELINE_SOURCE,
            module.CANCEL_RESUME_FIXTURE,
        )
        self.assertNotEqual(
            module.FRESH_RESUME_BASELINE_NAME,
            module.CANCEL_RESUME_FIXTURE,
        )
        self.assertEqual(
            module.FRESH_RESUME_BASELINE_NAME,
            "raster_cover_caption_fresh.pdf",
        )

    def test_locked_oracle_gate_and_deterministic_generation_command(
        self,
    ) -> None:
        module = self._load_runner()
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            missing = root / "missing.oracle.json"
            with self.assertRaisesRegex(ValueError, "cannot read PDF oracle"):
                module._load_oracle(missing)

            invalid = root / "invalid.oracle.json"
            invalid.write_text(
                '{"schema_version":2,"oracle":{}}\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "schema_version 1"):
                module._load_oracle(invalid)

            valid = root / "valid.oracle.json"
            valid.write_text(
                '{"schema_version":1,"oracle":{"locked":true}}\n',
                encoding="utf-8",
            )
            self.assertEqual(module._load_oracle(valid), {"locked": True})

            program = root / "isolated" / "program"
            command = module.oracle_generation_command(program, valid)
            self.assertEqual(
                command,
                [
                    sys.executable,
                    str(RUNNER),
                    "--program",
                    str(program.resolve()),
                    "--no-build",
                    "--headless",
                    "--oracle",
                    str(valid.resolve()),
                    "--update-oracle",
                ],
            )
            self.assertEqual(
                command,
                module.oracle_generation_command(program, valid),
            )

        if module.DEFAULT_ORACLE.is_file():
            module._load_oracle(module.DEFAULT_ORACLE)
        else:
            generation = module.oracle_generation_command(
                module.DEFAULT_PROGRAM,
                module.DEFAULT_ORACLE,
            )
            self.assertIn("--update-oracle", generation)

        runner_source = RUNNER.read_text(encoding="utf-8")
        validate_oracle_publish_order_source(runner_source)

    def test_runner_splits_cancel_resume_and_cached_into_separate_processes(
        self,
    ) -> None:
        module = self._load_runner()
        calls: list[tuple[Path, Path, str, str, str, str]] = []

        def fake_run_program(
            program: Path,
            cwd: Path,
            fixture_root: str,
            pass_name: str,
            phase: str,
            runner_nonce: str,
            headless: bool,
            timeout: int,
        ) -> dict:
            self.assertTrue(headless)
            self.assertEqual(timeout, 90)
            calls.append(
                (
                    program,
                    cwd,
                    fixture_root,
                    pass_name,
                    phase,
                    runner_nonce,
                )
            )
            return {
                "pass": pass_name,
                "phase": phase,
                "runner_nonce": runner_nonce,
            }

        module._run_program = fake_run_program
        program = REPO_ROOT / ".tools" / "isolated-simulator" / "program"
        cwd = REPO_ROOT / ".tmp" / "acceptance-process-split"
        results = module._run_acceptance_phases(
            program,
            cwd,
            "/books/pdf-acceptance",
            True,
            90,
            "nonce-cross-process",
        )

        self.assertEqual(
            [(call[3], call[4]) for call in calls],
            [
                ("uncached", "cancel"),
                ("uncached", "resume"),
                ("cached", "cached"),
            ],
        )
        self.assertTrue(all(call[0] == program for call in calls))
        self.assertTrue(all(call[1] == cwd for call in calls))
        self.assertTrue(
            all(call[5] == "nonce-cross-process" for call in calls)
        )
        self.assertEqual([result["phase"] for result in results], [
            "cancel",
            "resume",
            "cached",
        ])

    def test_phases_launch_three_bound_program_processes_in_one_fs(self) -> None:
        module = self._load_runner()
        calls: list[tuple[list[str], Path, str, str, str]] = []

        def fake_subprocess_run(command, *, cwd, env, **kwargs):
            phase = env["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PHASE"]
            pass_name = env["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PASS"]
            nonce = env["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_NONCE"]
            calls.append((command, cwd, pass_name, phase, nonce))
            result = {
                "pass": pass_name,
                "phase": phase,
                "runner_nonce": nonce,
            }
            stdout = (
                f'{module.RESET_MARKER}{json.dumps({"phase": phase, "runner_nonce": nonce})}\n'
                f"{module.RESULT_MARKER}{json.dumps(result)}\n"
            )
            return subprocess.CompletedProcess(command, 0, stdout=stdout)

        program = REPO_ROOT / ".tools" / "isolated-simulator" / "program"
        filesystem_root = REPO_ROOT / ".tmp" / "one-pdf-acceptance-fs"
        original_run = module.subprocess.run
        try:
            module.subprocess.run = fake_subprocess_run
            with contextlib.redirect_stdout(io.StringIO()):
                module._run_acceptance_phases(
                    program,
                    filesystem_root,
                    "/books/pdf-acceptance",
                    True,
                    90,
                    "0123456789abcdef0123456789abcdef",
                )
        finally:
            module.subprocess.run = original_run

        self.assertEqual(len(calls), 3)
        self.assertTrue(all(call[0] == [str(program)] for call in calls))
        self.assertTrue(all(call[1] == filesystem_root for call in calls))
        self.assertEqual(
            [(call[2], call[3]) for call in calls],
            [("uncached", "cancel"), ("uncached", "resume"), ("cached", "cached")],
        )
        self.assertEqual(len({call[4] for call in calls}), 1)

    def test_cli_and_simulator_hook_are_narrow_and_separate_from_epub(
        self,
    ) -> None:
        completed = subprocess.run(
            [sys.executable, str(RUNNER), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        for flag in (
            "--container",
            "--headless",
            "--no-build",
            "--oracle",
        ):
            self.assertIn(flag, completed.stdout)

        cmake_source = SIMULATOR_CMAKE.read_text(encoding="utf-8")
        validate_portable_cmake_source(cmake_source)
        mutated_cmake = cmake_source.replace(
            '"${CMAKE_CURRENT_SOURCE_DIR}/test_pdf_simulator_acceptance.py" -v',
            "-m unittest "
            "test.pdf_simulator_acceptance.test_pdf_simulator_acceptance -v",
            1,
        )
        self.assertNotEqual(mutated_cmake, cmake_source)
        with self.assertRaisesRegex(
            AssertionError, "source path|stdlib-colliding"
        ):
            validate_portable_cmake_source(mutated_cmake)

        documentation = FILE_FORMATS.read_text(encoding="utf-8")
        semantic_writer = SEMANTIC_WRITER_SOURCE.read_text(encoding="utf-8")
        validate_semantic_anchor_documentation(
            documentation, semantic_writer
        )
        mutated_documentation = documentation.replace(
            "eight lowercase", "eight uppercase", 1
        )
        self.assertNotEqual(mutated_documentation, documentation)
        with self.assertRaisesRegex(AssertionError, "lowercase semantic anchors"):
            validate_semantic_anchor_documentation(
                mutated_documentation, semantic_writer
            )
        mutated_semantic_writer = semantic_writer.replace(
            'HEX_DIGITS[] = "0123456789abcdef"',
            'HEX_DIGITS[] = "0123456789ABCDEF"',
            1,
        )
        self.assertNotEqual(mutated_semantic_writer, semantic_writer)
        with self.assertRaisesRegex(
            AssertionError, "encoder must use lowercase"
        ):
            validate_semantic_anchor_documentation(
                documentation, mutated_semantic_writer
            )

        smoke = SMOKE_SOURCE.read_text(encoding="utf-8")
        acceptance = ACCEPTANCE_SOURCE.read_text(encoding="utf-8")
        self.assertIn("runPdfSimulatorAcceptance", smoke)
        self.assertIn("runEpubReflowRegressionOracle", smoke)
        self.assertIn("CROSSINK_SIMULATOR_PDF_ACCEPTANCE", smoke)
        self.assertNotIn("runEpubReflowRegressionOracle", acceptance)
        self.assertNotIn("#include <Epub.h>", acceptance)
        self.assertIn("#include <Epub/Page.h>", acceptance)
        self.assertIn("#include <Epub/Section.h>", acceptance)

    def test_epub_regression_uses_the_exact_same_isolated_program(
        self,
    ) -> None:
        pdf_runner = RUNNER.read_text(encoding="utf-8")
        epub_runner = EPUB_RUNNER_SOURCE.read_text(encoding="utf-8")
        validate_epub_program_contract(pdf_runner, epub_runner)

        dropped_program = pdf_runner.replace(
            '"--program",\n        str(program),',
            '"--passes",\n        "2",',
            1,
        )
        self.assertNotEqual(dropped_program, pdf_runner)
        with self.assertRaisesRegex(AssertionError, "exact isolated program"):
            validate_epub_program_contract(dropped_program, epub_runner)

        ignored_program = epub_runner.replace(
            "program = Path(args.program).resolve()",
            "program = PROGRAM",
            1,
        )
        self.assertNotEqual(ignored_program, epub_runner)
        with self.assertRaisesRegex(AssertionError, "explicit --program"):
            validate_epub_program_contract(pdf_runner, ignored_program)

        skipped_epub = pdf_runner.replace(
            (
                "        _run_epub_regression(\n"
                "            program, arguments.headless, arguments.timeout\n"
                "        )"
            ),
            "        pass  # skipped EPUB regression",
            1,
        )
        self.assertNotEqual(skipped_epub, pdf_runner)
        with self.assertRaisesRegex(AssertionError, "invoke the EPUB oracle"):
            validate_epub_program_contract(skipped_epub, epub_runner)

    @staticmethod
    def _load_runner():
        spec = importlib.util.spec_from_file_location(
            "run_pdf_simulator_acceptance", RUNNER
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load PDF simulator runner")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


if __name__ == "__main__":
    unittest.main()
