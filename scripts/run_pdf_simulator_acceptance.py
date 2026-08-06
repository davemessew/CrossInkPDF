#!/usr/bin/env python3
"""Run the deterministic on-device PDF reflow acceptance in the native simulator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"
DEFAULT_ORACLE = (
    ROOT
    / "test"
    / "pdf_simulator_acceptance"
    / "pdf_simulator_acceptance.oracle.json"
)
EPUB_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
EPUB_ORACLE = (
    ROOT
    / "test"
    / "epubs"
    / "test_reader_rendering_matrix.oracle.json"
)
CONTAINER_RUNNER = ROOT / "scripts" / "run_pdf_simulator_container.py"
EPUB_RUNNER = ROOT / "scripts" / "run_simulator_smoke_test.py"
FIXTURE_SOURCE = ROOT / "test" / "pdf_reflow_core" / "fixtures"
FIXTURES = (
    "classic_text.pdf",
    "font_size_6.pdf",
    "font_size_72.pdf",
    "navigation_outline.pdf",
    "raster_cover_caption.pdf",
    "bad_startxref.pdf",
    "encrypted.pdf",
    "flate_bomb.pdf",
    "scan_only.pdf",
)
CANCEL_RESUME_FIXTURE = "raster_cover_caption.pdf"
FRESH_RESUME_BASELINE_SOURCE = CANCEL_RESUME_FIXTURE
FRESH_RESUME_BASELINE_NAME = "raster_cover_caption_fresh.pdf"
RESULT_MARKER = "SIM_PDF_ACCEPTANCE_RESULT "
RESET_MARKER = "SIM_PDF_ACCEPTANCE_RESET "
PASS_MARKER = "PDF_SIMULATOR_ACCEPTANCE_PASS"
UI_RESET_MARKER = "SIM_PDF_UI_RESET "
UI_EVENT_MARKER = "SIM_PDF_UI_EVENT "
UI_RESULT_MARKER = "SIM_PDF_UI_RESULT "
UI_PASS_MARKER = "PDF_SIMULATOR_UI_ACCEPTANCE_PASS"
UI_EVENT_SEQUENCE = (
    "home",
    "uncached_file_selected",
    "prepare_visible",
    "cancelled_checkpoint",
    "home_after_cancel",
    "resume_file_selected",
    "prepare_resumed",
    "reader_open",
    "page_turned",
    "contents_navigated",
    "bookmark_added",
    "clipping_added",
    "progress_saved",
    "cached_file_selected",
    "cached_reopen",
    "home_before_early_error",
    "early_error_file_selected",
    "early_error_visible",
    "home_before_warning",
    "warning_back_file_selected",
    "warning_back_visible",
    "warning_back_home",
    "warning_file_selected",
    "warning_visible",
    "warning_reader_open",
    "home_before_warning_cached",
    "warning_cached_file_selected",
    "warning_cached_reopen",
    "home_before_encrypted_error",
    "encrypted_error_file_selected",
    "encrypted_error_visible",
    "home_before_error",
    "error_file_selected",
    "error_visible",
    "complete",
)
UI_VALID_FIXTURE = "navigation_outline.pdf"
UI_EARLY_ERROR_FIXTURE = "classic_text.pdf"
UI_WARNING_BACK_FIXTURE = "unsupported_jpx_caption.pdf"
UI_WARNING_FIXTURE = "unsupported_jpx_caption.pdf"
UI_ENCRYPTED_ERROR_FIXTURE = "encrypted.pdf"
UI_ERROR_FIXTURE = "scan_only.pdf"
MAX_CANCELLATION_SLICES = 256
MAX_CANCELLATION_SLICE_MS = 8
MAX_CANCELLATION_SLICE_OPERATIONS = 32
CANCELLATION_CHECKPOINT = "after_image_repair"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(64 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def stage_raw_fixture(source: Path, destination: Path) -> str:
    """Copy a PDF byte-for-byte; preparation is intentionally simulator-side."""
    before = sha256_file(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    after = sha256_file(destination)
    if after != before:
        raise ValueError(f"raw fixture changed while staging: {source.name}")
    return before


def verify_staged_fixture(path: Path, expected_sha256: str) -> None:
    if sha256_file(path) != expected_sha256:
        raise ValueError(f"raw staged PDF changed: {path.name}")


def parse_result(
    output: str,
    expected_pass: str,
    expected_phase: str,
    expected_nonce: str,
) -> dict:
    reset_seen = False
    payloads: list[str] = []
    for line in output.splitlines():
        if line.startswith(RESULT_MARKER):
            if not reset_seen:
                raise ValueError("simulator result evidence appeared before reset")
            payloads.append(line.split(RESULT_MARKER, 1)[1])
            continue
        if not line.startswith(RESET_MARKER):
            continue
        if reset_seen:
            raise ValueError("expected exactly one simulator reset marker")
        try:
            reset = json.loads(line.split(RESET_MARKER, 1)[1])
        except json.JSONDecodeError as error:
            raise ValueError(f"invalid simulator reset JSON: {error}") from error
        if not isinstance(reset, dict):
            raise ValueError("simulator reset must be a JSON object")
        if reset.get("phase") != expected_phase:
            raise ValueError(
                f"simulator reset phase mismatch: expected {expected_phase}"
            )
        if reset.get("runner_nonce") != expected_nonce:
            raise ValueError("simulator reset nonce mismatch")
        reset_seen = True
    if not reset_seen:
        raise ValueError("expected exactly one simulator reset marker, found 0")
    if len(payloads) != 1:
        raise ValueError(
            f"expected exactly one {RESULT_MARKER.strip()} marker, "
            f"found {len(payloads)}"
        )
    try:
        result = json.loads(payloads[0])
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid simulator result JSON: {error}") from error
    if not isinstance(result, dict):
        raise ValueError("simulator result must be a JSON object")
    if result.get("pass") != expected_pass:
        raise ValueError(
            f"simulator result pass mismatch: expected {expected_pass}"
        )
    if result.get("phase") != expected_phase:
        raise ValueError(
            f"simulator result phase mismatch: expected {expected_phase}"
        )
    if result.get("runner_nonce") != expected_nonce:
        raise ValueError("simulator result nonce mismatch")
    return result


def _parse_marker_json(line: str, marker: str, label: str) -> dict:
    try:
        payload = json.loads(line.split(marker, 1)[1])
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid {label} JSON: {error}") from error
    if not isinstance(payload, dict):
        raise ValueError(f"{label} must be a JSON object")
    return payload


def _require_activity_subsequence(
    activities: list[tuple[int, str]],
    start_line: int,
    end_line: int,
    expected: tuple[str, ...],
    label: str,
) -> None:
    observed = [
        name for line_number, name in activities
        if start_line < line_number < end_line
    ]
    cursor = 0
    for name in observed:
        if cursor < len(expected) and name == expected[cursor]:
            cursor += 1
    if cursor != len(expected):
        raise ValueError(
            f"{label} did not traverse real activities: "
            f"expected {list(expected)}, observed {observed}"
        )


def _require_frame_hash(event: dict, label: str) -> str:
    value = event.get("frame_hash")
    if not isinstance(value, str) or re.fullmatch(r"[0-9A-F]{16}", value) is None:
        raise ValueError(f"{label} frame hash is invalid")
    if value == "0000000000000000":
        raise ValueError(f"{label} frame hash is empty")
    return value


def _require_detail_hash(event: dict, label: str) -> str:
    value = event.get("detail_hash")
    if not isinstance(value, str) or re.fullmatch(r"[0-9A-F]{16}", value) is None:
        raise ValueError(f"{label} detail hash is invalid")
    if value == "0000000000000000":
        raise ValueError(f"{label} detail hash is empty")
    return value


def _position(event: dict, label: str) -> tuple[int, int, int, int]:
    values = tuple(event.get(key) for key in ("spine", "page", "pages", "progress"))
    if any(not isinstance(value, int) or isinstance(value, bool) for value in values):
        raise ValueError(f"{label} position is invalid")
    spine, page, pages, progress = values
    if spine < 0 or pages <= 0 or page <= 0 or page > pages or progress < 0 or progress > 100:
        raise ValueError(f"{label} position is out of range")
    return spine, page, pages, progress


def _resume_checkpoint(event: dict, label: str) -> tuple[str, int, int]:
    resume_phase = event.get("resume_phase")
    last_verified_page = event.get("last_verified_page")
    generation = event.get("generation")
    if (
        resume_phase != "after_page"
        or not isinstance(last_verified_page, int)
        or isinstance(last_verified_page, bool)
        or last_verified_page <= 0
        or not isinstance(generation, int)
        or isinstance(generation, bool)
        or generation <= 0
    ):
        raise ValueError(f"{label} is not a durable resumable checkpoint")
    return resume_phase, last_verified_page, generation


def parse_ui_acceptance(output: str, expected_nonce: str) -> dict:
    reset_entries: list[tuple[int, dict]] = []
    result_entries: list[tuple[int, dict]] = []
    events: list[dict] = []
    event_lines: dict[str, int] = {}
    activities: list[tuple[int, str]] = []
    activity_exits: list[tuple[int, str]] = []
    pass_lines: list[int] = []

    for line_number, line in enumerate(output.splitlines()):
        if line.startswith(UI_RESET_MARKER):
            reset_entries.append(
                (
                    line_number,
                    _parse_marker_json(line, UI_RESET_MARKER, "PDF UI reset"),
                )
            )
            continue
        if line.startswith(UI_EVENT_MARKER):
            event = _parse_marker_json(line, UI_EVENT_MARKER, "PDF UI event")
            name = event.get("event")
            if isinstance(name, str):
                event_lines[name] = line_number
            events.append(event)
            continue
        if line.startswith(UI_RESULT_MARKER):
            result_entries.append(
                (
                    line_number,
                    _parse_marker_json(line, UI_RESULT_MARKER, "PDF UI result"),
                )
            )
            continue
        if line == UI_PASS_MARKER:
            pass_lines.append(line_number)
            continue
        activity_marker = "Entering activity: "
        marker_index = line.find(activity_marker)
        if marker_index >= 0:
            activities.append(
                (line_number, line[marker_index + len(activity_marker):].strip())
            )
            continue
        exit_marker = "Exiting activity: "
        marker_index = line.find(exit_marker)
        if marker_index >= 0:
            activity_exits.append(
                (line_number, line[marker_index + len(exit_marker):].strip())
            )

    if len(reset_entries) != 1:
        raise ValueError(
            f"expected exactly one {UI_RESET_MARKER.strip()} marker, "
            f"found {len(reset_entries)}"
        )
    if len(result_entries) != 1:
        raise ValueError(
            f"expected exactly one {UI_RESULT_MARKER.strip()} marker, "
            f"found {len(result_entries)}"
        )
    if len(pass_lines) != 1:
        raise ValueError(
            f"expected exactly one {UI_PASS_MARKER} marker, found {len(pass_lines)}"
        )

    reset_line, reset = reset_entries[0]
    result_line, result = result_entries[0]
    if reset_line >= result_line or result_line >= pass_lines[0]:
        raise ValueError("PDF UI reset/result/pass markers are out of order")
    for label, payload in (("reset", reset), ("result", result), *[("event", event) for event in events]):
        if payload.get("schema_version") != 1:
            raise ValueError(f"PDF UI {label} schema is not version 1")
        if payload.get("runner_nonce") != expected_nonce:
            raise ValueError(f"PDF UI {label} nonce mismatch")

    names = [event.get("event") for event in events]
    if names != list(UI_EVENT_SEQUENCE):
        raise ValueError(
            f"PDF UI event order mismatch: expected {list(UI_EVENT_SEQUENCE)}, got {names}"
        )
    if any(event_lines.get(name, -1) <= reset_line for name in UI_EVENT_SEQUENCE):
        raise ValueError("PDF UI event appeared before reset")
    if event_lines[UI_EVENT_SEQUENCE[-1]] >= result_line:
        raise ValueError("PDF UI result appeared before the complete event")
    if (
        result.get("completed") is not True
        or result.get("event_count") != len(UI_EVENT_SEQUENCE)
    ):
        raise ValueError("PDF UI result does not receipt every event")

    by_name = {event["event"]: event for event in events}
    encrypted_receipt = by_name["encrypted_error_visible"]
    if (
        encrypted_receipt.get("pdf_error") != "PdfError::Encrypted"
        or encrypted_receipt.get("translation_key") != "STR_PDF_ENCRYPTED"
    ):
        raise ValueError(
            "encrypted PDF receipt does not bind PdfError::Encrypted to "
            "STR_PDF_ENCRYPTED"
        )
    first_line = reset_line
    _require_activity_subsequence(
        activities, first_line, event_lines["home"], ("Home",), "home route"
    )
    _require_activity_subsequence(
        activities,
        event_lines["home"],
        event_lines["prepare_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "uncached FileBrowser/PDF route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["cancelled_checkpoint"],
        event_lines["home_after_cancel"],
        ("Home",),
        "cancel return",
    )
    _require_activity_subsequence(
        activities,
        event_lines["home_after_cancel"],
        event_lines["prepare_resumed"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "resumable reopen",
    )
    _require_activity_subsequence(
        activities,
        event_lines["prepare_resumed"],
        event_lines["reader_open"],
        ("EpubReader",),
        "real reflow reader transition",
    )
    _require_activity_subsequence(
        activities,
        event_lines["reader_open"],
        event_lines["contents_navigated"],
        ("EpubReaderMenu", "EpubReaderChapterSelection"),
        "chapter selection",
    )
    _require_activity_subsequence(
        activities,
        event_lines["contents_navigated"],
        event_lines["bookmark_added"],
        ("EpubReaderMenu",),
        "bookmark menu",
    )
    _require_activity_subsequence(
        activities,
        event_lines["bookmark_added"],
        event_lines["clipping_added"],
        ("EpubReaderMenu", "ClipSelection"),
        "clipping selection",
    )
    _require_activity_subsequence(
        activities,
        event_lines["clipping_added"],
        event_lines["progress_saved"],
        ("Home",),
        "progress save exit",
    )

    cached_activities = [
        name for line_number, name in activities
        if event_lines["progress_saved"] < line_number < event_lines["cached_reopen"]
    ]
    _require_activity_subsequence(
        activities,
        event_lines["progress_saved"],
        event_lines["cached_reopen"],
        ("FileBrowser", "Reader", "EpubReader"),
        "cached reopen",
    )
    if "PdfPrepare" in cached_activities:
        raise ValueError("cached reopen unexpectedly entered PdfPrepare")
    _require_activity_subsequence(
        activities,
        event_lines["cached_reopen"],
        event_lines["home_before_early_error"],
        ("Home",),
        "cached reader exit",
    )
    _require_activity_subsequence(
        activities,
        event_lines["home_before_early_error"],
        event_lines["early_error_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "early cache error route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["early_error_visible"],
        event_lines["home_before_warning"],
        ("Home",),
        "early cache error exit",
    )
    _require_activity_subsequence(
        activities,
        event_lines["home_before_warning"],
        event_lines["warning_back_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "optional-content warning Back route",
    )
    warning_back_exits = [
        name
        for line_number, name in activity_exits
        if event_lines["warning_back_visible"]
        < line_number
        < event_lines["warning_back_home"]
    ]
    if "PdfPrepare" not in warning_back_exits:
        raise ValueError("warning Back did not exit PdfPrepare")
    _require_activity_subsequence(
        activities,
        event_lines["warning_back_visible"],
        event_lines["warning_back_home"],
        ("Home",),
        "warning Back home route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["warning_back_home"],
        event_lines["warning_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "optional-content warning Confirm route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["warning_visible"],
        event_lines["warning_reader_open"],
        ("EpubReader",),
        "warning continue route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["warning_reader_open"],
        event_lines["home_before_warning_cached"],
        ("Home",),
        "warning reader exit",
    )
    warning_cached_activities = [
        name for line_number, name in activities
        if event_lines["home_before_warning_cached"]
        < line_number
        < event_lines["warning_cached_reopen"]
    ]
    _require_activity_subsequence(
        activities,
        event_lines["home_before_warning_cached"],
        event_lines["warning_cached_reopen"],
        ("FileBrowser", "Reader", "EpubReader"),
        "warning cached reopen",
    )
    if "PdfPrepare" in warning_cached_activities:
        raise ValueError("warning cached reopen unexpectedly entered PdfPrepare")
    _require_activity_subsequence(
        activities,
        event_lines["warning_cached_reopen"],
        event_lines["home_before_encrypted_error"],
        ("Home",),
        "warning cached reader exit",
    )
    _require_activity_subsequence(
        activities,
        event_lines["home_before_encrypted_error"],
        event_lines["encrypted_error_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "encrypted PDF translated error route",
    )
    _require_activity_subsequence(
        activities,
        event_lines["encrypted_error_visible"],
        event_lines["home_before_error"],
        ("Home",),
        "encrypted PDF error exit",
    )
    _require_activity_subsequence(
        activities,
        event_lines["home_before_error"],
        event_lines["error_visible"],
        ("FileBrowser", "Reader", "PdfPrepare"),
        "translated error route",
    )

    frame_events = (
        "home",
        "prepare_visible",
        "cancelled_checkpoint",
        "prepare_resumed",
        "reader_open",
        "page_turned",
        "contents_navigated",
        "cached_reopen",
        "early_error_visible",
        "warning_back_visible",
        "warning_visible",
        "warning_reader_open",
        "warning_cached_reopen",
        "encrypted_error_visible",
        "error_visible",
    )
    frame_hashes = {
        name: _require_frame_hash(by_name[name], name) for name in frame_events
    }
    if by_name["cancelled_checkpoint"].get("checkpoint_exists") is not True:
        raise ValueError("cancel did not leave a resumable checkpoint")
    if by_name["prepare_resumed"].get("checkpoint_exists") is not True:
        raise ValueError("resumed preparation did not observe its checkpoint")
    cancelled_resume = _resume_checkpoint(
        by_name["cancelled_checkpoint"], "cancelled checkpoint"
    )
    resumed_resume = _resume_checkpoint(
        by_name["prepare_resumed"], "resumed checkpoint"
    )
    if resumed_resume != cancelled_resume:
        raise ValueError("resume checkpoint continuity failed")
    if frame_hashes["prepare_visible"] == frame_hashes["prepare_resumed"]:
        raise ValueError("resumed preparation frame did not change")
    fresh_detail_hash = _require_detail_hash(
        by_name["prepare_visible"], "fresh preparation"
    )
    resumed_detail_hash = _require_detail_hash(
        by_name["prepare_resumed"], "resumed preparation"
    )
    if fresh_detail_hash == resumed_detail_hash:
        raise ValueError("resume detail text did not change")

    reader_position = _position(by_name["reader_open"], "reader open")
    turn_position = _position(by_name["page_turned"], "page turn")
    if (
        turn_position[:2] == reader_position[:2]
        or turn_position[3] < reader_position[3]
        or frame_hashes["page_turned"] == frame_hashes["reader_open"]
    ):
        raise ValueError("page turn produced no observed reader effect")
    contents_position = _position(by_name["contents_navigated"], "contents navigation")
    if contents_position[0] == turn_position[0]:
        raise ValueError("contents navigation did not change chapter")

    for name, label in (("bookmark_added", "bookmark"), ("clipping_added", "clipping")):
        before = by_name[name].get("before")
        after = by_name[name].get("after")
        if (
            not isinstance(before, int)
            or isinstance(before, bool)
            or not isinstance(after, int)
            or isinstance(after, bool)
            or after <= before
        ):
            raise ValueError(f"{label} input did not add a saved item")

    saved_position = _position(by_name["progress_saved"], "saved progress")
    cached_position = _position(by_name["cached_reopen"], "cached reopen")
    if cached_position != saved_position:
        raise ValueError("cached reopen did not restore saved progress")
    warning_position = _position(by_name["warning_reader_open"], "warning reader open")
    warning_cached_position = _position(
        by_name["warning_cached_reopen"], "warning cached reopen"
    )
    if warning_cached_position != warning_position:
        raise ValueError("warning cached reopen did not restore reader position")
    if frame_hashes["warning_visible"] == frame_hashes["warning_reader_open"]:
        raise ValueError("warning frame is not distinct from the reader")
    if frame_hashes["early_error_visible"] in {
        frame_hashes["warning_visible"],
        frame_hashes["warning_reader_open"],
        frame_hashes["warning_cached_reopen"],
    }:
        raise ValueError("early cache error frame is not a distinct rendered screen")
    if frame_hashes["encrypted_error_visible"] in {
        frame_hashes["prepare_visible"],
        frame_hashes["cancelled_checkpoint"],
        frame_hashes["prepare_resumed"],
        frame_hashes["reader_open"],
        frame_hashes["early_error_visible"],
        frame_hashes["warning_visible"],
        frame_hashes["error_visible"],
    }:
        raise ValueError(
            "encrypted PDF translated error frame is not distinct"
        )
    if frame_hashes["error_visible"] in {
        frame_hashes["prepare_visible"],
        frame_hashes["cancelled_checkpoint"],
        frame_hashes["prepare_resumed"],
        frame_hashes["reader_open"],
        frame_hashes["early_error_visible"],
        frame_hashes["warning_visible"],
        frame_hashes["encrypted_error_visible"],
    }:
        raise ValueError("translated error frame is not a distinct rendered screen")

    return {
        "reset": reset,
        "events": events,
        "activities": [name for _, name in activities],
        "activity_exits": [name for _, name in activity_exits],
        "result": result,
    }


def _first_difference(
    expected: object, actual: object, path: str = ""
) -> str:
    if type(expected) is not type(actual):
        return path or "<root>"
    if isinstance(expected, dict):
        if set(expected) != set(actual):
            key = sorted(set(expected).symmetric_difference(actual))[0]
            return ".".join(filter(None, (path, key)))
        for key in sorted(expected):
            difference = _first_difference(
                expected[key],
                actual[key],
                ".".join(filter(None, (path, key))),
            )
            if difference:
                return difference
        return ""
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return path or "<root>"
        for index, (expected_item, actual_item) in enumerate(
            zip(expected, actual, strict=True)
        ):
            difference = _first_difference(
                expected_item, actual_item, f"{path}[{index}]"
            )
            if difference:
                return difference
        return ""
    return "" if expected == actual else (path or "<root>")


def _require_counter(counters: dict, name: str) -> int:
    value = counters.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"invalid counter: {name}")
    return value


def validate_acceptance(
    cancelled: dict,
    uncached: dict,
    cached: dict,
    expected_oracle: dict,
) -> None:
    expected_results = (
        ("uncached", "cancel", cancelled),
        ("uncached", "resume", uncached),
        ("cached", "cached", cached),
    )
    runner_nonce = cancelled.get("runner_nonce")
    if not isinstance(runner_nonce, str) or not runner_nonce:
        raise ValueError("cancel result has no runner nonce")
    for pass_name, phase, result in expected_results:
        if result.get("schema_version") != 1:
            raise ValueError(f"{phase} result schema is not version 1")
        if result.get("pass") != pass_name:
            raise ValueError(f"{phase} result pass mismatch")
        if result.get("phase") != phase:
            raise ValueError(f"{phase} result phase mismatch")
        if result.get("runner_nonce") != runner_nonce:
            raise ValueError(f"{phase} result runner nonce mismatch")
    for pass_name, result in (("uncached", uncached), ("cached", cached)):
        if not isinstance(result.get("oracle"), dict):
            raise ValueError(f"{pass_name} result has no oracle")

    cancel_continuity = cancelled.get("continuity")
    resume_continuity = uncached.get("continuity")
    if not isinstance(cancel_continuity, dict) or not isinstance(
        resume_continuity, dict
    ):
        raise ValueError("cancel/resume continuity evidence is missing")
    if (
        cancel_continuity.get("checkpoint_name") != CANCELLATION_CHECKPOINT
        or resume_continuity.get("checkpoint_name") != CANCELLATION_CHECKPOINT
    ):
        raise ValueError("cancel/resume checkpoint name is not deterministic")
    if (
        cancel_continuity.get("source_identity")
        != resume_continuity.get("source_identity")
    ):
        raise ValueError("cancel/resume source identity continuity failed")
    if cancel_continuity.get("generation") != resume_continuity.get(
        "generation"
    ):
        raise ValueError("cancel/resume generation continuity failed")
    checkpoint = cancel_continuity.get("checkpoint")
    resumed_checkpoint = resume_continuity.get("resumed_checkpoint")
    if (
        not isinstance(checkpoint, dict)
        or checkpoint != resumed_checkpoint
        or checkpoint.get("name") != CANCELLATION_CHECKPOINT
        or checkpoint.get("resume_phase") != CANCELLATION_CHECKPOINT
        or not isinstance(checkpoint.get("sequence"), int)
        or checkpoint["sequence"] <= 0
    ):
        raise ValueError("cancel/resume checkpoint continuity failed")
    checkpoint_cursor_fields = (
        "last_verified_page",
        "last_verified_object",
        "emitted_sections",
        "emitted_images",
        "cumulative_words",
        "output_bytes",
    )
    if any(
        not isinstance(checkpoint.get(name), int)
        or isinstance(checkpoint.get(name), bool)
        or checkpoint[name] <= 0
        for name in checkpoint_cursor_fields
    ) or (
        not isinstance(checkpoint.get("warning_flags"), int)
        or isinstance(checkpoint.get("warning_flags"), bool)
        or checkpoint["warning_flags"] < 0
    ):
        raise ValueError("cancel/resume checkpoint cursor evidence is incomplete")
    cancel_snapshot = cancel_continuity.get("counter_snapshot")
    resume_snapshot = resume_continuity.get("counter_snapshot")
    if (
        not isinstance(cancel_snapshot, dict)
        or cancel_snapshot != resume_snapshot
    ):
        raise ValueError("cancel/resume counter snapshot continuity failed")
    checkpoint_work_fields = (
        "preparation_steps",
        "parser_steps",
        "page_steps",
        "image_steps",
        "xref_steps",
        "pages_walked",
        "content_tokens",
        "sections_emitted",
        "images_emitted",
        "source_bytes_read",
    )
    if any(
        not isinstance(cancel_snapshot.get(name), int)
        or isinstance(cancel_snapshot.get(name), bool)
        or cancel_snapshot[name] <= 0
        for name in checkpoint_work_fields
    ):
        raise ValueError("cancel/resume checkpoint work snapshot is incomplete")
    if resume_continuity.get("resumed_from_checkpoint") is not True:
        raise ValueError("resume process did not use the persisted checkpoint")

    difference = _first_difference(uncached["oracle"], cached["oracle"])
    if difference:
        raise ValueError(
            f"uncached/cached oracle mismatch at {difference}"
        )
    difference = _first_difference(expected_oracle, uncached["oracle"])
    if difference:
        raise ValueError(f"locked PDF oracle mismatch at {difference}")

    oracle = uncached["oracle"]
    typography = oracle.get("typography", {})
    pdf_6 = typography.get("pdf_6")
    pdf_72 = typography.get("pdf_72")
    positive = typography.get("device_font_positive")
    if not isinstance(pdf_6, dict) or pdf_6 != pdf_72:
        raise ValueError(
            "6 pt and 72 pt PDFs did not use identical device typography"
        )
    if not isinstance(positive, dict) or positive == pdf_6:
        raise ValueError("device font positive control did not change layout")
    if (
        positive.get("text_hash") != pdf_6.get("text_hash")
        or (
            positive.get("page_count") == pdf_6.get("page_count")
            and positive.get("first_frame") == pdf_6.get("first_frame")
            and positive.get("middle_frame") == pdf_6.get("middle_frame")
            and positive.get("last_frame") == pdf_6.get("last_frame")
        )
    ):
        raise ValueError("device font positive control is not credible")
    if typography.get("fixed_page_canvas") is not False:
        raise ValueError("acceptance rendered a fixed PDF page canvas")

    navigation = oracle.get("navigation", {})
    if (
        navigation.get("sections", 0) < 2
        or navigation.get("toc_entries", 0) < 3
        or navigation.get("internal_links", 0) < 2
        or navigation.get("resolved_internal_links")
        != navigation.get("internal_links")
        or navigation.get("publisher_labels", 0) < 2
    ):
        raise ValueError(
            "PDF navigation oracle has an unresolved href/fragment"
        )

    image = oracle.get("image", {})
    image_rect = image.get("rect")
    if image.get("page_image_found") is not True or (
        not isinstance(image_rect, list)
        or len(image_rect) != 4
        or any(
            not isinstance(value, int) or isinstance(value, bool)
            for value in image_rect
        )
        or image_rect[2] <= 0
        or image_rect[3] <= 0
    ):
        raise ValueError("retained PDF image has no inspected PageImage rectangle")
    if (
        not isinstance(image.get("non_white_pixels"), int)
        or image["non_white_pixels"] <= 0
        or not isinstance(image.get("region_hash"), str)
        or not image["region_hash"]
    ):
        raise ValueError("retained PDF image has no deterministic non-white pixels")
    if (
        image.get("retained", 0) < 1
        or image.get("frame_hash") == image.get("blank_hash")
    ):
        raise ValueError("retained PDF image did not affect the framebuffer")

    progress = oracle.get("progress", {})
    total_words = progress.get("total_words")
    nonterminal_saved_cursor = progress.get("nonterminal_saved_cursor")
    saved_cursor = progress.get("saved_cursor")
    if (
        not isinstance(total_words, int)
        or total_words <= 0
        or nonterminal_saved_cursor != total_words * 3 // 5
        or progress.get("nonterminal_resumed_cursor")
        != nonterminal_saved_cursor
        or progress.get("nonterminal_percent_millionths")
        != nonterminal_saved_cursor * 1_000_000 // total_words
        or nonterminal_saved_cursor >= total_words
        or not isinstance(saved_cursor, int)
        or saved_cursor != total_words
        or progress.get("resumed_cursor") != saved_cursor
        or progress.get("percent_millionths") != 1_000_000
    ):
        raise ValueError("word progress/resume oracle is invalid")

    route = oracle.get("route")
    if not isinstance(route, dict) or any(
        route.get(name) is not True
        for name in (
            "raw_pdf",
            "on_device_preparation",
            "cancelled",
            "resumed",
            "cached_reopen",
        )
    ):
        raise ValueError("on-device PDF route witness is incomplete")

    layout_controls = oracle.get("layout_controls", {})
    portrait = layout_controls.get("portrait_frame")
    if (
        not isinstance(portrait, str)
        or layout_controls.get("landscape_frame") == portrait
    ):
        raise ValueError("orientation positive control did not change layout")
    if layout_controls.get("wide_margin_frame") == portrait:
        raise ValueError("margin positive control did not change layout")

    for pass_name, result in (("uncached", uncached), ("cached", cached)):
        negative = result.get("negative", {})
        checked = negative.get("checked")
        if (
            not isinstance(checked, int)
            or checked < 4
            or negative.get("rejected") != checked
        ):
            raise ValueError(
                f"{pass_name} negative PDF corpus did not fail closed"
            )
        counters = result.get("counters")
        if not isinstance(counters, dict):
            raise ValueError(f"{pass_name} result has no counters")
        for name in (
            "preparation_steps",
            "extraction_runs",
            "parser_calls",
            "source_open_calls",
            "source_read_calls",
            "source_read_bytes",
            "source_max_read",
            "cached_page_turns",
            "page_turn_source_opens",
            "page_turn_source_reads",
            "io_calls",
            "max_io_request",
            "yielded_slices",
            "cancelled_runs",
            "resumed_runs",
            "cancellation_steps",
            "cancellation_elapsed_ms",
            "cancellation_max_slice_ms",
            "cancellation_max_slice_io_calls",
            "fresh_baseline_steps",
            "fresh_baseline_parser_steps",
            "fresh_baseline_page_steps",
            "fresh_baseline_image_steps",
            "resumed_preparation_steps",
            "resumed_parser_steps",
            "resumed_page_steps",
            "resumed_image_steps",
            "max_actual_step_us",
            "framebuffer_guard_checks",
            "framebuffer_guard_failures",
            "framebuffer_guard_controls",
            "framebuffer_guard_rejections",
        ):
            _require_counter(counters, name)
        if counters["max_io_request"] > 4096:
            raise ValueError(f"{pass_name} I/O request exceeded 4 KiB")

    cached_counters = cached["counters"]
    if cached_counters["preparation_steps"] != 0:
        raise ValueError("cached pass invoked PDF preparation")
    if cached_counters["extraction_runs"] != 0:
        raise ValueError("cached pass invoked PDF extraction")
    if cached_counters["parser_calls"] != 0:
        raise ValueError("cached pass invoked the PDF parser")
    if cached_counters["source_open_calls"] > 1:
        raise ValueError("cached pass used more than one identity open")
    if cached_counters["source_read_calls"] > 2:
        raise ValueError("cached pass used more than two identity reads")
    if cached_counters["source_max_read"] > 4096:
        raise ValueError("cached pass identity reads exceeded 4 KiB")
    if cached_counters["cached_page_turns"] != 100:
        raise ValueError("cached pass did not replay 100 page turns")
    if (
        cached_counters["page_turn_source_opens"] != 0
        or cached_counters["page_turn_source_reads"] != 0
    ):
        raise ValueError("cached page turns reopened the source PDF")
    uncached_counters = uncached["counters"]
    if uncached_counters["resumed_runs"] != 1:
        raise ValueError("on-device cancel/resume witness is incomplete")
    for name in (
        "cancelled_runs",
        "cancellation_steps",
        "cancellation_elapsed_ms",
        "cancellation_max_slice_ms",
        "cancellation_max_slice_io_calls",
    ):
        if uncached_counters[name] != 0:
            raise ValueError(
                "resume process unexpectedly reported cancellation work"
            )

    fresh_work = tuple(
        uncached_counters[name]
        for name in (
            "fresh_baseline_steps",
            "fresh_baseline_parser_steps",
            "fresh_baseline_page_steps",
            "fresh_baseline_image_steps",
        )
    )
    resumed_work = tuple(
        uncached_counters[name]
        for name in (
            "resumed_preparation_steps",
            "resumed_parser_steps",
            "resumed_page_steps",
            "resumed_image_steps",
        )
    )
    if fresh_work[0] == 0 or resumed_work[0] == 0:
        raise ValueError("fresh/resumed preparation work witness is missing")
    if any(resumed > fresh for resumed, fresh in zip(resumed_work, fresh_work)):
        raise ValueError("resumed preparation exceeded fresh preparation work")
    if resumed_work[0] >= fresh_work[0] or any(
        fresh == 0 or resumed >= fresh
        for resumed, fresh in zip(resumed_work[1:], fresh_work[1:])
    ):
        raise ValueError(
            "cancelled PDF did not reduce every completed work category"
        )
    if uncached_counters["max_actual_step_us"] > 8000:
        raise ValueError("preparation step exceeded 8 ms actual wall time")
    if (
        uncached_counters["framebuffer_guard_checks"] <= 0
        or uncached_counters["framebuffer_guard_failures"] != 0
    ):
        raise ValueError("uncached PDF preparation changed the framebuffer")
    if (
        uncached_counters["framebuffer_guard_controls"]
        != uncached_counters["framebuffer_guard_checks"] * 2
        or uncached_counters["framebuffer_guard_rejections"]
        != uncached_counters["framebuffer_guard_controls"]
    ):
        raise ValueError("uncached framebuffer positive controls failed")
    if (
        cached_counters["framebuffer_guard_checks"] != 0
        or cached_counters["framebuffer_guard_failures"] != 0
        or cached_counters["framebuffer_guard_controls"] != 0
        or cached_counters["framebuffer_guard_rejections"] != 0
    ):
        raise ValueError("cached pass reported PDF framebuffer access")

    cancel_counters = cancelled.get("counters")
    if not isinstance(cancel_counters, dict):
        raise ValueError("cancel result has no counters")
    for name in (
        "cancellation_steps",
        "cancellation_elapsed_ms",
        "cancellation_max_slice_ms",
        "cancellation_max_slice_io_calls",
        "max_actual_step_us",
        "framebuffer_guard_checks",
        "framebuffer_guard_failures",
        "framebuffer_guard_controls",
        "framebuffer_guard_rejections",
    ):
        _require_counter(cancel_counters, name)
    if cancel_counters["cancellation_steps"] <= 1:
        raise ValueError("cooperative cancellation incorrectly completed in one step")
    if cancel_counters["cancellation_steps"] > MAX_CANCELLATION_SLICES:
        raise ValueError("cancellation slice cap was exhausted")
    if (
        cancel_counters["cancellation_elapsed_ms"]
        > MAX_CANCELLATION_SLICES * MAX_CANCELLATION_SLICE_MS
    ):
        raise ValueError("cancellation total elapsed-work cap was exceeded")
    if cancel_counters["cancellation_max_slice_ms"] > MAX_CANCELLATION_SLICE_MS:
        raise ValueError("cancellation slice exceeded 8 ms equivalent")
    if (
        cancel_counters["cancellation_max_slice_io_calls"]
        > MAX_CANCELLATION_SLICE_OPERATIONS
    ):
        raise ValueError("cancellation slice exceeded 32 operations")
    if cancel_counters["max_actual_step_us"] > 8000:
        raise ValueError("cancellation step exceeded 8 ms actual wall time")
    if (
        cancel_counters["framebuffer_guard_checks"] <= 0
        or cancel_counters["framebuffer_guard_failures"] != 0
    ):
        raise ValueError("cancelled PDF preparation changed the framebuffer")
    if (
        cancel_counters["framebuffer_guard_controls"]
        != cancel_counters["framebuffer_guard_checks"] * 2
        or cancel_counters["framebuffer_guard_rejections"]
        != cancel_counters["framebuffer_guard_controls"]
    ):
        raise ValueError("cancelled framebuffer positive controls failed")

    if (
        cached_counters["cancelled_runs"] != 0
        or cached_counters["resumed_runs"] != 0
    ):
        raise ValueError("cached pass unexpectedly ran cancel/resume")
    for name in (
        "cancellation_steps",
        "cancellation_elapsed_ms",
        "cancellation_max_slice_ms",
        "cancellation_max_slice_io_calls",
        "fresh_baseline_steps",
        "fresh_baseline_parser_steps",
        "fresh_baseline_page_steps",
        "fresh_baseline_image_steps",
        "resumed_preparation_steps",
        "resumed_parser_steps",
        "resumed_page_steps",
        "resumed_image_steps",
    ):
        if cached_counters[name] != 0:
            raise ValueError(
                "cached pass unexpectedly reported preparation work"
            )


def _load_oracle(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read PDF oracle {path}: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or not isinstance(document.get("oracle"), dict)
    ):
        raise ValueError("PDF oracle must contain schema_version 1 and oracle")
    return document["oracle"]


def oracle_generation_command(program: Path, oracle: Path) -> list[str]:
    return [
        sys.executable,
        str(Path(__file__).resolve()),
        "--program",
        str(program.resolve()),
        "--no-build",
        "--headless",
        "--oracle",
        str(oracle.resolve()),
        "--update-oracle",
    ]


def _run_program(
    program: Path,
    cwd: Path,
    fixture_root: str,
    pass_name: str,
    phase: str,
    runner_nonce: str,
    headless: bool,
    timeout: int,
) -> dict:
    environment = os.environ.copy()
    environment["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
    environment["CROSSINK_SIMULATOR_PDF_ACCEPTANCE"] = "1"
    environment["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PASS"] = pass_name
    environment["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PHASE"] = phase
    environment["CROSSINK_SIMULATOR_PDF_ACCEPTANCE_NONCE"] = runner_nonce
    environment["CROSSINK_SIMULATOR_PDF_FIXTURE_ROOT"] = fixture_root
    if headless:
        environment.setdefault("SDL_VIDEODRIVER", "dummy")
    process = subprocess.run(
        [str(program)],
        cwd=cwd,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    print(process.stdout, end="")
    if process.returncode != 0:
        raise ValueError(
            f"{pass_name} simulator exited with {process.returncode}"
        )
    for pattern in CRASH_PATTERNS:
        if pattern in process.stdout:
            raise ValueError(
                f"{pass_name} simulator output contained {pattern}"
            )
    return parse_result(process.stdout, pass_name, phase, runner_nonce)


def _run_ui_acceptance(
    program: Path,
    cwd: Path,
    valid_book: str,
    early_error_book: str,
    warning_back_book: str,
    warning_book: str,
    encrypted_error_book: str,
    error_book: str,
    runner_nonce: str,
    headless: bool,
    timeout: int,
) -> dict:
    environment = os.environ.copy()
    environment["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
    environment["CROSSINK_SIMULATOR_PDF_UI_ACCEPTANCE"] = "1"
    environment["CROSSINK_SIMULATOR_PDF_UI_BOOK"] = valid_book
    environment["CROSSINK_SIMULATOR_PDF_UI_EARLY_ERROR_BOOK"] = early_error_book
    environment["CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK"] = early_error_book
    environment["CROSSINK_SIMULATOR_PDF_UI_WARNING_BACK_BOOK"] = warning_back_book
    environment["CROSSINK_SIMULATOR_PDF_UI_WARNING_BOOK"] = warning_book
    environment["CROSSINK_SIMULATOR_PDF_UI_ENCRYPTED_ERROR_BOOK"] = (
        encrypted_error_book
    )
    environment["CROSSINK_SIMULATOR_PDF_UI_ERROR_BOOK"] = error_book
    environment["CROSSINK_SIMULATOR_PDF_UI_NONCE"] = runner_nonce
    if headless:
        environment.setdefault("SDL_VIDEODRIVER", "dummy")
    process = subprocess.run(
        [str(program)],
        cwd=cwd,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
        check=False,
    )
    print(process.stdout, end="")
    if process.returncode != 0:
        raise ValueError(
            f"PDF UI simulator exited with {process.returncode}"
        )
    for pattern in CRASH_PATTERNS:
        if pattern in process.stdout:
            raise ValueError(
                f"PDF UI simulator output contained {pattern}"
            )
    return parse_ui_acceptance(process.stdout, runner_nonce)


def _run_acceptance_phases(
    program: Path,
    cwd: Path,
    fixture_root: str,
    headless: bool,
    timeout: int,
    runner_nonce: str,
) -> tuple[dict, dict, dict]:
    cancelled = _run_program(
        program,
        cwd,
        fixture_root,
        "uncached",
        "cancel",
        runner_nonce,
        headless,
        timeout,
    )
    resumed = _run_program(
        program,
        cwd,
        fixture_root,
        "uncached",
        "resume",
        runner_nonce,
        headless,
        timeout,
    )
    cached = _run_program(
        program,
        cwd,
        fixture_root,
        "cached",
        "cached",
        runner_nonce,
        headless,
        timeout,
    )
    return cancelled, resumed, cached


def _run_epub_regression(program: Path, headless: bool, timeout: int) -> None:
    command = [
        sys.executable,
        str(EPUB_RUNNER),
        "--book",
        str(EPUB_BOOK),
        "--passes",
        "2",
        "--page-turns",
        "2",
        "--program",
        str(program),
        "--reflow-oracle",
        str(EPUB_ORACLE),
        "--no-build",
        "--timeout",
        str(timeout),
    ]
    if not headless:
        command.append("--window")
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=os.environ.copy(),
        check=False,
    )
    if completed.returncode != 0:
        raise ValueError("cached/uncached EPUB regression oracle failed")


def _container_workspace_path(path: Path) -> str:
    try:
        relative = path.resolve().relative_to(ROOT.resolve())
    except ValueError as error:
        raise ValueError(
            f"container path must be inside the repository: {path}"
        ) from error
    return relative.as_posix()


def _container_command(arguments: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        str(CONTAINER_RUNNER),
        "--image",
        arguments.image,
        "--",
        "python3",
        "scripts/run_pdf_simulator_acceptance.py",
    ]
    if arguments.headless:
        command.append("--headless")
    else:
        command.append("--window")
    if not arguments.build:
        command.append("--no-build")
    if arguments.update_oracle:
        command.append("--update-oracle")
    command.extend(
        ("--oracle", _container_workspace_path(Path(arguments.oracle)))
    )
    command.extend(("--timeout", str(arguments.timeout)))
    return command


def run(arguments: argparse.Namespace) -> int:
    if arguments.container:
        return subprocess.run(
            _container_command(arguments), cwd=ROOT, check=False
        ).returncode

    program = Path(arguments.program).resolve()
    if arguments.build:
        build = subprocess.run(
            ["pio", "run", "-e", "simulator"], cwd=ROOT, check=False
        )
        if build.returncode != 0:
            return build.returncode
    if not program.is_file():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        return 2

    try:
        expected_oracle = (
            None
            if arguments.update_oracle
            else _load_oracle(Path(arguments.oracle))
        )
        with tempfile.TemporaryDirectory(
            prefix="crossink-pdf-simulator-"
        ) as temporary_directory:
            temporary_root = Path(temporary_directory)
            staged = (
                temporary_root / "fs_" / "books" / "pdf-acceptance"
            )
            staged_hashes: dict[Path, str] = {}
            for fixture_name in FIXTURES:
                source = FIXTURE_SOURCE / fixture_name
                if not source.is_file():
                    raise ValueError(
                        f"PDF acceptance fixture is missing: {source}"
                    )
                destination = staged / fixture_name
                staged_hashes[destination] = stage_raw_fixture(
                    source, destination
                )
            baseline_source = (
                FIXTURE_SOURCE / FRESH_RESUME_BASELINE_SOURCE
            )
            baseline_destination = staged / FRESH_RESUME_BASELINE_NAME
            staged_hashes[baseline_destination] = stage_raw_fixture(
                baseline_source, baseline_destination
            )
            ui_valid_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui"
                / UI_VALID_FIXTURE
            )
            staged_hashes[ui_valid_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_VALID_FIXTURE,
                ui_valid_destination,
            )
            ui_error_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui-error"
                / UI_ERROR_FIXTURE
            )
            staged_hashes[ui_error_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_ERROR_FIXTURE,
                ui_error_destination,
            )
            ui_encrypted_error_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui-encrypted-error"
                / UI_ENCRYPTED_ERROR_FIXTURE
            )
            staged_hashes[ui_encrypted_error_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_ENCRYPTED_ERROR_FIXTURE,
                ui_encrypted_error_destination,
            )
            ui_early_error_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui-early-error"
                / UI_EARLY_ERROR_FIXTURE
            )
            staged_hashes[ui_early_error_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_EARLY_ERROR_FIXTURE,
                ui_early_error_destination,
            )
            ui_warning_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui-warning"
                / UI_WARNING_FIXTURE
            )
            staged_hashes[ui_warning_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_WARNING_FIXTURE,
                ui_warning_destination,
            )
            ui_warning_back_destination = (
                temporary_root
                / "fs_"
                / "books"
                / "pdf-ui-warning-back"
                / UI_WARNING_BACK_FIXTURE
            )
            staged_hashes[ui_warning_back_destination] = stage_raw_fixture(
                FIXTURE_SOURCE / UI_WARNING_BACK_FIXTURE,
                ui_warning_back_destination,
            )

            fixture_root = "/books/pdf-acceptance"
            runner_nonce = secrets.token_hex(16)
            cancelled, uncached, cached = _run_acceptance_phases(
                program,
                temporary_root,
                fixture_root,
                arguments.headless,
                arguments.timeout,
                runner_nonce,
            )
            for path, digest in staged_hashes.items():
                verify_staged_fixture(path, digest)
            runtime_oracle = uncached.get("oracle")
            if not isinstance(runtime_oracle, dict):
                raise ValueError("uncached result has no oracle")
            validate_acceptance(
                cancelled,
                uncached,
                cached,
                runtime_oracle if expected_oracle is None else expected_oracle,
            )
            _run_ui_acceptance(
                program,
                temporary_root,
                f"/books/pdf-ui/{UI_VALID_FIXTURE}",
                f"/books/pdf-ui-early-error/{UI_EARLY_ERROR_FIXTURE}",
                f"/books/pdf-ui-warning-back/{UI_WARNING_BACK_FIXTURE}",
                f"/books/pdf-ui-warning/{UI_WARNING_FIXTURE}",
                f"/books/pdf-ui-encrypted-error/{UI_ENCRYPTED_ERROR_FIXTURE}",
                f"/books/pdf-ui-error/{UI_ERROR_FIXTURE}",
                runner_nonce,
                arguments.headless,
                arguments.timeout,
            )
            for path, digest in staged_hashes.items():
                verify_staged_fixture(path, digest)

        _run_epub_regression(
            program, arguments.headless, arguments.timeout
        )
        if arguments.update_oracle:
            oracle_path = Path(arguments.oracle)
            oracle_path.parent.mkdir(parents=True, exist_ok=True)
            oracle_temporary_path = oracle_path.with_name(
                oracle_path.name + ".tmp"
            )
            oracle_temporary_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "oracle": runtime_oracle,
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            oracle_temporary_path.replace(oracle_path)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"PDF simulator acceptance failed: {error}", file=sys.stderr)
        return 2

    print(PASS_MARKER)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--container",
        action="store_true",
        help="Run through the pinned Ubuntu SDL simulator container",
    )
    parser.add_argument(
        "--image",
        default="crossink-pdf-simulator:ubuntu24.04-sdl2-v1",
        help="Pinned simulator container image",
    )
    parser.add_argument(
        "--program",
        default=str(DEFAULT_PROGRAM),
        help="Native simulator executable",
    )
    parser.add_argument(
        "--oracle",
        default=str(DEFAULT_ORACLE),
        help="Locked PDF framebuffer/oracle JSON",
    )
    parser.add_argument(
        "--update-oracle",
        action="store_true",
        help="Replace the locked PDF oracle after all runtime controls pass",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=180,
        help="Per-process timeout in seconds",
    )
    parser.add_argument(
        "--no-build",
        dest="build",
        action="store_false",
        help="Use the existing simulator executable",
    )
    parser.add_argument(
        "--headless",
        dest="headless",
        action="store_true",
        help="Use SDL's deterministic dummy video driver",
    )
    parser.add_argument(
        "--window",
        dest="headless",
        action="store_false",
        help="Show the SDL simulator window",
    )
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
