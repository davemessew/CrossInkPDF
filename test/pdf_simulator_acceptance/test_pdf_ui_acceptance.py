import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "run_pdf_simulator_acceptance.py"
SMOKE_SOURCE = REPO_ROOT / "src" / "simulator" / "SimulatorSmokeTest.cpp"
UI_SOURCE = REPO_ROOT / "src" / "simulator" / "PdfUiSimulatorAcceptance.cpp"
PREPARE_SOURCE = (
    REPO_ROOT / "src" / "activities" / "reader" / "PdfPrepareActivity.cpp"
)
PREPARE_HEADER = (
    REPO_ROOT / "src" / "activities" / "reader" / "PdfPrepareActivity.h"
)
PREPARE_OBSERVER_HEADER = (
    REPO_ROOT
    / "src"
    / "activities"
    / "reader"
    / "PdfPrepareAcceptanceObserver.h"
)
READER_SOURCE = REPO_ROOT / "src" / "activities" / "reader" / "ReaderActivity.cpp"
REFLOW_HEADER = REPO_ROOT / "lib" / "PdfReflow" / "PdfReflowDocument.h"
MANIFEST_HEADER = REPO_ROOT / "lib" / "PdfReflow" / "PdfCacheManifest.h"
CI_SOURCE = REPO_ROOT / ".github" / "workflows" / "ci.yml"
PDF_REFLOW_ROOT = REPO_ROOT / "lib" / "PdfReflow"
PDF_PREPARATION_INTEGRATION_SOURCES = (
    PREPARE_SOURCE,
    PREPARE_HEADER,
    REPO_ROOT / "lib" / "Epub" / "Epub" / "Section.cpp",
    REPO_ROOT / "lib" / "Epub" / "Epub" / "Section.h",
)


def _matching_cpp_brace(source: str, opening_brace: int) -> int:
    if opening_brace < 0 or opening_brace >= len(source) or source[opening_brace] != "{":
        raise AssertionError("C++ block opening brace is missing")

    depth = 0
    state = "code"
    escaped = False
    index = opening_brace
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and following == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError("C++ block closing brace is missing")


def _cpp_block_after(source: str, marker: str, start: int = 0) -> tuple[str, int, int]:
    marker_index = source.find(marker, start)
    if marker_index < 0:
        raise AssertionError(f"C++ source marker is missing: {marker}")
    opening_brace = source.find("{", marker_index + len(marker))
    closing_brace = _matching_cpp_brace(source, opening_brace)
    return source[opening_brace + 1 : closing_brace], opening_brace, closing_brace


def _normalized_cpp(source: str) -> str:
    return re.sub(r"\s+", " ", source).strip()


def _cpp_code_without_comments_and_literals(source: str) -> str:
    output = list(source)
    state = "code"
    escaped = False
    index = 0
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
            else:
                output[index] = " "
        elif state == "block_comment":
            output[index] = " " if char != "\n" else "\n"
            if char == "*" and following == "/":
                output[index + 1] = " "
                state = "code"
                index += 1
        elif state in ("string", "character"):
            output[index] = " " if char != "\n" else "\n"
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            output[index] = output[index + 1] = " "
            state = "line_comment"
            index += 1
        elif char == "/" and following == "*":
            output[index] = output[index + 1] = " "
            state = "block_comment"
            index += 1
        elif char == '"':
            output[index] = " "
            state = "string"
        elif char == "'":
            output[index] = " "
            state = "character"
        index += 1
    return "".join(output)


def _require_positive_macro_guard(
    source: str, token: str, macro: str, label: str
) -> None:
    token_lines = {
        source.count("\n", 0, match.start())
        for match in re.finditer(re.escape(token), source)
    }
    if not token_lines:
        raise AssertionError(f"{label} token is missing")

    stack: list[bool] = []
    for line_number, line in enumerate(source.splitlines()):
        if line_number in token_lines and not any(stack):
            raise AssertionError(
                f"{label} is not inside an exact positive {macro} guard"
            )

        directive = re.match(
            r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$", line
        )
        if directive is None:
            continue
        kind, expression = directive.groups()
        expression = expression.strip()
        if kind == "ifdef":
            stack.append(expression == macro)
        elif kind == "if":
            compact = re.sub(r"\s+", "", expression)
            stack.append(compact == f"defined({macro})")
        elif kind == "ifndef":
            stack.append(False)
        elif kind in ("else", "elif"):
            if not stack:
                raise AssertionError(f"{label} has an unmatched #{kind}")
            stack[-1] = False
        elif kind == "endif":
            if not stack:
                raise AssertionError(f"{label} has an unmatched #endif")
            stack.pop()
    if stack:
        raise AssertionError(f"{label} has an unterminated preprocessor guard")


def _validate_read_only_framebuffer_observation(ui_source: str) -> None:
    code = _cpp_code_without_comments_and_literals(ui_source)
    getter = re.compile(r"\brenderer\s*\.\s*getFrameBuffer\s*\(\s*\)")
    declaration = re.compile(
        r"(?:(?:const\s+uint8_t)|(?:uint8_t\s+const))\s*\*\s*const\s+"
        r"([A-Za-z_]\w*)\s*=\s*renderer\s*\.\s*getFrameBuffer\s*\(\s*\)\s*;"
    )
    getter_calls = list(getter.finditer(code))
    declarations = list(declaration.finditer(code))
    if not getter_calls or len(declarations) != len(getter_calls):
        raise AssertionError(
            "framebuffer observation must bind every renderer pointer as const data and a const pointer"
        )

    forbidden_storage_operations = (
        "lendBuildStorage",
        "borrowSecondaryBuffer",
        "releaseBuffers",
        "reallocBuffers",
        "memcpy",
        "memmove",
        "std::copy",
        "std::copy_n",
        "std::ranges::copy",
    )
    if any(operation in code for operation in forbidden_storage_operations):
        raise AssertionError(
            "framebuffer observation must not borrow, reallocate, or copy framebuffer storage"
        )

    remaining = list(code)
    for match in declarations:
        remaining[match.start() : match.end()] = " " * (match.end() - match.start())
    remaining_code = "".join(remaining)
    for name in {match.group(1) for match in declarations}:
        indexed = rf"\b{re.escape(name)}\s*\[[^\[\]]+\]"
        write = rf"(?:{indexed}|\*\s*\b{re.escape(name)}\b)"
        if re.search(
            rf"{write}\s*(?:<<=|>>=|[+\-*/%&|^]=|=(?!=)|\+\+|--)",
            remaining_code,
        ) or re.search(rf"(?:\+\+|--)\s*{write}", remaining_code):
            raise AssertionError("framebuffer observation must not write pixels")
        if re.search(rf"&\s*{indexed}", remaining_code):
            raise AssertionError("framebuffer observation must not alias pixel storage")

        allowed_removed = re.sub(
            rf"\b{re.escape(name)}\s*(?:==|!=)\s*nullptr|"
            rf"nullptr\s*(?:==|!=)\s*\b{re.escape(name)}\b",
            " ",
            remaining_code,
        )
        allowed_removed = re.sub(indexed, " ", allowed_removed)
        if re.search(rf"\b{re.escape(name)}\b", allowed_removed):
            raise AssertionError(
                "framebuffer pointer must not escape, alias, or be passed to copy/borrow code"
            )


def _pdf_preparation_production_sources() -> dict[Path, str]:
    paths = sorted(PDF_REFLOW_ROOT.rglob("*.cpp"))
    paths.extend(sorted(PDF_REFLOW_ROOT.rglob("*.h")))
    paths.extend(PDF_PREPARATION_INTEGRATION_SOURCES)
    if not paths or any(not path.is_file() for path in paths):
        raise AssertionError("PDF preparation production source closure is incomplete")
    return {path: path.read_text(encoding="utf-8") for path in paths}


def _validate_pdf_preparation_framebuffer_isolation(
    sources: dict[Path, str],
) -> None:
    forbidden_calls = re.compile(
        r"\b(?:getFrameBuffer|storeBwBuffer|restoreBwBuffer|"
        r"lendBuildStorage|borrowSecondaryBuffer|releaseBuffers|"
        r"reallocBuffers|copyRegionToBuffer|copyBufferToRegion)\s*\("
    )
    fixed_frame_bytes = re.compile(
        r"\b48000(?:[uUlL]*)\b|"
        r"\b800\s*\*\s*480\s*/\s*8\b|"
        r"\b480\s*\*\s*800\s*/\s*8\b"
    )
    framebuffer_alias = re.compile(r"\bframe_?buffer\b", re.IGNORECASE)

    violations: list[str] = []
    for path, source in sources.items():
        code = _cpp_code_without_comments_and_literals(source)
        if forbidden_calls.search(code):
            violations.append(f"{path}: framebuffer storage API")
        if fixed_frame_bytes.search(code):
            violations.append(f"{path}: fixed 48 KiB framebuffer alias")
        if framebuffer_alias.search(code):
            violations.append(f"{path}: framebuffer pointer alias")
    if violations:
        raise AssertionError(
            "PDF preparation must not borrow or alias the framebuffer: "
            + "; ".join(violations)
        )


def validate_ui_source_contract(
    smoke_source: str, ui_source: str, prepare_source: str
) -> None:
    activity_header = ui_source.find('#include "activities/Activity.h"')
    manager_header = ui_source.find('#include "activities/ActivityManager.h"')
    if activity_header < 0 or manager_header < 0 or activity_header > manager_header:
        raise AssertionError(
            "UI harness must complete Activity before ActivityManager templates"
        )
    for required in (
        "pdfUiSimulatorAcceptanceEnabled",
        "runPdfUiSimulatorAcceptanceTick",
    ):
        if required not in smoke_source:
            raise AssertionError(f"simulator smoke hook is missing {required}")

    for required in (
        "CROSSINK_SIMULATOR_PDF_UI_ACCEPTANCE",
        "CROSSINK_SIMULATOR_PDF_UI_EARLY_ERROR_BOOK",
        "CROSSINK_SIMULATOR_PDF_UI_WARNING_BACK_BOOK",
        "CROSSINK_SIMULATOR_PDF_UI_WARNING_BOOK",
        "CROSSINK_SIMULATOR_PDF_UI_ENCRYPTED_ERROR_BOOK",
        "activityManager.goHome",
        "activityManager.goToFileBrowser",
        "activityManager.getScreenshotInfo",
        "activityManager.requestUpdateAndWait",
        "hashPreparationDetail",
        "detail_hash",
        "simulatorInjectPress",
        "simulatorInjectRelease",
        "ScreenshotInfo::ReaderType::Pdf",
        "BOOKMARKS.getBookmarks",
        "CLIPPINGS.getClippings",
        "SIM_PDF_UI_EVENT",
        "SIM_PDF_UI_RESULT",
        'emitFrame("early_error_visible"',
        'emitFrame("warning_back_visible"',
        'emitSimple("warning_back_home"',
        'emitFrame("warning_visible"',
        'emitPositionFrame("warning_reader_open"',
        'emitPositionFrame("warning_cached_reopen"',
        "PdfPrepareAcceptanceObservation observation{};",
        "pdfObserveActivePrepareFailure(&observation)",
        "emitEncryptedErrorFrame(encryptedErrorFrameHash_, observation.error, "
        "observation.translationKey)",
    ):
        if required not in ui_source:
            raise AssertionError(f"real UI harness is missing {required}")
    normalized_ui_source = re.sub(r"\s+", " ", ui_source)
    for label, required in (
        (
            "encrypted PdfError receipt binding",
            'error == PdfError::Encrypted ? "PdfError::Encrypted" : "Unexpected"',
        ),
        (
            "encrypted translation receipt binding",
            "translationKey == StrId::STR_PDF_ENCRYPTED ? "
            '"STR_PDF_ENCRYPTED" : "Unexpected"',
        ),
        ("encrypted PdfError receipt field", '\\"pdf_error\\":\\"%s\\"'),
        (
            "encrypted translation receipt field",
            '\\"translation_key\\":\\"%s\\"',
        ),
    ):
        if required not in normalized_ui_source:
            raise AssertionError(f"real UI harness lacks {label}")

    if "runPdfSimulatorAcceptance" in ui_source:
        raise AssertionError("UI acceptance must not delegate to the direct oracle")
    if "Entering activity:" in ui_source:
        raise AssertionError("UI acceptance must not synthesize activity logs")
    _require_positive_macro_guard(
        ui_source,
        "CROSSINK_SIMULATOR_PDF_UI_ACCEPTANCE",
        "SIMULATOR",
        "PDF UI acceptance harness",
    )
    _validate_read_only_framebuffer_observation(ui_source)

    encrypted_step, _, _ = _cpp_block_after(
        ui_source, "case Step::WaitEncryptedError:"
    )
    if (
        "PdfError::Encrypted" in encrypted_step
        or "StrId::STR_PDF_ENCRYPTED" in encrypted_step
    ):
        raise AssertionError(
            "encrypted UI receipt must not use expected constants at the harness call site"
        )

    error_start = prepare_source.find("PdfPrepareActivity::errorMessage")
    error_end = prepare_source.find("PdfPrepareActivity::onEnter", error_start)
    error_render = prepare_source.find("errorMessage(failure_.error)")
    if error_start < 0 or error_end < 0 or error_render < error_end:
        raise AssertionError("PdfPrepare translated error render path is missing")
    error_body = prepare_source[error_start:error_end]
    translated_errors = (
        "STR_PDF_NO_READABLE_TEXT",
        "STR_PDF_ENCRYPTED",
        "STR_PDF_UNSUPPORTED_FILTER",
        "STR_PDF_UNSUPPORTED_ENCODING",
        "STR_PDF_INSUFFICIENT_MEMORY",
        "STR_PDF_INSUFFICIENT_STORAGE",
        "STR_PDF_PREPARATION_PAUSED",
        "STR_PDF_DAMAGED_OR_UNSAFE",
        "STR_PDF_UNSUPPORTED",
        "STR_PDF_PREPARATION_FAILED",
    )
    if any(f"return tr({key});" not in error_body for key in translated_errors):
        raise AssertionError("PdfPrepare error messages must all use translations")


def validate_prepare_observer_contract(
    prepare_header: str, prepare_source: str, observer_header: str
) -> None:
    for required in (
        "struct PdfPrepareAcceptanceObservation",
        "PdfPrepareAcceptanceObservation pdfPrepareAcceptanceObservationFor(",
        "bool pdfObserveActivePrepareFailure(",
    ):
        if required not in observer_header:
            raise AssertionError(f"PdfPrepare observer API is missing {required}")
    for required in (
        "bool acceptanceObserveFailure(",
        "PdfPrepareActivity* activePdfPrepareActivity",
        "activePdfPrepareActivity = this;",
        "activePdfPrepareActivity = nullptr;",
        "pdfPrepareAcceptanceObservationFor(failure_.error)",
        "activePdfPrepareActivity->acceptanceObserveFailure(observation)",
        "pdfPrepareAcceptanceObservationFor(",
    ):
        if required not in prepare_header + "\n" + prepare_source:
            raise AssertionError(
                f"PdfPrepare observer is not bound to the live activity: {required}"
            )
    if "#if defined(SIMULATOR) || defined(CROSSINK_QEMU)" not in observer_header:
        raise AssertionError("PdfPrepare acceptance observer is not test-build guarded")
    if "return {error, pdfPrepareErrorTranslationKey(error)};" not in observer_header:
        raise AssertionError(
            "PdfPrepare observer does not preserve the actual failure observation"
        )
    if (
        "case PdfError::Encrypted:" not in observer_header
        or "return StrId::STR_PDF_ENCRYPTED;" not in observer_header
    ):
        raise AssertionError(
            "encrypted PDF errors must use their dedicated translation"
        )


def validate_ci_contract(ci_source: str) -> None:
    if "pdf-simulator-acceptance:" not in ci_source:
        raise AssertionError("CI has no native PDF simulator acceptance job")
    if (
        "python scripts/run_pdf_simulator_acceptance.py --container"
        not in ci_source
    ):
        raise AssertionError("CI does not execute the actual PDF acceptance runner")
    status_start = ci_source.find("test-status:")
    if status_start < 0:
        raise AssertionError("CI test-status job is missing")
    if "- pdf-simulator-acceptance" not in ci_source[status_start:]:
        raise AssertionError("test-status does not require native PDF acceptance")


def validate_runner_wiring(runner_source: str) -> None:
    run_start = runner_source.find("def run(arguments: argparse.Namespace) -> int:")
    args_start = runner_source.find("def parse_args()", run_start)
    if run_start < 0 or args_start < 0:
        raise AssertionError("PDF runner entry point is missing")
    run_body = runner_source[run_start:args_start]
    for required in (
        "UI_VALID_FIXTURE",
        "UI_EARLY_ERROR_FIXTURE",
        "UI_WARNING_BACK_FIXTURE",
        "UI_WARNING_FIXTURE",
        "UI_ENCRYPTED_ERROR_FIXTURE",
        "UI_ERROR_FIXTURE",
        "stage_raw_fixture(",
        "_run_ui_acceptance(",
    ):
        if required not in run_body:
            raise AssertionError(f"PDF runner does not execute UI lane: {required}")
    if run_body.find("_run_ui_acceptance(") < run_body.find("validate_acceptance("):
        raise AssertionError("UI lane must follow the locked direct oracle")


def validate_pdf_ui_failure_warning_contract(
    reader_source: str,
    prepare_header: str,
    prepare_source: str,
    reflow_header: str,
    manifest_header: str,
) -> None:
    route_start = reader_source.find("bool ReaderActivity::openPdfRoute()")
    route_end = reader_source.find("bool ReaderActivity::openEpubRoute()", route_start)
    if route_start < 0 or route_end < 0:
        raise AssertionError("PDF reader route is missing")
    route = reader_source[route_start:route_end]
    failure_start = route.find("status.error == PdfError::InsufficientMemory")
    failure_end = route.find("auto preparation", failure_start)
    if failure_start < 0 or failure_end < 0:
        raise AssertionError("early PDF cache failure branch is missing")
    early_failure = route[failure_start:failure_end]
    error_activity = early_failure.find("makeUniqueNoThrow<PdfPrepareActivity>")
    silent_back = early_failure.find("onGoBack()")
    if silent_back >= 0 and (error_activity < 0 or silent_back < error_activity):
        raise AssertionError("early PDF cache failure still silently goes back")
    for required in (
        "makeUniqueNoThrow<PdfPrepareActivity>",
        "initialBookPath",
        "status",
        "activityManager.replaceActivity",
    ):
        if required not in early_failure:
            raise AssertionError(
                f"early PDF cache failure does not show translated error UI: {required}"
            )

    if "CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK" not in route:
        raise AssertionError("real-UI early cache failure injection is missing")
    _require_positive_macro_guard(
        reader_source,
        "CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK",
        "SIMULATOR",
        "PDF cache failure injection",
    )
    injection = route[route.find("CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK") : failure_start]
    if "PdfError::IoFailure" not in injection:
        raise AssertionError("cache failure injection is not simulator-only")

    for required in (
        "PdfStatus initialFailure",
        "Warning,",
        "pendingDocument_",
    ):
        if required not in prepare_header:
            raise AssertionError(f"PDF prepare UI state is missing {required}")
    on_enter, _, _ = _cpp_block_after(
        prepare_source, "void PdfPrepareActivity::onEnter()"
    )
    initial_failure, _, failure_end = _cpp_block_after(
        on_enter, "if (!initialFailure_.ok())"
    )
    if _normalized_cpp(initial_failure) != (
        "setFailure(initialFailure_); return;"
    ):
        raise AssertionError(
            "initial PDF error must return immediately without allocating or starting preparation"
        )
    preparation_start = on_enter.find("beginPreparation();")
    preparation_scope = on_enter
    if preparation_start >= 0:
        if preparation_start < failure_end:
            raise AssertionError(
                "initial PDF error screen still allocates or starts preparation"
            )
        preparation_scope, _, _ = _cpp_block_after(
            prepare_source, "void PdfPrepareActivity::beginPreparation()"
        )
    allocation = preparation_scope.find("makeUniqueNoThrow<PdfPreparation>")
    start = preparation_scope.find("preparation_->begin(")
    if allocation < 0 or start < allocation:
        raise AssertionError(
            "initial PDF error screen still allocates or starts preparation"
        )

    on_exit, _, _ = _cpp_block_after(
        prepare_source, "void PdfPrepareActivity::onExit()"
    )
    if "pendingDocument_.reset();" not in on_exit:
        raise AssertionError(
            "PdfPrepare onExit must release pending PDF document ownership"
        )

    finish_start = prepare_source.find("void PdfPrepareActivity::finishPreparation()")
    finish_end = prepare_source.find("void PdfPrepareActivity::loop()", finish_start)
    finish = prepare_source[finish_start:finish_end]
    for required in (
        "warningFlags()",
        "pendingDocument_ = std::move",
        "State::Warning",
    ):
        if required not in finish:
            raise AssertionError(f"fresh PDF warning handoff is missing {required}")

    loop, _, _ = _cpp_block_after(prepare_source, "void PdfPrepareActivity::loop()")
    warning_branch, _, _ = _cpp_block_after(loop, "if (state_ == State::Warning)")
    normalized_warning = _normalized_cpp(warning_branch)
    if (
        "if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) { "
        "openPreparedDocument(std::move(pendingDocument_)); }"
        not in normalized_warning
    ):
        raise AssertionError(
            "warning Confirm must transfer the pending PDF document to the reader"
        )
    if (
        "else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { "
        "finish(); }"
        not in normalized_warning
    ):
        raise AssertionError("warning Back must exit PdfPrepare")
    if "optionalContentWasSkipped()" in route:
        raise AssertionError("cached PDF reopen must not show the preparation warning")
    for required in (
        "STR_PDF_OPTIONAL_CONTENT_SKIPPED",
        "STR_CONTINUE",
    ):
        if required not in prepare_source:
            raise AssertionError(f"translated PDF warning UI is missing {required}")
    if "optionalContentWasSkipped() const" not in reflow_header:
        raise AssertionError("loaded PDF does not expose persisted warning state")
    if "PDF_CACHE_WARNING_OPTIONAL_CONTENT_OMITTED" not in manifest_header:
        raise AssertionError("persisted optional-content warning bit is not exported")


class PdfUiAcceptanceTest(unittest.TestCase):
    NONCE = "0123456789abcdef0123456789abcdef"

    def test_parser_accepts_real_activity_and_input_effect_evidence(self) -> None:
        module = self._load_runner()
        parsed = module.parse_ui_acceptance(self._valid_output(module), self.NONCE)

        self.assertTrue(parsed["result"]["completed"])
        self.assertEqual(
            [event["event"] for event in parsed["events"]],
            list(module.UI_EVENT_SEQUENCE),
        )
        self.assertIn("EpubReaderChapterSelection", parsed["activities"])
        self.assertIn("ClipSelection", parsed["activities"])

    def test_parser_requires_distinct_encrypted_pdf_error_route(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        encrypted = next(
            item
            for item in lines
            if item.get("event") == "encrypted_error_visible"
        )
        scan_only = next(
            item for item in lines if item.get("event") == "error_visible"
        )
        encrypted["frame_hash"] = scan_only["frame_hash"]
        with self.assertRaisesRegex(ValueError, "encrypted"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        for field, value in (
            ("pdf_error", "PdfError::Malformed"),
            ("translation_key", "STR_PDF_DAMAGED_OR_UNSAFE"),
        ):
            with self.subTest(field=field):
                lines = self._valid_lines(module)
                encrypted = next(
                    item
                    for item in lines
                    if item.get("event") == "encrypted_error_visible"
                )
                encrypted[field] = value
                with self.assertRaisesRegex(ValueError, "encrypted PDF receipt"):
                    module.parse_ui_acceptance(
                        self._serialize_lines(module, lines), self.NONCE
                    )

    def test_parser_rejects_missing_or_shadow_activity_evidence(self) -> None:
        module = self._load_runner()
        output = self._valid_output(module).replace(
            "[DBG] [ACT] Entering activity: EpubReaderChapterSelection\n",
            "",
            1,
        )
        with self.assertRaisesRegex(ValueError, "chapter selection"):
            module.parse_ui_acceptance(output, self.NONCE)

    def test_parser_rejects_cached_reopen_that_prepares_again(self) -> None:
        module = self._load_runner()
        cached_event = next(
            event
            for event in self._valid_lines(module)
            if event.get("event") == "cached_reopen"
        )
        cached_line = module.UI_EVENT_MARKER + json.dumps(cached_event)
        output = self._valid_output(module).replace(
            '[DBG] [ACT] Entering activity: EpubReader\n' + cached_line,
            '[DBG] [ACT] Entering activity: PdfPrepare\n'
            '[DBG] [ACT] Entering activity: EpubReader\n' + cached_line,
            1,
        )
        with self.assertRaisesRegex(ValueError, "cached reopen"):
            module.parse_ui_acceptance(output, self.NONCE)

    def test_parser_rejects_missing_visible_early_cache_error(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        lines = [
            item
            for item in lines
            if item.get("event") != "early_error_visible"
        ]
        with self.assertRaisesRegex(ValueError, "event order"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        lines = self._valid_lines(module)
        selected_index = next(
            index
            for index, item in enumerate(lines)
            if item.get("event") == "early_error_file_selected"
        )
        self.assertEqual(lines[selected_index + 1].get("activity"), "PdfPrepare")
        del lines[selected_index + 1]
        with self.assertRaisesRegex(ValueError, "early cache error route"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

    def test_parser_rejects_missing_warning_or_rewarning_cached_pdf(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        warning = next(
            event for event in lines if event.get("event") == "warning_visible"
        )
        warning["frame_hash"] = next(
            event
            for event in lines
            if event.get("event") == "warning_reader_open"
        )["frame_hash"]
        with self.assertRaisesRegex(ValueError, "warning frame"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        cached_event = next(
            event
            for event in self._valid_lines(module)
            if event.get("event") == "warning_cached_reopen"
        )
        cached_line = module.UI_EVENT_MARKER + json.dumps(cached_event)
        output = self._valid_output(module).replace(
            "[DBG] [ACT] Entering activity: EpubReader\n" + cached_line,
            "[DBG] [ACT] Entering activity: PdfPrepare\n"
            "[DBG] [ACT] Entering activity: EpubReader\n"
            + cached_line,
            1,
        )
        with self.assertRaisesRegex(ValueError, "warning cached reopen"):
            module.parse_ui_acceptance(output, self.NONCE)

    def test_parser_requires_real_warning_back_exit_before_confirm(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        module.parse_ui_acceptance(self._serialize_lines(module, lines), self.NONCE)

        without_exit = [
            item for item in lines if item.get("exit_activity") != "PdfPrepare"
        ]
        with self.assertRaisesRegex(ValueError, "warning Back.*exit"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, without_exit), self.NONCE
            )

    def test_parser_rejects_resume_without_detail_text_change(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        preparing = next(
            event for event in lines if event.get("event") == "prepare_visible"
        )
        resumed = next(
            event for event in lines if event.get("event") == "prepare_resumed"
        )
        resumed["detail_hash"] = preparing["detail_hash"]

        with self.assertRaisesRegex(ValueError, "resume detail"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

    def test_parser_rejects_nonresumable_or_discontinuous_checkpoint(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        cancelled = next(
            event for event in lines if event.get("event") == "cancelled_checkpoint"
        )
        cancelled["resume_phase"] = "none"
        with self.assertRaisesRegex(ValueError, "durable resumable checkpoint"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        lines = self._valid_lines(module)
        resumed = next(
            event for event in lines if event.get("event") == "prepare_resumed"
        )
        resumed["generation"] += 1
        with self.assertRaisesRegex(ValueError, "resume checkpoint continuity"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

    def test_parser_rejects_unchanged_page_turn_and_saved_progress(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        open_event = next(
            event for event in lines if event.get("event") == "reader_open"
        )
        turn_event = next(
            event for event in lines if event.get("event") == "page_turned"
        )
        for key in ("spine", "page", "pages", "progress", "frame_hash"):
            turn_event[key] = open_event[key]
        with self.assertRaisesRegex(ValueError, "page turn"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        lines = self._valid_lines(module)
        saved = next(
            event for event in lines if event.get("event") == "progress_saved"
        )
        reopened = next(
            event for event in lines if event.get("event") == "cached_reopen"
        )
        reopened["page"] = saved["page"] + 1
        with self.assertRaisesRegex(ValueError, "saved progress"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

    def test_parser_rejects_missing_bookmark_clipping_and_error_pixels(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        bookmark = next(
            event for event in lines if event.get("event") == "bookmark_added"
        )
        bookmark["after"] = bookmark["before"]
        with self.assertRaisesRegex(ValueError, "bookmark"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        lines = self._valid_lines(module)
        clipping = next(
            event for event in lines if event.get("event") == "clipping_added"
        )
        clipping["after"] = clipping["before"]
        with self.assertRaisesRegex(ValueError, "clipping"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        lines = self._valid_lines(module)
        preparing = next(
            event for event in lines if event.get("event") == "prepare_visible"
        )
        error = next(
            event for event in lines if event.get("event") == "error_visible"
        )
        error["frame_hash"] = preparing["frame_hash"]
        with self.assertRaisesRegex(ValueError, "error frame"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

    def test_parser_rejects_marker_nonce_order_and_result_spoofing(self) -> None:
        module = self._load_runner()
        lines = self._valid_lines(module)
        event = next(item for item in lines if item.get("event") == "reader_open")
        event["runner_nonce"] = "wrong"
        with self.assertRaisesRegex(ValueError, "nonce"):
            module.parse_ui_acceptance(
                self._serialize_lines(module, lines), self.NONCE
            )

        output = self._valid_output(module).replace(
            '"event": "page_turned"', '"event": "wrong_order"', 1
        )
        with self.assertRaisesRegex(ValueError, "event order"):
            module.parse_ui_acceptance(output, self.NONCE)

        output = self._valid_output(module).replace(
            module.UI_RESULT_MARKER,
            module.UI_RESULT_MARKER
            + json.dumps(
                {
                    "schema_version": 1,
                    "runner_nonce": self.NONCE,
                    "event_count": len(module.UI_EVENT_SEQUENCE),
                    "completed": True,
                }
            )
            + "\n"
            + module.UI_RESULT_MARKER,
            1,
        )
        with self.assertRaisesRegex(ValueError, "exactly one"):
            module.parse_ui_acceptance(output, self.NONCE)

    def test_runner_launches_bound_ui_mode_and_parses_its_output(self) -> None:
        module = self._load_runner()
        program = REPO_ROOT / ".pio" / "build" / "simulator" / "program"
        filesystem = REPO_ROOT / ".tmp" / "pdf-ui-contract"
        valid_book = "/books/pdf-ui/navigation_outline.pdf"
        early_error_book = "/books/pdf-ui-early-error/classic_text.pdf"
        warning_back_book = (
            "/books/pdf-ui-warning-back/unsupported_jpx_caption.pdf"
        )
        warning_book = "/books/pdf-ui-warning/unsupported_jpx_caption.pdf"
        encrypted_error_book = "/books/pdf-ui-encrypted-error/encrypted.pdf"
        error_book = "/books/pdf-ui-error/scan_only.pdf"
        completed = subprocess.CompletedProcess(
            [str(program)], 0, stdout=self._valid_output(module)
        )

        with mock.patch.object(module.subprocess, "run", return_value=completed) as run:
            result = module._run_ui_acceptance(
                program,
                filesystem,
                valid_book,
                early_error_book,
                warning_back_book,
                warning_book,
                encrypted_error_book,
                error_book,
                self.NONCE,
                True,
                90,
            )

        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["CROSSINK_SIMULATOR_PDF_UI_BOOK"], valid_book)
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_ERROR_BOOK"], error_book
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_EARLY_ERROR_BOOK"],
            early_error_book,
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK"],
            early_error_book,
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_WARNING_BACK_BOOK"],
            warning_back_book,
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_WARNING_BOOK"], warning_book
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_ENCRYPTED_ERROR_BOOK"],
            encrypted_error_book,
        )
        self.assertEqual(
            environment["CROSSINK_SIMULATOR_PDF_UI_NONCE"], self.NONCE
        )
        self.assertEqual(result["result"]["event_count"], len(module.UI_EVENT_SEQUENCE))

    def test_container_command_maps_repo_oracle_into_workspace(self) -> None:
        module = self._load_runner()
        arguments = SimpleNamespace(
            image="crossink-pdf-simulator:test",
            headless=True,
            build=False,
            update_oracle=False,
            oracle=module.DEFAULT_ORACLE,
            timeout=240,
        )
        command = module._container_command(arguments)
        oracle = command[command.index("--oracle") + 1]
        self.assertEqual(
            oracle,
            "test/pdf_simulator_acceptance/pdf_simulator_acceptance.oracle.json",
        )
        self.assertNotIn("\\", oracle)
        self.assertNotIn(":", oracle)

    def test_source_contract_uses_real_ui_and_read_only_frame_observation(self) -> None:
        validate_ui_source_contract(
            SMOKE_SOURCE.read_text(encoding="utf-8"),
            UI_SOURCE.read_text(encoding="utf-8"),
            PREPARE_SOURCE.read_text(encoding="utf-8"),
        )
        validate_prepare_observer_contract(
            PREPARE_HEADER.read_text(encoding="utf-8"),
            PREPARE_SOURCE.read_text(encoding="utf-8"),
            PREPARE_OBSERVER_HEADER.read_text(encoding="utf-8"),
        )

    def test_source_contract_rejects_unbound_encrypted_error_receipts(self) -> None:
        smoke_source = SMOKE_SOURCE.read_text(encoding="utf-8")
        ui_source = UI_SOURCE.read_text(encoding="utf-8")
        prepare_source = PREPARE_SOURCE.read_text(encoding="utf-8")
        mutations = {
            "error": ui_source.replace(
                "error == PdfError::Encrypted",
                "true",
                1,
            ),
            "translation": ui_source.replace(
                "translationKey == StrId::STR_PDF_ENCRYPTED",
                "true",
                1,
            ),
        }
        for label, mutation in mutations.items():
            with self.subTest(label=label):
                self.assertNotEqual(mutation, ui_source)
                with self.assertRaisesRegex(AssertionError, "receipt binding"):
                    validate_ui_source_contract(
                        smoke_source, mutation, prepare_source
                    )

        observer_header = PREPARE_OBSERVER_HEADER.read_text(encoding="utf-8")
        observer_mutations = {
            "actual error": observer_header.replace(
                "return {error, pdfPrepareErrorTranslationKey(error)};",
                "return {PdfError::Encrypted, pdfPrepareErrorTranslationKey(error)};",
                1,
            ),
            "actual translation": observer_header.replace(
                "return {error, pdfPrepareErrorTranslationKey(error)};",
                "return {error, StrId::STR_PDF_ENCRYPTED};",
                1,
            ),
        }
        for label, mutation in observer_mutations.items():
            with self.subTest(observer=label):
                self.assertNotEqual(mutation, observer_header)
                with self.assertRaisesRegex(
                    AssertionError, "actual failure observation"
                ):
                    validate_prepare_observer_contract(
                        PREPARE_HEADER.read_text(encoding="utf-8"),
                        PREPARE_SOURCE.read_text(encoding="utf-8"),
                        mutation,
                    )

        mutated_prepare = PREPARE_SOURCE.read_text(encoding="utf-8").replace(
            "return tr(STR_PDF_NO_READABLE_TEXT);",
            'return "No readable text";',
            1,
        )
        with self.assertRaisesRegex(AssertionError, "translations"):
            validate_ui_source_contract(
                SMOKE_SOURCE.read_text(encoding="utf-8"),
                UI_SOURCE.read_text(encoding="utf-8"),
                mutated_prepare,
            )

        generic_encrypted = PREPARE_OBSERVER_HEADER.read_text(encoding="utf-8").replace(
            "return StrId::STR_PDF_ENCRYPTED;",
            "return StrId::STR_PDF_PREPARATION_FAILED;",
            1,
        )
        with self.assertRaisesRegex(AssertionError, "encrypted PDF"):
            validate_prepare_observer_contract(
                PREPARE_HEADER.read_text(encoding="utf-8"),
                PREPARE_SOURCE.read_text(encoding="utf-8"),
                generic_encrypted,
            )

        with self.assertRaisesRegex(AssertionError, "synthesize activity"):
            validate_ui_source_contract(
                SMOKE_SOURCE.read_text(encoding="utf-8"),
                UI_SOURCE.read_text(encoding="utf-8")
                + '\nstd::printf("Entering activity: EpubReader\\n");\n',
                PREPARE_SOURCE.read_text(encoding="utf-8"),
            )

    def test_pdf_preparation_production_closure_never_borrows_framebuffer(
        self,
    ) -> None:
        sources = _pdf_preparation_production_sources()
        _validate_pdf_preparation_framebuffer_isolation(sources)

        mutation_path = PREPARE_SOURCE
        storage_borrow = dict(sources)
        storage_borrow[mutation_path] += "\nvoid mutation() { renderer.storeBwBuffer(); }\n"
        with self.assertRaisesRegex(AssertionError, "framebuffer"):
            _validate_pdf_preparation_framebuffer_isolation(storage_borrow)

        fixed_alias = dict(sources)
        fixed_alias[mutation_path] += "\nuint8_t borrowedFrame[48000];\n"
        with self.assertRaisesRegex(AssertionError, "48 KiB"):
            _validate_pdf_preparation_framebuffer_isolation(fixed_alias)

    def test_source_contract_rejects_simulator_guard_bypasses(self) -> None:
        smoke_source = SMOKE_SOURCE.read_text(encoding="utf-8")
        ui_source = UI_SOURCE.read_text(encoding="utf-8")
        prepare_source = PREPARE_SOURCE.read_text(encoding="utf-8")
        reader_source = READER_SOURCE.read_text(encoding="utf-8")

        unguarded_ui = ui_source.replace("#ifdef SIMULATOR", "#if 1", 1)
        self.assertNotEqual(unguarded_ui, ui_source)
        with self.assertRaisesRegex(AssertionError, "SIMULATOR"):
            validate_ui_source_contract(
                smoke_source, unguarded_ui, prepare_source
            )

        injection_guard = (
            "#ifdef SIMULATOR\n"
            "  const char* const injectedCacheErrorBook"
        )
        unguarded_reader = reader_source.replace(
            injection_guard,
            "#if 1\n  const char* const injectedCacheErrorBook",
            1,
        )
        self.assertNotEqual(unguarded_reader, reader_source)
        with self.assertRaisesRegex(AssertionError, "SIMULATOR guard"):
            validate_pdf_ui_failure_warning_contract(
                unguarded_reader,
                PREPARE_HEADER.read_text(encoding="utf-8"),
                prepare_source,
                REFLOW_HEADER.read_text(encoding="utf-8"),
                MANIFEST_HEADER.read_text(encoding="utf-8"),
            )

    def test_source_contract_rejects_framebuffer_alias_writes_and_copies(self) -> None:
        smoke_source = SMOKE_SOURCE.read_text(encoding="utf-8")
        ui_source = UI_SOURCE.read_text(encoding="utf-8")
        prepare_source = PREPARE_SOURCE.read_text(encoding="utf-8")
        declaration = (
            "    const uint8_t* const frameBuffer = renderer.getFrameBuffer();\n"
        )
        mutations = (
            ui_source.replace(
                declaration,
                "    auto* frameBuffer = renderer.getFrameBuffer();\n",
                1,
            ),
            ui_source.replace(
                declaration,
                declaration + "    frameBuffer[0] = 0;\n",
                1,
            ),
            ui_source.replace(
                declaration,
                declaration
                + "    const uint8_t* const framebufferAlias = frameBuffer;\n",
                1,
            ),
            ui_source.replace(
                declaration,
                declaration
                + "    std::copy_n(frameBuffer, frameBufferSize, frameBuffer);\n",
                1,
            ),
        )

        for mutated in mutations:
            with self.subTest(mutation=mutated[len(ui_source) :]):
                self.assertNotEqual(mutated, ui_source)
                with self.assertRaisesRegex(AssertionError, "framebuffer"):
                    validate_ui_source_contract(
                        smoke_source, mutated, prepare_source
                    )

    def test_ci_executes_and_requires_the_real_native_acceptance(self) -> None:
        source = CI_SOURCE.read_text(encoding="utf-8")
        validate_ci_contract(source)
        mutated = source.replace("- pdf-simulator-acceptance", "", 1)
        with self.assertRaisesRegex(AssertionError, "does not require"):
            validate_ci_contract(mutated)

        runner_source = RUNNER.read_text(encoding="utf-8")
        validate_runner_wiring(runner_source)
        removed_ui_call = runner_source.replace(
            "            _run_ui_acceptance(\n",
            "            _disabled_ui_acceptance(\n",
            1,
        )
        self.assertNotEqual(removed_ui_call, runner_source)
        with self.assertRaisesRegex(AssertionError, "does not execute UI lane"):
            validate_runner_wiring(removed_ui_call)

    def test_pdf_early_failures_and_optional_warning_have_visible_ui(self) -> None:
        validate_pdf_ui_failure_warning_contract(
            READER_SOURCE.read_text(encoding="utf-8"),
            PREPARE_HEADER.read_text(encoding="utf-8"),
            PREPARE_SOURCE.read_text(encoding="utf-8"),
            REFLOW_HEADER.read_text(encoding="utf-8"),
            MANIFEST_HEADER.read_text(encoding="utf-8"),
        )

    def test_source_contract_rejects_initial_failure_fallthrough(self) -> None:
        prepare_source = PREPARE_SOURCE.read_text(encoding="utf-8")
        mutated = prepare_source.replace(
            "    setFailure(initialFailure_);\n    return;\n",
            "    setFailure(initialFailure_);\n",
            1,
        )
        self.assertNotEqual(mutated, prepare_source)

        with self.assertRaisesRegex(AssertionError, "initial PDF error"):
            validate_pdf_ui_failure_warning_contract(
                READER_SOURCE.read_text(encoding="utf-8"),
                PREPARE_HEADER.read_text(encoding="utf-8"),
                mutated,
                REFLOW_HEADER.read_text(encoding="utf-8"),
                MANIFEST_HEADER.read_text(encoding="utf-8"),
            )

    def test_source_contract_rejects_warning_ownership_breaks(self) -> None:
        prepare_source = PREPARE_SOURCE.read_text(encoding="utf-8")
        mutations = (
            (
                "release pending PDF document",
                prepare_source.replace("  pendingDocument_.reset();\n", "", 1),
            ),
            (
                "warning Back",
                prepare_source.replace(
                    "    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {\n"
                    "      finish();\n",
                    "    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {\n"
                    "      return;\n",
                    1,
                ),
            ),
            (
                "warning Confirm",
                prepare_source.replace(
                    "      openPreparedDocument(std::move(pendingDocument_));\n",
                    "      openPreparedDocument(nullptr);\n",
                    1,
                ),
            ),
        )

        for expected_failure, mutated in mutations:
            with self.subTest(expected_failure=expected_failure):
                self.assertNotEqual(mutated, prepare_source)
                with self.assertRaisesRegex(AssertionError, expected_failure):
                    validate_pdf_ui_failure_warning_contract(
                        READER_SOURCE.read_text(encoding="utf-8"),
                        PREPARE_HEADER.read_text(encoding="utf-8"),
                        mutated,
                        REFLOW_HEADER.read_text(encoding="utf-8"),
                        MANIFEST_HEADER.read_text(encoding="utf-8"),
                    )

    def _valid_output(self, module) -> str:
        return self._serialize_lines(module, self._valid_lines(module))

    def _valid_lines(self, module) -> list[dict]:
        def event(name: str, **evidence) -> dict:
            return {
                "schema_version": 1,
                "runner_nonce": self.NONCE,
                "event": name,
                **evidence,
            }

        position = {"spine": 1, "page": 1, "pages": 2, "progress": 55}
        return [
            {"reset": True, "schema_version": 1, "runner_nonce": self.NONCE},
            {"activity": "Home"},
            event("home", frame_hash="0000000000000010"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("uncached_file_selected"),
            {"activity": "PdfPrepare"},
            event(
                "prepare_visible",
                frame_hash="0000000000000020",
                detail_hash="00000000000000A0",
            ),
            event(
                "cancelled_checkpoint",
                frame_hash="0000000000000030",
                checkpoint_exists=True,
                resume_phase="after_page",
                last_verified_page=1,
                generation=77,
            ),
            {"activity": "Home"},
            event("home_after_cancel"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("resume_file_selected"),
            {"activity": "PdfPrepare"},
            event(
                "prepare_resumed",
                frame_hash="0000000000000040",
                detail_hash="00000000000000B0",
                checkpoint_exists=True,
                resume_phase="after_page",
                last_verified_page=1,
                generation=77,
            ),
            {"activity": "EpubReader"},
            event(
                "reader_open",
                frame_hash="0000000000000050",
                spine=0,
                page=1,
                pages=3,
                progress=10,
            ),
            event(
                "page_turned",
                frame_hash="0000000000000060",
                spine=0,
                page=2,
                pages=3,
                progress=20,
            ),
            {"activity": "EpubReaderMenu"},
            {"activity": "EpubReaderChapterSelection"},
            event(
                "contents_navigated",
                frame_hash="0000000000000070",
                **position,
            ),
            {"activity": "EpubReaderMenu"},
            event("bookmark_added", before=0, after=1),
            {"activity": "EpubReaderMenu"},
            {"activity": "ClipSelection"},
            event("clipping_added", before=0, after=1),
            {"activity": "Home"},
            event("progress_saved", **position),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("cached_file_selected"),
            {"activity": "EpubReader"},
            event(
                "cached_reopen",
                frame_hash="0000000000000080",
                **position,
            ),
            {"activity": "Home"},
            event("home_before_early_error"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("early_error_file_selected"),
            {"activity": "PdfPrepare"},
            event("early_error_visible", frame_hash="0000000000000090"),
            {"activity": "Home"},
            event("home_before_warning"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("warning_back_file_selected"),
            {"activity": "PdfPrepare"},
            event("warning_back_visible", frame_hash="0000000000000098"),
            {"exit_activity": "PdfPrepare"},
            {"activity": "Home"},
            event("warning_back_home"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("warning_file_selected"),
            {"activity": "PdfPrepare"},
            event("warning_visible", frame_hash="00000000000000A0"),
            {"activity": "EpubReader"},
            event(
                "warning_reader_open",
                frame_hash="00000000000000B0",
                spine=0,
                page=1,
                pages=2,
                progress=5,
            ),
            {"activity": "Home"},
            event("home_before_warning_cached"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("warning_cached_file_selected"),
            {"activity": "EpubReader"},
            event(
                "warning_cached_reopen",
                frame_hash="00000000000000C0",
                spine=0,
                page=1,
                pages=2,
                progress=5,
            ),
            {"activity": "Home"},
            event("home_before_encrypted_error"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("encrypted_error_file_selected"),
            {"activity": "PdfPrepare"},
            event(
                "encrypted_error_visible",
                frame_hash="00000000000000C8",
                pdf_error="PdfError::Encrypted",
                translation_key="STR_PDF_ENCRYPTED",
            ),
            {"activity": "Home"},
            event("home_before_error"),
            {"activity": "FileBrowser"},
            {"activity": "Reader"},
            event("error_file_selected"),
            {"activity": "PdfPrepare"},
            event("error_visible", frame_hash="00000000000000D0"),
            event("complete"),
            {
                "result": True,
                "schema_version": 1,
                "runner_nonce": self.NONCE,
                "event_count": len(module.UI_EVENT_SEQUENCE),
                "completed": True,
            },
            {"pass": True},
        ]

    @staticmethod
    def _serialize_lines(module, lines: list[dict]) -> str:
        output: list[str] = []
        for item in copy.deepcopy(lines):
            if item.pop("reset", False):
                output.append(module.UI_RESET_MARKER + json.dumps(item))
            elif "activity" in item:
                output.append(
                    "[DBG] [ACT] Entering activity: " + item["activity"]
                )
            elif "exit_activity" in item:
                output.append(
                    "[DBG] [ACT] Exiting activity: " + item["exit_activity"]
                )
            elif item.pop("result", False):
                output.append(module.UI_RESULT_MARKER + json.dumps(item))
            elif item.pop("pass", False):
                output.append(module.UI_PASS_MARKER)
            else:
                output.append(module.UI_EVENT_MARKER + json.dumps(item))
        return "\n".join(output) + "\n"

    @staticmethod
    def _load_runner():
        spec = importlib.util.spec_from_file_location(
            "run_pdf_simulator_acceptance_ui", RUNNER
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load PDF simulator runner")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


if __name__ == "__main__":
    unittest.main()
