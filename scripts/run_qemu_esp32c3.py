import argparse
import hashlib
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import TextIO


FAIL_MARKER = re.compile(r"^QEMU_[A-Z0-9_]*FAIL(?:\s|$)")
PDF_CANCEL_SLICE_FAIL_PREFIX = "QEMU_PDF_CANCEL_FAIL reason=slice_budget"
PDF_CANCEL_SLICE_FAIL_MARKER = re.compile(
    r"^QEMU_PDF_CANCEL_FAIL reason=slice_budget elapsed_ms=(\d+) elapsed_us=(\d+) "
    r"io_calls=(\d+) io_kind=([a-z_]+) io_mode=([a-z_]+) io_recursive=([01]) io_request=(\d+) "
    r"callback_us=(\d+) nonio_us=(\d+) max_io_request=(\d+) generation=(\d+) "
    r"expected_generation=(\d+)$"
)
BOOT_MARKER = re.compile(r"^QEMU_BOOT seq=(\d+)$")
EXPECT_RESET_MARKER = re.compile(r"^QEMU_EXPECT_RESET seq=(\d+)$")
MAX_EXPECTED_RESETS = 8
UINT8_MAX = (1 << 8) - 1
UINT16_MAX = (1 << 16) - 1
UINT32_MAX = (1 << 32) - 1
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1
MAX_PREPARATION_STEPS = 100000
MAX_CANCELLATION_STEPS = MAX_PREPARATION_STEPS + 256
OUTPUT_FAILURES = (
    (re.compile(r"\bpanic\b", re.IGNORECASE), "panic"),
    (re.compile(r"Guru Meditation", re.IGNORECASE), "Guru Meditation"),
    (re.compile(r"\babort(?:\(\))?\b", re.IGNORECASE), "abort"),
    (re.compile(r"\bwatchdog\b", re.IGNORECASE), "watchdog"),
)
TRACER_EXPECTED_MARKER = "QEMU_TRACER_PASS"
FULL_EXPECTED_MARKER = "QEMU_TEST_PASS"
MAX_PDF_ALLOCATION_BYTES = 32768
EXPECTED_TYPOGRAPHY = (
    "95EE2813D71DFE2E",
    "E1AC47B687F6E82A",
)
EXPECTED_PDF_POSITIVE = (
    "DFAE2740CD6F6513",
    "715E72B598FFFFE3",
    "4BD86B77E1579064",
    "E72D737B2BF7D6CF",
)
EXPECTED_EPUB_ORACLE = (
    "46385061C46C2FE4",
    "4060CB229041492D",
    "3CC3246E367521F4",
    "DEE723508F423F9A",
    "E99DC1B84A90C006",
)
RUNTIME_MARKER = re.compile(
    r"^QEMU_RUNTIME heap_start=(\d+) min_free=(\d+) "
    r"min_max_alloc=(\d+) max_alloc=(\d+) stack_margin=(\d+)$"
)
TRACER_MARKERS = (
    re.compile(r"^QEMU_STORAGE_PASS path=/qemu/sentinel\.txt bytes=26$"),
    re.compile(r"^QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45$"),
    re.compile(r"^QEMU_INPUT_PASS button=DOWN press=1 release=1$"),
    re.compile(r"^QEMU_POWER_PASS idle_ms=3000 saving=1$"),
    RUNTIME_MARKER,
)
PDF_RAW_MARKER = re.compile(r"^QEMU_PDF_RAW_PASS files=15 unchanged=15$")
PDF_CANCEL_MARKER = re.compile(
    r"^QEMU_PDF_CANCEL_PASS generation=(\d+) steps=(\d+) "
    r"cancel_slices=(\d+) max_slice_ms=(\d+) "
    r"max_slice_us=(\d+) max_callback_us=(\d+) max_callback_kind=([a-z_]+) "
    r"max_slice_io=(\d+) max_io_request=(\d+)$"
)
PDF_SLOW_ATOMIC_MARKER = re.compile(
    r"^QEMU_PDF_SLOW_ATOMIC index=(\d+) slice=(\d+) calls=(\d+) "
    r"kind=([a-z_]+) mode=([a-z_]+) recursive=([01]) request=(\d+) "
    r"total_us=(\d+) callback_us=(\d+) nonio_us=(\d+)$"
)
PDF_SLOW_ATOMIC_SUMMARY_MARKER = re.compile(
    r"^QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=(\d+) slices=(\d+) "
    r"total=(\d+) write=(\d+) rename=(\d+) open_read=(\d+) "
    r"request_bytes=(\d+) callback_us=(\d+) nonio_us=(\d+) "
    r"max_total_us=(\d+) max_callback_us=(\d+)$"
)
MAX_QEMU_SLOW_ATOMIC_WRITE_US = 30000
MAX_QEMU_SLOW_ATOMIC_RENAME_US = 24000
MAX_QEMU_SLOW_ATOMIC_OPEN_READ_US = 12000
MAX_QEMU_SLOW_ATOMIC_NONIO_US = 500
MAX_QEMU_SLOW_ATOMIC_REQUEST_BYTES = 3072
MAX_QEMU_SLOW_ATOMIC_CALLBACK_US = 550000
MAX_QEMU_SLOW_ATOMIC_AGGREGATE_NONIO_US = 5000
MAX_QEMU_SLOW_ATOMIC_WRITES = 22
MAX_QEMU_SLOW_ATOMIC_RENAMES = 2
MAX_QEMU_SLOW_ATOMIC_OPEN_READS = 2
MAX_QEMU_SLOW_ATOMIC_TOTAL = 26
PDF_IO_OPERATION_NAMES = frozenset(
    (
        "open",
        "read",
        "write",
        "flush",
        "sync",
        "close",
        "remove",
        "mkdir",
        "list",
        "capacity",
        "metadata",
        "rename",
        "multiple",
    )
)
PDF_RESUME_MARKER = re.compile(
    r"^QEMU_PDF_RESUME_PASS generation=(\d+) resumed=(\d+) "
    r"fresh_steps=(\d+) resumed_steps=(\d+) "
    r"retained_truncate=(\d+) retained_remove=(\d+)$"
)
PDF_PROGRESS_MARKER = re.compile(
    r"^QEMU_PDF_PROGRESS_PASS words=(\d+) cursor=(\d+) "
    r"percent=(\d+) bookmark=1 clipping=1 resumed=1$"
)
PDF_FRAMEBUFFER_GUARD_MARKER = re.compile(
    r"^QEMU_PDF_FRAMEBUFFER_GUARD_PASS bytes=(\d+) checks=(\d+) "
    r"violations=(\d+) controls=(\d+) rejected=(\d+)$"
)
PDF_PROGRESS_MID_MARKER = re.compile(
    r"^QEMU_PDF_PROGRESS_MID_PASS words=(\d+) cursor=(\d+) "
    r"percent=(\d+) resumed=1$"
)
PDF_PROGRESS_PAGE_MARKER = re.compile(
    r"^QEMU_PDF_PROGRESS_PAGE section=(\d+) page=(\d+) page_count=(\d+) "
    r"found=([01]) valid=([01]) first=(\d+) last=(\d+) cursor=(\d+)$"
)
PDF_PROGRESS_DIAGNOSTIC_MARKER = re.compile(
    r"^QEMU_PDF_PROGRESS_DIAGNOSTIC selected_range=([01]) section=(-?\d+) "
    r"page=(-?\d+) word_start=(\d+) word_cursor=(\d+) total_words=(\d+) "
    r"saved=([01]) progress_ok=([01]) millionths=(\d+) page_count=(-?\d+)$"
)
PDF_TYPOGRAPHY_MARKER = re.compile(
    r"^QEMU_PDF_TYPOGRAPHY_PASS semantic_six=([0-9A-F]{16}) "
    r"semantic_seventy_two=([0-9A-F]{16}) text_six=([0-9A-F]{16}) "
    r"text_seventy_two=([0-9A-F]{16}) frame_six=([0-9A-F]{8}) "
    r"frame_seventy_two=([0-9A-F]{8}) blank=([0-9A-F]{8}) "
    r"words_six=(\d+) words_seventy_two=(\d+) "
    r"pages_six=(\d+) pages_seventy_two=(\d+) "
    r"font_id=(-?\d+) font_size=2 line_height=100$"
)
PDF_IMAGE_MARKER = re.compile(
    r"^QEMU_PDF_IMAGE_PASS retained=(\d+) "
    r"frame=([0-9A-F]{8}) blank=([0-9A-F]{8})$"
)
PDF_POSITIVE_MARKER = re.compile(
    r"^QEMU_PDF_POSITIVE_PASS ocr=([0-9A-F]{16}) ocr_words=(\d+) "
    r"columns=([0-9A-F]{16}) columns_words=(\d+) "
    r"table=([0-9A-F]{16}) table_words=(\d+) "
    r"jpeg=([0-9A-F]{16}) jpeg_words=(\d+) retained=(\d+) "
    r"decoded=(\d+) frame=([0-9A-F]{8}) blank=([0-9A-F]{8})$"
)
PDF_CORPUS_JPEG_DIAGNOSTIC_PREFIX = "QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC"
PDF_CORPUS_JPEG_DIAGNOSTIC_MARKER = re.compile(
    r"^QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC document=([01]) layout=([01]) "
    r"sections=(-?\d+) semantic_stream=([01]) content_stream=([01]) "
    r"words=(\d+) semantic=([0-9A-F]{16}) expected=([0-9A-F]{16}) "
    r"semantic_bytes=(\d+) image_tags=(\d+) captions=(\d+)$"
)
EPUB_ORACLE_MARKER = re.compile(
    r"^QEMU_EPUB_ORACLE_PASS pass=(uncached|cached) "
    r"xhtml0=([0-9A-F]{16}) xhtml1=([0-9A-F]{16}) "
    r"css=([0-9A-F]{16}) cache=([0-9A-F]{16}) "
    r"frame=([0-9A-F]{16})$"
)
EPUB_PROGRESS_MARKER = re.compile(
    r"^QEMU_EPUB_PROGRESS_PASS section=(\d+) page=(\d+) pages=(\d+) "
    r"book_millionths=(\d+) bookmark_millionths=(\d+) "
    r"snippet=(qemu-epub-smoke)$"
)
EPUB_STAGE_MARKER = re.compile(
    r"^QEMU_EPUB_STAGE pass=(uncached|cached) "
    r"stage=(preclean|begin|load|section|xhtml|css|cache|frame|end)"
    r"(?: index=(\d+))?(?: pages=(\d+))?$"
)
EPUB_REPRESENTATIVE_PAGES = 3


def _epub_expected_stages(
    pass_name: str, *, preclean: bool = False
) -> tuple[tuple[str, str, int | None, int | None], ...]:
    stages: list[tuple[str, str, int | None, int | None]] = []
    if preclean:
        stages.append((pass_name, "preclean", None, None))
    stages.extend(
        (
            (pass_name, "begin", None, None),
            (pass_name, "load", None, None),
        )
    )
    stages.extend(
        (
            (pass_name, "xhtml", 0, None),
            (pass_name, "xhtml", 1, None),
            (pass_name, "css", 0, None),
            (
                pass_name,
                "section",
                1,
                EPUB_REPRESENTATIVE_PAGES,
            ),
            (pass_name, "cache", 1, None),
            (pass_name, "frame", 1, None),
        )
    )
    stages.append((pass_name, "end", None, None))
    return tuple(stages)


EPUB_UNCACHED_EXPECTED_STAGES = _epub_expected_stages(
    "uncached", preclean=True
)
EPUB_EXPECTED_STAGES = EPUB_UNCACHED_EXPECTED_STAGES + _epub_expected_stages(
    "cached"
)
PDF_CACHE_REOPEN_MARKER = re.compile(
    r"^QEMU_PDF_CACHE_REOPEN_PASS page_turns=100 extraction=(\d+) "
    r"parser=(\d+) source_opens=(\d+) source_reads=(\d+) "
    r"source_max_request=(\d+) "
    r"heap_before=(\d+) heap_after=(\d+) "
    r"largest_before=(\d+) largest_after=(\d+) "
    r"stack_before=(\d+) stack_after=(\d+) frame=[0-9A-F]{8}$"
)
PDF_NEGATIVE_MARKER = re.compile(
    r"^QEMU_PDF_NEGATIVE_PASS checked=(\d+) rejected=(\d+) "
    r"forced_oom=([A-Za-z]+) completed_cache=(\d+)$"
)
FULL_MARKERS = (
    (0, TRACER_MARKERS[0]),
    (0, PDF_RAW_MARKER),
    (0, PDF_SLOW_ATOMIC_SUMMARY_MARKER),
    (0, PDF_CANCEL_MARKER),
    (0, RUNTIME_MARKER),
    (1, PDF_RESUME_MARKER),
    (
        1,
        PDF_TYPOGRAPHY_MARKER,
    ),
    (
        1,
        re.compile(
            r"^QEMU_PDF_NAV_PASS chapters=3 sections=2 words=10 "
            r"links=2 labels=2 index=1$"
        ),
    ),
    (
        1,
        PDF_IMAGE_MARKER,
    ),
    (
        1,
        PDF_POSITIVE_MARKER,
    ),
    (1, PDF_FRAMEBUFFER_GUARD_MARKER),
    (1, PDF_PROGRESS_MID_MARKER),
    (
        1,
        PDF_PROGRESS_MARKER,
    ),
    (1, PDF_CACHE_REOPEN_MARKER),
    (1, PDF_NEGATIVE_MARKER),
    (1, EPUB_ORACLE_MARKER),
    (1, EPUB_ORACLE_MARKER),
    (1, EPUB_PROGRESS_MARKER),
    (1, TRACER_MARKERS[0]),
    (1, TRACER_MARKERS[1]),
    (1, TRACER_MARKERS[2]),
    (1, TRACER_MARKERS[3]),
    (1, TRACER_MARKERS[4]),
    (1, re.compile(r"^QEMU_PDF_TRACER_PASS$")),
    (1, re.compile(r"^QEMU_TRACER_PASS$")),
)
FULL_BOOT_ZERO_MARKER_COUNT = 5


class OutputGuard:
    def __init__(self, expected_marker: str) -> None:
        self.expected_marker = expected_marker
        self.last_boot: int | None = None
        self.pending_reset: int | None = None
        self.reset_count = 0
        self.tracer_marker_index = 0
        self.tracer_sequence_error: str | None = None
        self.epub_oracle: tuple[str, ...] | None = None
        self.cancellation_generation: int | None = None
        self.slow_atomic_records: list[tuple[int, ...] | tuple[str, ...]] = []
        self.slow_atomic_slices: list[int] = []
        self.slow_atomic_writes = 0
        self.slow_atomic_renames = 0
        self.slow_atomic_open_reads = 0
        self.slow_atomic_request_bytes = 0
        self.slow_atomic_callback_us = 0
        self.slow_atomic_nonio_us = 0
        self.slow_atomic_max_total_us = 0
        self.slow_atomic_max_callback_us = 0
        self.slow_atomic_summary: tuple[int, ...] | None = None
        self.pdf_raw_seen_boot_zero = False
        self.progress_page_ranges: list[tuple[int, ...]] = []
        self.progress_diagnostic: tuple[int, ...] | None = None
        self.jpeg_diagnostic: tuple[int | str, ...] | None = None
        self.epub_stage_index = 0
        self.last_epub_stage: str | None = None

    def inspect(self, line: str) -> tuple[str | None, bool]:
        if (
            PDF_RAW_MARKER.fullmatch(line)
            and self.last_boot == 0
            and self.pending_reset is None
            and self.slow_atomic_summary is None
        ):
            self.pdf_raw_seen_boot_zero = True
        if line.startswith("QEMU_PDF_SLOW_ATOMIC "):
            if (
                self.last_boot != 0
                or not self.pdf_raw_seen_boot_zero
                or self.pending_reset is not None
                or self.slow_atomic_summary is not None
            ):
                return (
                    "QEMU slow atomic marker outside boot-0 RAW phase",
                    False,
                )
            marker = PDF_SLOW_ATOMIC_MARKER.fullmatch(line)
            if marker is None:
                return "malformed QEMU slow atomic I/O marker", False
            (
                raw_index,
                raw_slice,
                raw_calls,
                kind,
                mode,
                raw_recursive,
                raw_request,
                raw_total_us,
                raw_callback_us,
                raw_nonio_us,
            ) = marker.groups()
            index, slice_index, calls, recursive, request, total_us, callback_us, nonio_us = (
                int(raw_index),
                int(raw_slice),
                int(raw_calls),
                int(raw_recursive),
                int(raw_request),
                int(raw_total_us),
                int(raw_callback_us),
                int(raw_nonio_us),
            )
            allowed_kind = (
                kind == "write"
                and mode == "none"
                and 1 <= request <= 1024
                and callback_us <= MAX_QEMU_SLOW_ATOMIC_WRITE_US
            ) or (
                kind == "rename"
                and mode == "none"
                and request == 0
                and callback_us <= MAX_QEMU_SLOW_ATOMIC_RENAME_US
            ) or (
                kind == "open"
                and mode == "read"
                and request == 0
                and callback_us <= MAX_QEMU_SLOW_ATOMIC_OPEN_READ_US
            )
            if (
                self.slow_atomic_summary is not None
                or index != len(self.slow_atomic_records)
                or slice_index >= 256
                or (
                    self.slow_atomic_slices
                    and slice_index <= self.slow_atomic_slices[-1]
                )
                or calls != 1
                or recursive != 0
                or total_us <= 8000
                or callback_us > total_us
                or nonio_us != total_us - callback_us
                or nonio_us > MAX_QEMU_SLOW_ATOMIC_NONIO_US
                or not allowed_kind
            ):
                return "invalid QEMU slow atomic I/O marker", False
            self.slow_atomic_records.append(
                (
                    index,
                    slice_index,
                    calls,
                    kind,
                    mode,
                    recursive,
                    request,
                    total_us,
                    callback_us,
                    nonio_us,
                )
            )
            self.slow_atomic_slices.append(slice_index)
            self.slow_atomic_writes += int(kind == "write")
            self.slow_atomic_renames += int(kind == "rename")
            self.slow_atomic_open_reads += int(kind == "open")
            self.slow_atomic_request_bytes += request
            self.slow_atomic_callback_us += callback_us
            self.slow_atomic_nonio_us += nonio_us
            self.slow_atomic_max_total_us = max(
                self.slow_atomic_max_total_us, total_us
            )
            self.slow_atomic_max_callback_us = max(
                self.slow_atomic_max_callback_us, callback_us
            )
            if (
                len(self.slow_atomic_records) > MAX_QEMU_SLOW_ATOMIC_TOTAL
                or self.slow_atomic_writes > MAX_QEMU_SLOW_ATOMIC_WRITES
                or self.slow_atomic_renames > MAX_QEMU_SLOW_ATOMIC_RENAMES
                or self.slow_atomic_open_reads > MAX_QEMU_SLOW_ATOMIC_OPEN_READS
                or self.slow_atomic_request_bytes
                > MAX_QEMU_SLOW_ATOMIC_REQUEST_BYTES
                or self.slow_atomic_callback_us
                > MAX_QEMU_SLOW_ATOMIC_CALLBACK_US
                or self.slow_atomic_nonio_us
                > MAX_QEMU_SLOW_ATOMIC_AGGREGATE_NONIO_US
            ):
                return "QEMU slow atomic aggregate limit exceeded", False
            return None, False

        if line.startswith("QEMU_PDF_SLOW_ATOMIC_SUMMARY"):
            marker = PDF_SLOW_ATOMIC_SUMMARY_MARKER.fullmatch(line)
            if marker is None:
                return "malformed QEMU slow atomic summary marker", False
            values = tuple(int(value) for value in marker.groups())
            (
                generation,
                slices,
                total,
                writes,
                renames,
                open_reads,
                request_bytes,
                callback_us,
                nonio_us,
                max_total_us,
                max_callback_us,
            ) = values
            if (
                self.slow_atomic_summary is not None
                or not 1 <= generation <= UINT32_MAX
                or not 2 <= slices <= 256
                or (self.slow_atomic_slices and self.slow_atomic_slices[-1] >= slices)
                or total != len(self.slow_atomic_records)
                or writes != self.slow_atomic_writes
                or renames != self.slow_atomic_renames
                or open_reads != self.slow_atomic_open_reads
                or request_bytes != self.slow_atomic_request_bytes
                or callback_us != self.slow_atomic_callback_us
                or nonio_us != self.slow_atomic_nonio_us
                or max_total_us != self.slow_atomic_max_total_us
                or max_callback_us != self.slow_atomic_max_callback_us
            ):
                return "QEMU slow atomic summary mismatch", False
            self.slow_atomic_summary = values

        if line.startswith(PDF_CANCEL_SLICE_FAIL_PREFIX):
            diagnostic = PDF_CANCEL_SLICE_FAIL_MARKER.fullmatch(line)
            if diagnostic is None:
                return "malformed PDF cancellation slice budget marker", False
            (
                elapsed,
                elapsed_us,
                io_calls,
                io_kind,
                io_mode,
                io_recursive,
                io_request,
                callback_us,
                nonio_us,
                maximum_request,
                generation,
                expected_generation,
            ) = diagnostic.groups()
            return (
                "PDF cancellation slice budget exceeded: "
                f"elapsed_ms={elapsed} elapsed_us={elapsed_us} io_calls={io_calls} "
                f"io_kind={io_kind} io_mode={io_mode} io_recursive={io_recursive} io_request={io_request} "
                f"callback_us={callback_us} nonio_us={nonio_us} "
                f"max_io_request={maximum_request} generation={generation} "
                f"expected_generation={expected_generation}",
                False,
            )
        if line.startswith("QEMU_PDF_PROGRESS_PAGE"):
            page = PDF_PROGRESS_PAGE_MARKER.fullmatch(line)
            if page is None:
                return "malformed PDF progress page diagnostic marker", False
            values = tuple(int(value) for value in page.groups())
            if any(value > UINT32_MAX for value in values):
                return "PDF progress page diagnostic exceeds uint32_t", False
            self.progress_page_ranges.append(values)
            return None, False
        if line.startswith("QEMU_PDF_PROGRESS_DIAGNOSTIC"):
            diagnostic = PDF_PROGRESS_DIAGNOSTIC_MARKER.fullmatch(line)
            if diagnostic is None:
                return "malformed PDF progress diagnostic marker", False
            values = tuple(int(value) for value in diagnostic.groups())
            if (
                not INT32_MIN <= values[1] <= INT32_MAX
                or not INT32_MIN <= values[2] <= INT32_MAX
                or not INT32_MIN <= values[9] <= INT32_MAX
                or any(value > UINT32_MAX for value in values[3:9])
                or values[8] > 1000000
            ):
                return "PDF progress diagnostic exceeds numeric bounds", False
            self.progress_diagnostic = values
            return None, False
        if line.startswith(PDF_CORPUS_JPEG_DIAGNOSTIC_PREFIX):
            diagnostic = PDF_CORPUS_JPEG_DIAGNOSTIC_MARKER.fullmatch(line)
            if diagnostic is None:
                return "malformed PDF JPEG retained diagnostic marker", False
            raw_values = diagnostic.groups()
            values: tuple[int | str, ...] = (
                *(int(value) for value in raw_values[:6]),
                raw_values[6],
                raw_values[7],
                *(int(value) for value in raw_values[8:]),
            )
            sections = int(raw_values[2])
            words, semantic_bytes, image_tags, captions = (
                int(raw_values[5]),
                int(raw_values[8]),
                int(raw_values[9]),
                int(raw_values[10]),
            )
            if (
                self.jpeg_diagnostic is not None
                or not -1 <= sections <= INT32_MAX
                or words > UINT32_MAX
                or semantic_bytes > UINT32_MAX
                or image_tags > UINT16_MAX
                or captions > UINT16_MAX
            ):
                return "invalid PDF JPEG retained diagnostic marker", False
            self.jpeg_diagnostic = values
            return None, False
        if line.startswith("QEMU_EPUB_STAGE"):
            marker = EPUB_STAGE_MARKER.fullmatch(line)
            if marker is None:
                return "malformed EPUB stage marker", False
            pass_name, stage, raw_index, raw_pages = marker.groups()
            actual = (
                pass_name,
                stage,
                int(raw_index) if raw_index is not None else None,
                int(raw_pages) if raw_pages is not None else None,
            )
            if self.epub_stage_index >= len(EPUB_EXPECTED_STAGES):
                return "unexpected extra EPUB stage marker", False
            expected = EPUB_EXPECTED_STAGES[self.epub_stage_index]
            if actual != expected:
                return (
                    "out-of-order EPUB stage marker: "
                    f"expected={expected} received={actual}"
                ), False
            self.epub_stage_index += 1
            self.last_epub_stage = line.removeprefix("QEMU_EPUB_STAGE ")
            return None, False
        if FAIL_MARKER.match(line):
            return f"failure marker observed: {line}", False
        for pattern, label in OUTPUT_FAILURES:
            if pattern.search(line):
                return f"{label} observed in QEMU output", False

        boot_match = BOOT_MARKER.match(line)
        if boot_match:
            error = self._accept_boot(int(boot_match.group(1)))
            if error:
                return error, False
            if self.expected_marker == TRACER_EXPECTED_MARKER:
                self.tracer_marker_index = 0
                self.tracer_sequence_error = None

        reset_match = EXPECT_RESET_MARKER.match(line)
        if reset_match:
            error = self._arm_reset(int(reset_match.group(1)))
            if error:
                return error, False

        tracer_error = self._accept_tracer_marker(line)
        if tracer_error:
            return tracer_error, False

        if line == self.expected_marker:
            if self.pending_reset is not None:
                return "terminal marker arrived before armed reset", False
            if self.expected_marker == TRACER_EXPECTED_MARKER:
                if self.tracer_sequence_error is not None:
                    return self.tracer_sequence_error, False
                if self.tracer_marker_index != len(TRACER_MARKERS):
                    return (
                        "missing required tracer marker before terminal marker "
                        f"(next index {self.tracer_marker_index})"
                    ), False
            if self.expected_marker == FULL_EXPECTED_MARKER:
                if self.tracer_marker_index != len(FULL_MARKERS):
                    return (
                        "missing required full-acceptance marker before "
                        f"terminal marker (next index "
                        f"{self.tracer_marker_index})"
                    ), False
                if self.reset_count != 1 or self.last_boot != 1:
                    return (
                        "full acceptance requires exactly one armed "
                        "persistent reset"
                    ), False
            return None, True
        return None, False

    def _accept_tracer_marker(self, line: str) -> str | None:
        if self.expected_marker == TRACER_EXPECTED_MARKER:
            markers = TRACER_MARKERS
        elif self.expected_marker == FULL_EXPECTED_MARKER:
            markers = tuple(pattern for _, pattern in FULL_MARKERS)
        else:
            return None

        if self.expected_marker == FULL_EXPECTED_MARKER:
            validation_error = self._validate_full_marker(line)
            if validation_error:
                return validation_error

        if (
            self.tracer_marker_index < len(markers)
            and markers[self.tracer_marker_index].fullmatch(line)
        ):
            if self.last_boot is None:
                return "acceptance marker arrived before initial QEMU boot"
            if self.expected_marker == FULL_EXPECTED_MARKER:
                required_boot = FULL_MARKERS[self.tracer_marker_index][0]
                if self.last_boot != required_boot:
                    return (
                        "acceptance marker emitted on wrong boot "
                        f"(required boot {required_boot}, "
                        f"current boot {self.last_boot})"
                    )
            self.tracer_marker_index += 1
            return None

        if any(pattern.fullmatch(line) for pattern in markers):
            if self.expected_marker == FULL_EXPECTED_MARKER:
                matching_boots = {
                    boot
                    for boot, pattern in FULL_MARKERS
                    if pattern.fullmatch(line)
                }
                if (
                    self.last_boot is not None
                    and self.last_boot not in matching_boots
                ):
                    return (
                        "acceptance marker emitted on wrong boot "
                        f"(required boot {min(matching_boots)}, "
                        f"current boot {self.last_boot})"
                    )
            marker_kind = (
                "tracer"
                if self.expected_marker == TRACER_EXPECTED_MARKER
                else "acceptance"
            )
            error = (
                f"out-of-order {marker_kind} marker "
                f"(expected index {self.tracer_marker_index})"
            )
            if self.expected_marker == TRACER_EXPECTED_MARKER:
                if self.tracer_sequence_error is None:
                    self.tracer_sequence_error = error
                return None
            return error
        return None

    def _validate_full_marker(self, line: str) -> str | None:
        cancellation = PDF_CANCEL_MARKER.fullmatch(line)
        if cancellation:
            (
                generation,
                steps,
                slices,
                milliseconds,
                microseconds,
                callback_microseconds,
                callback_kind,
                io_calls,
                io_request,
            ) = cancellation.groups()
            generation = int(generation)
            steps = int(steps)
            slices = int(slices)
            milliseconds = int(milliseconds)
            microseconds = int(microseconds)
            callback_microseconds = int(callback_microseconds)
            io_calls = int(io_calls)
            io_request = int(io_request)
            if self.slow_atomic_summary is None:
                return "missing QEMU slow atomic summary"
            summary_generation = self.slow_atomic_summary[0]
            summary_slices = self.slow_atomic_summary[1]
            if self.slow_atomic_records:
                minimum_ms = self.slow_atomic_max_total_us // 1000
                maximum_ms = (self.slow_atomic_max_total_us + 999) // 1000
                slow_max_callback_us = self.slow_atomic_max_callback_us
                slow_max_callback_kind = next(
                    record[3]
                    for record in self.slow_atomic_records
                    if record[8] == slow_max_callback_us
                )
                slow_max_request = max(
                    record[6] for record in self.slow_atomic_records
                )
                timing_valid = (
                    microseconds == self.slow_atomic_max_total_us
                    and minimum_ms <= milliseconds <= maximum_ms
                    and slow_max_callback_us <= callback_microseconds
                    and callback_microseconds
                    <= max(8000, slow_max_callback_us)
                    and (
                        callback_microseconds <= 8000
                        and callback_kind in PDF_IO_OPERATION_NAMES
                        or callback_microseconds > 8000
                        and callback_kind == slow_max_callback_kind
                    )
                    and io_calls >= 1
                    and io_request >= slow_max_request
                )
            else:
                timing_valid = (
                    milliseconds <= 8
                    and microseconds <= 8000
                    and callback_microseconds <= microseconds
                    and callback_kind in PDF_IO_OPERATION_NAMES
                )
            if (
                not 1 <= generation <= UINT32_MAX
                or generation != summary_generation
                or not 1 <= steps <= MAX_CANCELLATION_STEPS
                or steps < slices
                or slices > UINT16_MAX
                or not 2 <= slices <= 256
                or slices != summary_slices
                or not timing_valid
                or not 0 <= io_calls <= 32
                or not 1 <= io_request <= 4096
            ):
                return "PDF cancellation limits exceeded"
            self.cancellation_generation = generation

        resume = PDF_RESUME_MARKER.fullmatch(line)
        if resume:
            generation, resumed, fresh_steps, resumed_steps, truncate, remove = (
                int(value) for value in resume.groups()
            )
            if (
                not 1 <= generation <= UINT32_MAX
                or generation != self.cancellation_generation
                or resumed != 1
                or not 1 <= fresh_steps <= MAX_PREPARATION_STEPS
                or not 1 <= resumed_steps <= MAX_PREPARATION_STEPS
                or resumed_steps >= fresh_steps
                or truncate != 0
                or remove != 0
            ):
                return "invalid PDF checkpoint continuation counters"

        runtime = RUNTIME_MARKER.fullmatch(line)
        if runtime:
            heap_start, min_free, min_largest, max_allocation, stack = (
                int(value) for value in runtime.groups()
            )
            if any(
                value > UINT32_MAX
                for value in (
                    heap_start,
                    min_free,
                    min_largest,
                    max_allocation,
                    stack,
                )
            ):
                return "QEMU runtime value exceeds uint32_t"
            if heap_start == 0 or min_free == 0:
                return "QEMU runtime resource floor is zero"
            if min_free > heap_start:
                return "QEMU runtime minimum free heap exceeds heap start"
            if min_largest > min_free or max_allocation > min_free:
                return "QEMU runtime heap relationships are invalid"
            if min_largest == 0 or stack == 0:
                return "QEMU runtime resource floor is zero"
            if not 1 <= max_allocation <= MAX_PDF_ALLOCATION_BYTES:
                return (
                    "PDF allocation exceeds "
                    f"{MAX_PDF_ALLOCATION_BYTES}-byte limit"
                )

        framebuffer = PDF_FRAMEBUFFER_GUARD_MARKER.fullmatch(line)
        if framebuffer:
            framebuffer_bytes, checks, violations, controls, rejected = (
                int(value) for value in framebuffer.groups()
            )
            if (
                framebuffer_bytes != 48000
                or not 1 <= checks <= UINT32_MAX
                or violations != 0
                or controls != checks * 2
                or rejected != controls
            ):
                return "invalid PDF framebuffer guard witness"

        mid_progress = PDF_PROGRESS_MID_MARKER.fullmatch(line)
        if mid_progress:
            words, cursor, percent = (
                int(value) for value in mid_progress.groups()
            )
            if (words, cursor, percent) != (10, 6, 60):
                return "invalid PDF mid-word progress witness"

        progress = PDF_PROGRESS_MARKER.fullmatch(line)
        if progress:
            words, cursor, percent = (int(value) for value in progress.groups())
            expected_percent = (cursor * 100 + words // 2) // words if words else 0
            if (
                (words, cursor, percent) != (10, 10, 100)
                or percent != expected_percent
            ):
                return "invalid PDF word progress witness"

        typography = PDF_TYPOGRAPHY_MARKER.fullmatch(line)
        if typography:
            semantic_six, semantic_seventy_two, text_six, text_seventy_two = (
                typography.groups()[:4]
            )
            if (
                semantic_six != EXPECTED_TYPOGRAPHY[0]
                or semantic_seventy_two != EXPECTED_TYPOGRAPHY[0]
                or text_six != EXPECTED_TYPOGRAPHY[1]
                or text_seventy_two != EXPECTED_TYPOGRAPHY[1]
            ):
                return "PDF typography witness differs from fixture oracle"
            six_frame = int(typography.group(5), 16)
            seventy_two_frame = int(typography.group(6), 16)
            blank_frame = int(typography.group(7), 16)
            if (
                six_frame == 0
                or seventy_two_frame == 0
                or six_frame != seventy_two_frame
                or six_frame == blank_frame
                or seventy_two_frame == blank_frame
            ):
                return "PDF typography frame witness is blank or inconsistent"
            if tuple(int(value) for value in typography.groups()[7:11]) != (
                4,
                4,
                1,
                1,
            ):
                return "PDF typography word or page witness differs from fixture oracle"
            font_id = int(typography.group(12))
            if not INT32_MIN <= font_id <= INT32_MAX:
                return "PDF typography font id exceeds int32_t"

        image = PDF_IMAGE_MARKER.fullmatch(line)
        if image:
            retained, frame, blank = image.groups()
            if int(retained) != 1 or int(frame, 16) == int(blank, 16):
                return "PDF image witness differs from fixture oracle"

        positive = PDF_POSITIVE_MARKER.fullmatch(line)
        if positive:
            (
                ocr,
                ocr_words,
                columns,
                columns_words,
                table,
                table_words,
                jpeg,
                jpeg_words,
                retained,
                decoded,
                frame,
                blank,
            ) = positive.groups()
            if ocr != EXPECTED_PDF_POSITIVE[0] or int(ocr_words) != 4:
                return "positive corpus OCR witness differs from fixture oracle"
            if columns != EXPECTED_PDF_POSITIVE[1] or int(columns_words) != 8:
                return "positive corpus column witness differs from fixture oracle"
            if table != EXPECTED_PDF_POSITIVE[2] or int(table_words) != 4:
                return "positive corpus table witness differs from fixture oracle"
            if (
                jpeg != EXPECTED_PDF_POSITIVE[3]
                or int(jpeg_words) != 2
                or int(retained) != 1
                or int(decoded) != 1
                or int(frame, 16) == 0
                or int(frame, 16) == int(blank, 16)
            ):
                return "positive corpus JPEG witness differs from fixture oracle"

        cached = PDF_CACHE_REOPEN_MARKER.fullmatch(line)
        if cached:
            (
                extraction,
                parser,
                source_opens,
                source_reads,
                source_max_request,
                heap_before,
                heap_after,
                largest_before,
                largest_after,
                stack_before,
                stack_after,
            ) = (int(value) for value in cached.groups())
            if any(
                value > UINT32_MAX
                for value in (
                    extraction,
                    parser,
                    source_opens,
                    source_reads,
                    source_max_request,
                    heap_before,
                    heap_after,
                    largest_before,
                    largest_after,
                    stack_before,
                    stack_after,
                )
            ):
                return "cached-turn value exceeds uint32_t"
            if extraction != 0 or parser != 0:
                return "cached-turn parser/extraction counters changed"
            if source_opens != 1:
                return "cached identity source open count is not one"
            if source_reads == 0 or source_reads > 2:
                return "cached identity read count is outside limits"
            if not 1 <= source_max_request <= 4096:
                return "cached identity request is outside limits"
            if not all(
                value > 0
                for value in (
                    heap_before,
                    heap_after,
                    largest_before,
                    largest_after,
                    stack_before,
                    stack_after,
                )
            ):
                return "cached-turn resource snapshot invalid"
            if (
                largest_before > heap_before
                or largest_after > heap_after
                or heap_after < heap_before
                or largest_after < largest_before
                or stack_after < stack_before
            ):
                return "cached-turn memory eroded"

        negative = PDF_NEGATIVE_MARKER.fullmatch(line)
        if negative:
            checked, rejected, forced_error, completed_cache = (
                negative.groups()
            )
            if (
                int(checked) != 7
                or int(rejected) != 7
                or forced_error != "InsufficientMemory"
                or int(completed_cache) != 0
            ):
                return "forced OOM negative witness invalid"

        epub = EPUB_ORACLE_MARKER.fullmatch(line)
        if epub:
            pass_name, *oracle = epub.groups()
            required_stage_count = (
                len(EPUB_UNCACHED_EXPECTED_STAGES)
                if pass_name == "uncached"
                else len(EPUB_EXPECTED_STAGES)
            )
            if self.epub_stage_index != required_stage_count:
                return f"{pass_name} EPUB oracle is missing ordered stage markers"
            oracle_tuple = tuple(oracle)
            if oracle_tuple != EXPECTED_EPUB_ORACLE:
                return f"{pass_name} EPUB oracle differs from fixture"
            if pass_name == "uncached":
                if self.epub_oracle is not None:
                    return "duplicate uncached EPUB oracle"
                self.epub_oracle = oracle_tuple
            elif self.epub_oracle is None:
                return "cached EPUB oracle arrived before uncached oracle"
            elif oracle_tuple != self.epub_oracle:
                return "cached EPUB oracle drifted from uncached oracle"

        epub_progress = EPUB_PROGRESS_MARKER.fullmatch(line)
        if epub_progress:
            section, page, pages, book_millionths, bookmark_millionths, snippet = (
                epub_progress.groups()
            )
            if (
                (int(section), int(page), int(pages)) != (1, 1, 3)
                or int(book_millionths) != 643454
                or int(bookmark_millionths) != 333333
                or snippet != "qemu-epub-smoke"
            ):
                return "EPUB progress or bookmark witness differs from fixture"
        return None

    def timeout_failure(self, timeout: float) -> str:
        last_stage = self.last_epub_stage or "none"
        return (
            f"QEMU timed out after {timeout:g} seconds; "
            f"last_epub_stage={last_stage}"
        )

    def _accept_boot(self, sequence: int) -> str | None:
        if sequence > UINT8_MAX:
            return f"QEMU boot sequence exceeds uint8_t: {sequence}"
        if self.last_boot is None:
            if sequence != 0:
                return f"first QEMU boot has invalid sequence {sequence}"
            self.last_boot = sequence
            return None

        if self.pending_reset is None:
            if sequence == self.last_boot:
                return f"restart loop detected at boot sequence {sequence}"
            return f"unarmed reset detected at boot sequence {sequence}"

        expected_boot = self.pending_reset + 1
        if sequence != expected_boot:
            return (
                f"armed reset expected boot sequence {expected_boot}, "
                f"received {sequence}"
            )
        self.last_boot = sequence
        self.pending_reset = None
        return None

    def _arm_reset(self, sequence: int) -> str | None:
        if sequence > UINT8_MAX:
            return f"reset sequence exceeds uint8_t: {sequence}"
        if self.last_boot is None:
            return "reset armed before initial QEMU boot"
        if (
            self.expected_marker == FULL_EXPECTED_MARKER
            and sequence == 0
            and self.tracer_marker_index != FULL_BOOT_ZERO_MARKER_COUNT
        ):
            return "full acceptance is missing the boot 0 runtime before reset"
        if self.pending_reset is not None:
            return "repeated reset arm without an intervening boot"
        if sequence != self.last_boot:
            return (
                f"reset arm sequence {sequence} does not match "
                f"boot sequence {self.last_boot}"
            )
        if self.reset_count >= MAX_EXPECTED_RESETS:
            return "expected reset limit exceeded"
        self.pending_reset = sequence
        self.reset_count += 1
        return None


def _pump_output(stream: TextIO, output_queue: queue.Queue[str | None]) -> None:
    try:
        for line in stream:
            output_queue.put(line)
    finally:
        output_queue.put(None)


def _terminate(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=1.0)


def _command(qemu: Path) -> list[str]:
    if qemu.suffix.lower() == ".py":
        return [sys.executable, str(qemu)]
    return [str(qemu)]


def _monitor(command: list[str], arguments: argparse.Namespace) -> int:
    if arguments.timeout <= 0:
        sys.stderr.write("QEMU timeout must be greater than zero\n")
        return 1

    arguments.log.parent.mkdir(parents=True, exist_ok=True)
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as error:
        sys.stderr.write(f"cannot start QEMU: {error}\n")
        return 1

    assert process.stdout is not None
    output_queue: queue.Queue[str | None] = queue.Queue()
    reader = threading.Thread(
        target=_pump_output,
        args=(process.stdout, output_queue),
        daemon=True,
    )
    reader.start()

    guard = OutputGuard(arguments.expect)
    deadline = time.monotonic() + arguments.timeout
    end_of_output = False
    failure: str | None = None
    passed = False

    try:
        with arguments.log.open("w", encoding="utf-8", newline="\n") as log:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    failure = guard.timeout_failure(arguments.timeout)
                    break
                try:
                    item = output_queue.get(timeout=min(0.05, remaining))
                except queue.Empty:
                    item = ""

                if item is None:
                    end_of_output = True
                elif item:
                    line = item.rstrip("\r\n")
                    error, terminal = guard.inspect(line)
                    log.write(item)
                    log.flush()
                    if not (error and line == arguments.expect):
                        sys.stdout.write(item)
                        sys.stdout.flush()
                    if error:
                        failure = error
                        break
                    if terminal:
                        passed = True
                        break

                exit_code = process.poll()
                if exit_code is not None and end_of_output:
                    if exit_code == 0:
                        failure = "QEMU exited with missing terminal marker"
                    else:
                        failure = f"QEMU unexpected exit with code {exit_code}"
                    break
    except OSError as error:
        failure = f"cannot write QEMU log: {error}"
    finally:
        _terminate(process)
        process.stdout.close()
        reader.join(timeout=1.0)

    if passed and arguments.timing_diagnostic:
        sys.stderr.write("QEMU timing diagnostic is intentionally non-accepting\n")
        return 1
    if passed:
        return 0
    sys.stderr.write((failure or "QEMU runner failed") + "\n")
    return 1


def _load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} must contain a JSON object")
    return value


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(64 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _manifest_artifact(
    entries: object, name: str, label: str
) -> Path:
    entry = entries.get(name) if isinstance(entries, dict) else None
    raw_path = entry.get("path") if isinstance(entry, dict) else None
    expected_hash = entry.get("sha256") if isinstance(entry, dict) else None
    expected_size = entry.get("size") if isinstance(entry, dict) else None
    if not isinstance(raw_path, str):
        raise RuntimeError(f"QEMU manifest has no {label} path")
    path = Path(raw_path)
    if not path.is_absolute():
        raise RuntimeError(f"QEMU {label} path must be absolute")
    if not path.is_file():
        raise RuntimeError(f"QEMU {label} does not exist: {path}")
    if not isinstance(expected_hash, str) or re.fullmatch(
        r"[0-9a-f]{64}", expected_hash
    ) is None:
        raise RuntimeError(f"QEMU manifest {label} SHA-256 is invalid")
    if _sha256_file(path) != expected_hash:
        raise RuntimeError(f"QEMU manifest {label} SHA-256 mismatch")
    if (
        not isinstance(expected_size, int)
        or isinstance(expected_size, bool)
        or expected_size < 0
        or path.stat().st_size != expected_size
    ):
        raise RuntimeError(f"QEMU manifest {label} size mismatch")
    return path.resolve()


def _default_install_receipt() -> Path:
    return (
        Path(__file__).resolve().parents[1]
        / ".tools"
        / "qemu-esp32c3"
        / "install.json"
    )


def _default_manifest() -> Path:
    return (
        Path(__file__).resolve().parents[1]
        / ".pio"
        / "build"
        / "qemu-esp32c3"
        / "qemu_manifest.json"
    )


def _installed_qemu(receipt_path: Path) -> Path:
    receipt = _load_json(receipt_path)
    raw_executable = receipt.get("executable")
    if not isinstance(raw_executable, str):
        raise RuntimeError("QEMU install receipt has no executable")
    executable = Path(raw_executable)
    if not executable.is_absolute():
        raise RuntimeError("QEMU install receipt executable must be absolute")
    if not executable.is_file():
        raise RuntimeError(f"QEMU executable does not exist: {executable}")
    return executable


def _manifest_images(manifest_path: Path) -> tuple[Path, Path]:
    manifest = _load_json(manifest_path)
    if manifest.get("schema_version") != 1:
        raise RuntimeError("QEMU manifest schema_version is not 1")
    images = manifest.get("images")
    if not isinstance(images, dict):
        raise RuntimeError("QEMU manifest has no images object")
    resolved = [
        _manifest_artifact(images, "flash", "flash"),
        _manifest_artifact(images, "efuse", "efuse"),
    ]
    _manifest_artifact(manifest.get("artifacts"), "elf", "ELF")
    return resolved[0], resolved[1]


def _qemu_machine_command(
    executable: Path, flash_image: Path, efuse_image: Path
) -> list[str]:
    return _command(executable) + [
        "-M",
        "esp32c3",
        "-icount",
        "shift=3,sleep=off",
        "-drive",
        f"file={flash_image},if=mtd,format=raw",
        "-drive",
        f"file={efuse_image},if=none,format=raw,id=efuse",
        "-global",
        "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
        "-nic",
        "none",
        "-nographic",
        "-serial",
        "mon:stdio",
    ]


def _run(arguments: argparse.Namespace) -> int:
    if arguments.qemu is not None and arguments.manifest is None:
        if not arguments.qemu.is_file():
            sys.stderr.write(
                f"QEMU executable does not exist: {arguments.qemu}\n"
            )
            return 1
        return _monitor(_command(arguments.qemu), arguments)

    try:
        executable = (
            arguments.qemu
            if arguments.qemu is not None
            else _installed_qemu(
                arguments.install or _default_install_receipt()
            )
        )
        if not executable.is_file():
            raise RuntimeError(
                f"QEMU executable does not exist: {executable}"
            )
        flash_source, efuse_source = _manifest_images(
            arguments.manifest or _default_manifest()
        )
        with tempfile.TemporaryDirectory(
            prefix="crossink-qemu-run-"
        ) as temporary_directory:
            temporary = Path(temporary_directory)
            flash_copy = temporary / "qemu_flash.bin"
            efuse_copy = temporary / "qemu_efuse.bin"
            shutil.copy2(flash_source, flash_copy)
            shutil.copy2(efuse_source, efuse_copy)
            return _monitor(
                _qemu_machine_command(
                    executable, flash_copy, efuse_copy
                ),
                arguments,
            )
    except (OSError, RuntimeError) as error:
        sys.stderr.write(f"QEMU runner setup failed: {error}\n")
        return 1


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--install", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expect", required=True)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--timing-diagnostic", action="store_true")
    return parser


def main() -> int:
    return _run(_build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
