import hashlib
import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "run_qemu_esp32c3.py"
EXPECTED_MARKER = "QEMU_TRACER_PASS"
FULL_EXPECTED_MARKER = "QEMU_TEST_PASS"
CACHE_REOPEN_WITH_METRICS = (
    "QEMU_PDF_CACHE_REOPEN_PASS page_turns=100 extraction=0 "
    "parser=0 source_opens=1 source_reads=2 "
    "source_max_request=4096 "
    "heap_before=90000 heap_after=90000 "
    "largest_before=60000 largest_after=60000 "
    "stack_before=2000 stack_after=2000 frame=13579BDF"
)
NEGATIVE_WITH_FORCED_OOM = (
    "QEMU_PDF_NEGATIVE_PASS checked=7 rejected=7 "
    "forced_oom=InsufficientMemory completed_cache=0"
)
SLOW_ATOMIC_LINES = (
    (
        "QEMU_PDF_SLOW_ATOMIC index=0 slice=0 calls=1 kind=write mode=none "
        "recursive=0 request=1024 total_us=18500 callback_us=18200 nonio_us=300"
    ),
    (
        "QEMU_PDF_SLOW_ATOMIC index=1 slice=1 calls=1 kind=open mode=read "
        "recursive=0 request=0 total_us=9000 callback_us=8600 nonio_us=400"
    ),
)
SLOW_ATOMIC_SUMMARY = (
    "QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=1 slices=2 total=2 write=1 "
    "rename=0 open_read=1 request_bytes=1024 callback_us=26800 "
    "nonio_us=700 max_total_us=18500 max_callback_us=18200"
)
EPUB_REPRESENTATIVE_PAGES = 3
def _epub_stage_lines(pass_name: str, *, preclean: bool = False) -> tuple[str, ...]:
    lines = []
    if preclean:
        lines.append(f"QEMU_EPUB_STAGE pass={pass_name} stage=preclean")
    lines.append(f"QEMU_EPUB_STAGE pass={pass_name} stage=begin")
    lines.append(f"QEMU_EPUB_STAGE pass={pass_name} stage=load")
    lines.extend(
        (
            f"QEMU_EPUB_STAGE pass={pass_name} stage=xhtml index=0",
            f"QEMU_EPUB_STAGE pass={pass_name} stage=xhtml index=1",
            f"QEMU_EPUB_STAGE pass={pass_name} stage=css index=0",
            f"QEMU_EPUB_STAGE pass={pass_name} stage=section index=1 pages={EPUB_REPRESENTATIVE_PAGES}",
            f"QEMU_EPUB_STAGE pass={pass_name} stage=cache index=1",
            f"QEMU_EPUB_STAGE pass={pass_name} stage=frame index=1",
        )
    )
    lines.append(f"QEMU_EPUB_STAGE pass={pass_name} stage=end")
    return tuple(lines)


EPUB_UNCACHED_STAGE_LINES = _epub_stage_lines("uncached", preclean=True)
EPUB_CACHED_STAGE_LINES = _epub_stage_lines("cached")
FULL_PASS_LINES = (
    "QEMU_BOOT seq=0",
    "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
    "QEMU_PDF_RAW_PASS files=15 unchanged=15",
    *SLOW_ATOMIC_LINES,
    SLOW_ATOMIC_SUMMARY,
    (
        "QEMU_PDF_CANCEL_PASS generation=1 steps=40 cancel_slices=2 "
        "max_slice_ms=19 max_slice_us=18500 max_callback_us=18200 "
        "max_callback_kind=write "
        "max_slice_io=4 max_io_request=4096"
    ),
    (
        "QEMU_RUNTIME heap_start=100000 min_free=90000 "
        "min_max_alloc=60000 max_alloc=1000 stack_margin=2000"
    ),
    "QEMU_EXPECT_RESET seq=0",
    "QEMU_BOOT seq=1",
    (
        "QEMU_PDF_RESUME_PASS generation=1 resumed=1 "
        "fresh_steps=120 resumed_steps=40 "
        "retained_truncate=0 retained_remove=0"
    ),
    (
        "QEMU_PDF_TYPOGRAPHY_PASS semantic_six=95EE2813D71DFE2E "
        "semantic_seventy_two=95EE2813D71DFE2E text_six=E1AC47B687F6E82A "
        "text_seventy_two=E1AC47B687F6E82A frame_six=12345678 "
        "frame_seventy_two=12345678 blank=9ABCDEF0 words_six=4 "
        "words_seventy_two=4 pages_six=1 pages_seventy_two=1 "
        "font_id=-1406445118 font_size=2 line_height=100"
    ),
    (
        "QEMU_PDF_NAV_PASS chapters=3 sections=2 words=10 "
        "links=2 labels=2 index=1"
    ),
    "QEMU_PDF_IMAGE_PASS retained=1 frame=12345678 blank=9ABCDEF0",
    (
        "QEMU_PDF_POSITIVE_PASS ocr=DFAE2740CD6F6513 ocr_words=4 "
        "columns=715E72B598FFFFE3 columns_words=8 "
        "table=4BD86B77E1579064 table_words=4 "
        "jpeg=E72D737B2BF7D6CF jpeg_words=2 retained=1 decoded=1 "
        "frame=2468ACE0 blank=13579BDF"
    ),
    (
        "QEMU_PDF_FRAMEBUFFER_GUARD_PASS bytes=48000 checks=7 violations=0 "
        "controls=14 rejected=14"
    ),
    "QEMU_PDF_PROGRESS_MID_PASS words=10 cursor=6 percent=60 resumed=1",
    (
        "QEMU_PDF_PROGRESS_PASS words=10 cursor=10 percent=100 "
        "bookmark=1 clipping=1 resumed=1"
    ),
    CACHE_REOPEN_WITH_METRICS,
    NEGATIVE_WITH_FORCED_OOM,
    *EPUB_UNCACHED_STAGE_LINES,
    (
        "QEMU_EPUB_ORACLE_PASS pass=uncached "
        "xhtml0=46385061C46C2FE4 xhtml1=4060CB229041492D "
        "css=3CC3246E367521F4 cache=5A4EB3E9690894B7 "
        "frame=E99DC1B84A90C006"
    ),
    *EPUB_CACHED_STAGE_LINES,
    (
        "QEMU_EPUB_ORACLE_PASS pass=cached "
        "xhtml0=46385061C46C2FE4 xhtml1=4060CB229041492D "
        "css=3CC3246E367521F4 cache=5A4EB3E9690894B7 "
        "frame=E99DC1B84A90C006"
    ),
    (
        "QEMU_EPUB_PROGRESS_PASS section=1 page=1 pages=3 "
        "book_millionths=643454 bookmark_millionths=333333 "
        "snippet=qemu-epub-smoke"
    ),
    "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
    "QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45",
    "QEMU_INPUT_PASS button=DOWN press=1 release=1",
    "QEMU_POWER_PASS idle_ms=3000 saving=1",
    (
        "QEMU_RUNTIME heap_start=100000 min_free=90000 "
        "min_max_alloc=60000 max_alloc=1000 stack_margin=2000"
    ),
    "QEMU_PDF_TRACER_PASS",
    "QEMU_TRACER_PASS",
    FULL_EXPECTED_MARKER,
)


class QemuRunnerTest(unittest.TestCase):
    @staticmethod
    def _guard_in_slow_phase(runner):
        guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
        for line in (
            "QEMU_BOOT seq=0",
            "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
            "QEMU_PDF_RAW_PASS files=15 unchanged=15",
        ):
            error, terminal = guard.inspect(line)
            if error is not None or terminal:
                raise AssertionError(error or "unexpected terminal marker")
        return guard

    @staticmethod
    def _slow_atom(
        *,
        index: int = 0,
        slice_index: int = 0,
        calls: int = 1,
        kind: str = "write",
        mode: str = "none",
        recursive: int = 0,
        request: int = 1,
        total_us: int = 8001,
        callback_us: int = 8000,
        nonio_us: int = 1,
    ) -> str:
        return (
            f"QEMU_PDF_SLOW_ATOMIC index={index} slice={slice_index} "
            f"calls={calls} kind={kind} mode={mode} recursive={recursive} "
            f"request={request} total_us={total_us} "
            f"callback_us={callback_us} nonio_us={nonio_us}"
        )

    def test_qemu_slow_atomic_boundaries_are_exact(self) -> None:
        runner = self._load_runner_module()
        accepted = (
            self._slow_atom(
                request=1024,
                total_us=30500,
                callback_us=30000,
                nonio_us=500,
            ),
            self._slow_atom(
                kind="rename",
                request=0,
                total_us=24500,
                callback_us=24000,
                nonio_us=500,
            ),
            self._slow_atom(
                kind="open",
                mode="read",
                request=0,
                total_us=16500,
                callback_us=16000,
                nonio_us=500,
            ),
            self._slow_atom(
                kind="open",
                mode="readwrite",
                request=0,
                total_us=36500,
                callback_us=36000,
                nonio_us=500,
            ),
            self._slow_atom(
                calls=4,
                kind="multiple",
                mode="read",
                request=1024,
                total_us=36500,
                callback_us=36000,
                nonio_us=500,
            ),
            self._slow_atom(
                calls=5,
                kind="multiple",
                mode="readwrite",
                request=1024,
                total_us=16500,
                callback_us=16000,
                nonio_us=500,
            ),
        )
        for line in accepted:
            with self.subTest(accepted=line):
                error, terminal = self._guard_in_slow_phase(runner).inspect(
                    line
                )
                self.assertIsNone(error)
                self.assertFalse(terminal)

        rejected = (
            self._slow_atom(request=1025),
            self._slow_atom(total_us=30501, callback_us=30001, nonio_us=500),
            self._slow_atom(
                kind="rename",
                request=0,
                total_us=24501,
                callback_us=24001,
                nonio_us=500,
            ),
            self._slow_atom(
                kind="open",
                mode="read",
                request=0,
                total_us=16501,
                callback_us=16001,
                nonio_us=500,
            ),
            self._slow_atom(total_us=8502, callback_us=8001, nonio_us=501),
            self._slow_atom(total_us=8001, callback_us=8002, nonio_us=0),
            self._slow_atom(calls=2),
            self._slow_atom(recursive=1),
            self._slow_atom(kind="multiple"),
            self._slow_atom(
                kind="open",
                mode="readwrite",
                request=0,
                total_us=36501,
                callback_us=36001,
                nonio_us=500,
            ),
            self._slow_atom(kind="read"),
        )
        for line in rejected:
            with self.subTest(rejected=line):
                error, terminal = self._guard_in_slow_phase(runner).inspect(
                    line
                )
                self.assertEqual(error, "invalid QEMU slow atomic I/O marker")
                self.assertFalse(terminal)

    def test_qemu_slow_atomic_order_and_summary_reconcile(self) -> None:
        runner = self._load_runner_module()
        guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
        prefix = (
            "QEMU_BOOT seq=0",
            "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
            "QEMU_PDF_RAW_PASS files=15 unchanged=15",
        )
        for line in prefix:
            error, terminal = guard.inspect(line)
            self.assertIsNone(error)
            self.assertFalse(terminal)
        for line in SLOW_ATOMIC_LINES:
            error, terminal = guard.inspect(line)
            self.assertIsNone(error)
            self.assertFalse(terminal)
        error, terminal = guard.inspect(SLOW_ATOMIC_SUMMARY)
        self.assertIsNone(error)
        self.assertFalse(terminal)

        cases = {
            "duplicate index": (
                SLOW_ATOMIC_LINES[0],
                SLOW_ATOMIC_LINES[0],
            ),
            "out of order slice": (
                SLOW_ATOMIC_LINES[1],
            ),
            "repeated slice with valid next index": (
                SLOW_ATOMIC_LINES[0],
                self._slow_atom(index=1, slice_index=0),
            ),
            "summary count mismatch": (
                *SLOW_ATOMIC_LINES,
                SLOW_ATOMIC_SUMMARY.replace("total=2", "total=3"),
            ),
            "summary aggregate mismatch": (
                *SLOW_ATOMIC_LINES,
                SLOW_ATOMIC_SUMMARY.replace(
                    "callback_us=26800", "callback_us=26801"
                ),
            ),
        }
        for name, lines in cases.items():
            with self.subTest(name=name):
                candidate = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed = None
                for line in (*prefix, *lines):
                    observed, _ = candidate.inspect(line)
                    if observed is not None:
                        break
                self.assertIsNotNone(observed)
                self.assertIn("slow atomic", observed or "")

    def test_qemu_slow_atomic_records_are_confined_to_boot_zero_raw_phase(
        self,
    ) -> None:
        runner = self._load_runner_module()
        raw = "QEMU_PDF_RAW_PASS files=15 unchanged=15"
        storage = "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26"
        cases = {
            "before boot": (self._slow_atom(),),
            "before raw": ("QEMU_BOOT seq=0", storage, self._slow_atom()),
            "after summary": (
                "QEMU_BOOT seq=0",
                storage,
                raw,
                *SLOW_ATOMIC_LINES,
                SLOW_ATOMIC_SUMMARY,
                self._slow_atom(index=2, slice_index=2),
            ),
        }
        for expected_marker in (
            runner.FULL_EXPECTED_MARKER,
            runner.TRACER_EXPECTED_MARKER,
        ):
            for name, lines in cases.items():
                with self.subTest(marker=expected_marker, placement=name):
                    guard = runner.OutputGuard(expected_marker)
                    error = None
                    for line in lines:
                        error, terminal = guard.inspect(line)
                        self.assertFalse(terminal)
                        if error is not None:
                            break
                    self.assertEqual(
                        error,
                        "QEMU slow atomic marker outside boot-0 RAW phase",
                    )

    def test_qemu_slow_atomic_cancel_maxima_reconcile_exactly(self) -> None:
        runner = self._load_runner_module()

        impossible = {
            "zero_slice_io": ("max_slice_io=4", "max_slice_io=0"),
            "request_below_record": (
                "max_io_request=4096",
                "max_io_request=1023",
            ),
            "callback_below_record": (
                "max_callback_us=18200",
                "max_callback_us=18199",
            ),
            "callback_above_record": (
                "max_callback_us=18200",
                "max_callback_us=18201",
            ),
            "wrong_callback_kind": (
                "max_callback_kind=write",
                "max_callback_kind=open",
            ),
        }
        for name, (old, new) in impossible.items():
            with self.subTest(impossible=name):
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                error = None
                for line in tuple(
                    candidate.replace(old, new)
                    if candidate.startswith("QEMU_PDF_CANCEL_PASS")
                    else candidate
                    for candidate in FULL_PASS_LINES
                ):
                    error, terminal = guard.inspect(line)
                    if error is not None or terminal:
                        break
                self.assertEqual(error, "PDF cancellation limits exceeded")
                self.assertFalse(terminal)

        tied_records = (
            self._slow_atom(
                kind="rename",
                request=0,
                total_us=18001,
                callback_us=18000,
            ),
            self._slow_atom(
                index=1,
                slice_index=1,
                request=1,
                total_us=18001,
                callback_us=18000,
            ),
        )
        tied_summary = (
            "QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=1 slices=2 total=2 "
            "write=1 rename=1 open_read=0 request_bytes=1 callback_us=36000 "
            "nonio_us=2 max_total_us=18001 max_callback_us=18000"
        )
        tied_cancel = (
            "QEMU_PDF_CANCEL_PASS generation=1 steps=40 cancel_slices=2 "
            "max_slice_ms=19 max_slice_us=18001 max_callback_us=18000 "
            "max_callback_kind=rename max_slice_io=1 max_io_request=1"
        )
        prefix = (
            "QEMU_BOOT seq=0",
            "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
            "QEMU_PDF_RAW_PASS files=15 unchanged=15",
        )
        for kind, accepted in (("rename", True), ("write", False)):
            with self.subTest(earliest_tied_kind=kind):
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                error = None
                for line in (
                    *prefix,
                    *tied_records,
                    tied_summary,
                    tied_cancel.replace("kind=rename", f"kind={kind}"),
                ):
                    error, terminal = guard.inspect(line)
                    if error is not None:
                        break
                self.assertEqual(error is None, accepted)

    def test_ordinary_cancel_callback_kind_is_a_real_operation(self) -> None:
        runner = self._load_runner_module()
        zero_summary = (
            "QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=1 slices=2 total=0 "
            "write=0 rename=0 open_read=0 request_bytes=0 callback_us=0 "
            "nonio_us=0 max_total_us=0 max_callback_us=0"
        )
        standard_cancel = (
            "QEMU_PDF_CANCEL_PASS generation=1 steps=40 cancel_slices=2 "
            "max_slice_ms=8 max_slice_us=8000 max_callback_us=7000 "
            "max_callback_kind=write max_slice_io=4 max_io_request=4096"
        )
        ordinary = tuple(
            zero_summary
            if line == SLOW_ATOMIC_SUMMARY
            else standard_cancel
            if line.startswith("QEMU_PDF_CANCEL_PASS")
            else line
            for line in FULL_PASS_LINES
            if line not in SLOW_ATOMIC_LINES
        )
        real_operations = (
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
        for kind in (*real_operations, "none", "bogus"):
            with self.subTest(kind=kind):
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                error = None
                for line in ordinary:
                    error, terminal = guard.inspect(
                        line.replace(
                            "max_callback_kind=write",
                            f"max_callback_kind={kind}",
                        )
                    )
                    if error is not None or terminal:
                        break
                self.assertEqual(error is None, kind in real_operations)

    def test_qemu_slow_atomic_aggregate_caps_are_exact(self) -> None:
        runner = self._load_runner_module()

        def inspect_atoms(atoms: list[dict[str, object]]) -> str | None:
            guard = self._guard_in_slow_phase(runner)
            error = None
            for index, atom in enumerate(atoms):
                error, terminal = guard.inspect(
                    self._slow_atom(index=index, slice_index=index, **atom)
                )
                self.assertFalse(terminal)
                if error is not None:
                    break
            return error

        write = {"request": 1, "callback_us": 8000, "total_us": 8001}
        rename = {
            "kind": "rename",
            "request": 0,
            "callback_us": 8000,
            "total_us": 8001,
        }
        open_read = {
            "kind": "open",
            "mode": "read",
            "request": 0,
            "callback_us": 8000,
            "total_us": 8001,
        }
        exact_cases = {
            "write_count": [write] * 22,
            "rename_count": [rename] * 2,
            "open_read_count": [open_read] * 8,
            "request_bytes": [
                {**write, "request": 1024},
                {**write, "request": 1024},
                {**write, "request": 1024},
            ],
            "callback_us": [
                {
                    **write,
                    "callback_us": 30000,
                    "total_us": 30001,
                }
            ]
            * 18
            + [
                {
                    **write,
                    "callback_us": 10000,
                    "total_us": 10001,
                }
            ],
            "nonio_us": [
                {
                    **write,
                    "callback_us": 8000,
                    "total_us": 8500,
                    "nonio_us": 500,
                }
            ]
            * 10,
            "total_count": [write] * 22 + [rename] * 2 + [open_read] * 8,
        }
        for name, atoms in exact_cases.items():
            with self.subTest(exact=name):
                self.assertIsNone(inspect_atoms(atoms))

        one_over_cases = {
            "write_count": [write] * 23,
            "rename_count": [rename] * 3,
            "open_read_count": [open_read] * 9,
            "request_bytes": exact_cases["request_bytes"] + [write],
            "callback_us": [
                {
                    **write,
                    "callback_us": 30000,
                    "total_us": 30001,
                }
            ]
            * 18
            + [
                {
                    **write,
                    "callback_us": 10001,
                    "total_us": 10002,
                }
            ],
            "nonio_us": exact_cases["nonio_us"]
            + [
                {
                    **write,
                    "callback_us": 8000,
                    "total_us": 8001,
                    "nonio_us": 1,
                }
            ],
            "total_count": exact_cases["total_count"] + [write],
        }
        for name, atoms in one_over_cases.items():
            with self.subTest(one_over=name):
                self.assertEqual(
                    inspect_atoms(atoms),
                    "QEMU slow atomic aggregate limit exceeded",
                )

    def test_epub_timeout_reports_last_validated_stage(self) -> None:
        representative_section = (
            "QEMU_EPUB_STAGE pass=uncached stage=section index=1 pages=3"
        )
        lines = EPUB_UNCACHED_STAGE_LINES[
            : EPUB_UNCACHED_STAGE_LINES.index(representative_section) + 1
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(
                Path(temporary_directory), lines
            )
            completed = self._run(
                paths,
                "epub_stage_timeout",
                timeout=0.1,
                expected_marker=FULL_EXPECTED_MARKER,
            )

        self.assertEqual(completed.returncode, 1)
        self.assertIn("QEMU timed out after 0.1 seconds", completed.stderr)
        self.assertIn(
            "last_epub_stage=pass=uncached stage=section index=1 pages=3",
            completed.stderr,
        )

    def test_progress_failure_diagnostics_parse_page_ranges_and_selected_position(self) -> None:
        runner = self._load_runner_module()
        guard = runner.OutputGuard("QEMU_TIMING_DIAGNOSTIC_COMPLETE")

        page_error, terminal = guard.inspect(
            "QEMU_PDF_PROGRESS_PAGE section=1 page=0 page_count=1 found=1 "
            "valid=1 first=6 last=9 cursor=10"
        )
        self.assertIsNone(page_error)
        self.assertFalse(terminal)
        self.assertEqual(
            guard.progress_page_ranges,
            [(1, 0, 1, 1, 1, 6, 9, 10)],
        )

        summary_error, terminal = guard.inspect(
            "QEMU_PDF_PROGRESS_DIAGNOSTIC selected_range=1 section=1 page=0 "
            "word_start=6 word_cursor=10 total_words=10 saved=1 progress_ok=1 "
            "millionths=1000000 page_count=1"
        )
        self.assertIsNone(summary_error)
        self.assertFalse(terminal)
        self.assertEqual(
            guard.progress_diagnostic,
            (1, 1, 0, 6, 10, 10, 1, 1, 1000000, 1),
        )

        malformed, terminal = guard.inspect(
            "QEMU_PDF_PROGRESS_DIAGNOSTIC selected_range=1"
        )
        self.assertEqual(malformed, "malformed PDF progress diagnostic marker")
        self.assertFalse(terminal)

    def test_slice_budget_failure_requires_complete_diagnostics(self) -> None:
        runner = self._load_runner_module()
        guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
        line = (
            "QEMU_PDF_CANCEL_FAIL reason=slice_budget elapsed_ms=9 elapsed_us=8621 "
            "io_calls=1 io_kind=open io_mode=readwrite io_recursive=0 io_request=0 "
            "callback_us=8570 nonio_us=51 max_io_request=619 "
            "generation=3 expected_generation=3"
        )

        error, passed = guard.inspect(line)

        self.assertFalse(passed)
        self.assertEqual(
            error,
            "PDF cancellation slice budget exceeded: elapsed_ms=9 elapsed_us=8621 "
            "io_calls=1 io_kind=open io_mode=readwrite io_recursive=0 io_request=0 "
            "callback_us=8570 nonio_us=51 max_io_request=619 "
            "generation=3 expected_generation=3",
        )
        malformed, passed = guard.inspect(
            "QEMU_PDF_CANCEL_FAIL reason=slice_budget"
        )
        self.assertFalse(passed)
        self.assertEqual(
            malformed, "malformed PDF cancellation slice budget marker"
        )

    def test_jpeg_failure_diagnostic_requires_complete_subpredicate_values(self) -> None:
        runner = self._load_runner_module()
        guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
        line = (
            "QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC document=1 layout=1 sections=1 "
            "semantic_stream=1 content_stream=1 words=2 "
            "semantic=E72D737B2BF7D6CF expected=E72D737B2BF7D6CF "
            "semantic_bytes=36 image_tags=0 captions=1"
        )

        error, passed = guard.inspect(line)

        self.assertIsNone(error)
        self.assertFalse(passed)
        self.assertEqual(
            getattr(guard, "jpeg_diagnostic", None),
            (
                1,
                1,
                1,
                1,
                1,
                2,
                "E72D737B2BF7D6CF",
                "E72D737B2BF7D6CF",
                36,
                0,
                1,
            ),
        )

        malformed, passed = guard.inspect(
            "QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC document=1 layout=1 sections=1"
        )
        self.assertEqual(malformed, "malformed PDF JPEG retained diagnostic marker")
        self.assertFalse(passed)

    def test_full_acceptance_stream_remains_tracer_compatible(self) -> None:
        runner = self._load_runner_module()
        guard = runner.OutputGuard(runner.TRACER_EXPECTED_MARKER)

        for line in FULL_PASS_LINES:
            error, passed = guard.inspect(line)
            self.assertIsNone(error, line)
            if line == EXPECTED_MARKER:
                self.assertTrue(passed)
                break
        else:
            self.fail("full acceptance stream did not reach tracer terminal")

    def test_terminal_boot_tracer_reorder_fails_at_terminal_marker(self) -> None:
        runner = self._load_runner_module()
        boot_one = FULL_PASS_LINES.index("QEMU_BOOT seq=1")
        storage = FULL_PASS_LINES.index(
            "QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26",
            boot_one,
        )
        frame = FULL_PASS_LINES.index(
            "QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45",
            boot_one,
        )
        reordered = list(FULL_PASS_LINES)
        reordered[storage], reordered[frame] = (
            reordered[frame],
            reordered[storage],
        )
        guard = runner.OutputGuard(runner.TRACER_EXPECTED_MARKER)

        for line in reordered:
            error, passed = guard.inspect(line)
            if line == EXPECTED_MARKER:
                self.assertFalse(passed)
                self.assertIn("out-of-order tracer marker", error or "")
                break
            self.assertIsNone(error, line)
        else:
            self.fail("reordered stream did not reach tracer terminal")

    def test_pass_marker_terminates_still_running_qemu(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_fake_qemu(Path(temporary_directory))
            started = time.monotonic()
            completed = self._run(paths, "pass", timeout=2.0)
            elapsed = time.monotonic() - started

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stderr, "")
            self.assertIn(EXPECTED_MARKER, completed.stdout)
            self.assertLess(elapsed, 2.0)
            self.assertEqual(
                paths["log"].read_text(encoding="utf-8"),
                "\n".join(
                    (
                        "QEMU_BOOT seq=0",
                        (
                            "QEMU_STORAGE_PASS "
                            "path=/qemu/sentinel.txt bytes=26"
                        ),
                        (
                            "QEMU_FRAME_PASS "
                            "bytes=48000 crc32=0F7C8C45"
                        ),
                        (
                            "QEMU_INPUT_PASS "
                            "button=DOWN press=1 release=1"
                        ),
                        "QEMU_POWER_PASS idle_ms=3000 saving=1",
                        (
                            "QEMU_RUNTIME heap_start=100000 "
                            "min_free=99000 min_max_alloc=60000 "
                            "max_alloc=1000 stack_margin=2000"
                        ),
                        EXPECTED_MARKER,
                        "",
                    )
                ),
            )

    def test_timing_diagnostic_terminal_is_intentionally_non_accepting(self) -> None:
        lines = (
            "QEMU_BOOT seq=0",
            "QEMU_EXPECT_RESET seq=0",
            "QEMU_BOOT seq=1",
            "QEMU_TIMING_DIAGNOSTIC_COMPLETE",
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(Path(temporary_directory), lines)
            completed = self._run(
                paths,
                "timing_diagnostic",
                timeout=2.0,
                expected_marker="QEMU_TIMING_DIAGNOSTIC_COMPLETE",
                timing_diagnostic=True,
            )

        self.assertEqual(completed.returncode, 1)
        self.assertIn("QEMU_TIMING_DIAGNOSTIC_COMPLETE", completed.stdout)
        self.assertIn("intentionally non-accepting", completed.stderr)
        self.assertNotIn("QEMU_TEST_PASS", completed.stdout)

    def test_valid_armed_reset_sequence_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_fake_qemu(Path(temporary_directory))
            completed = self._run(paths, "armed_reset", timeout=2.0)

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stderr, "")
            self.assertIn("QEMU_EXPECT_RESET seq=0", completed.stdout)
            self.assertIn("QEMU_BOOT seq=1", completed.stdout)

    def test_full_acceptance_requires_every_ordered_stage_across_reset(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(
                Path(temporary_directory), FULL_PASS_LINES
            )
            completed = self._run(
                paths,
                "full",
                timeout=2.0,
                expected_marker=FULL_EXPECTED_MARKER,
            )

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stderr, "")
            self.assertIn(FULL_EXPECTED_MARKER, completed.stdout)

    def test_full_acceptance_requires_exact_framebuffer_and_mid_progress_receipts(
        self,
    ) -> None:
        runner = self._load_runner_module()
        cases = {
            "missing framebuffer": tuple(
                line
                for line in FULL_PASS_LINES
                if not line.startswith("QEMU_PDF_FRAMEBUFFER_GUARD_PASS")
            ),
            "missing mid progress": tuple(
                line
                for line in FULL_PASS_LINES
                if not line.startswith("QEMU_PDF_PROGRESS_MID_PASS")
            ),
            "wrong framebuffer bytes": tuple(
                line.replace("bytes=48000", "bytes=47999")
                if line.startswith("QEMU_PDF_FRAMEBUFFER_GUARD_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "framebuffer violation": tuple(
                line.replace("violations=0", "violations=1")
                if line.startswith("QEMU_PDF_FRAMEBUFFER_GUARD_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "missing framebuffer control": tuple(
                line.replace("controls=14 rejected=14", "controls=13 rejected=13")
                if line.startswith("QEMU_PDF_FRAMEBUFFER_GUARD_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "accepted framebuffer mutation": tuple(
                line.replace("controls=14 rejected=14", "controls=14 rejected=13")
                if line.startswith("QEMU_PDF_FRAMEBUFFER_GUARD_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "wrong mid cursor": tuple(
                line.replace("cursor=6 percent=60", "cursor=5 percent=50")
                if line.startswith("QEMU_PDF_PROGRESS_MID_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
        }
        for name, lines in cases.items():
            with self.subTest(name=name):
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                error = None
                terminal = False
                for line in lines:
                    error, terminal = guard.inspect(line)
                    if error is not None or terminal:
                        break
                self.assertIsNotNone(error)
                self.assertFalse(terminal)

    def test_full_acceptance_rejects_missing_reordered_or_invalid_stages(
        self,
    ) -> None:
        cases = {
            "missing": tuple(
                line
                for line in FULL_PASS_LINES
                if not line.startswith("QEMU_PDF_IMAGE_PASS")
            ),
            "reordered": (
                FULL_PASS_LINES[:7]
                + (FULL_PASS_LINES[8], FULL_PASS_LINES[7])
                + FULL_PASS_LINES[9:]
            ),
            "no_reset": tuple(
                line
                for line in FULL_PASS_LINES
                if not line.startswith("QEMU_EXPECT_RESET")
                and line != "QEMU_BOOT seq=1"
            ),
            "full_rebuild": tuple(
                line.replace("fresh_steps=120 resumed_steps=40", "fresh_steps=120 resumed_steps=120")
                for line in FULL_PASS_LINES
            ),
            "retained_truncate": tuple(
                line.replace("retained_truncate=0", "retained_truncate=1")
                for line in FULL_PASS_LINES
            ),
            "epub_drift": tuple(
                line.replace(
                    "xhtml0=46385061C46C2FE4",
                    "xhtml0=56385061C46C2FE4",
                )
                if line.startswith("QEMU_EPUB_ORACLE_PASS pass=cached")
                else line
                for line in FULL_PASS_LINES
            ),
        }
        for mode, lines in cases.items():
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        mode,
                        timeout=0.5,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )

                    self.assertEqual(completed.returncode, 1)
                    self.assertNotIn(FULL_EXPECTED_MARKER, completed.stdout)

    def test_epub_oracle_rejects_every_high_half_mismatch(self) -> None:
        runner = self._load_runner_module()
        expected = dict(
            zip(
                ("xhtml0", "xhtml1", "css", "cache", "frame"),
                runner.EXPECTED_EPUB_ORACLE,
            )
        )
        for pass_name in ("uncached", "cached"):
            for field, value in expected.items():
                with self.subTest(pass_name=pass_name, field=field):
                    changed_high = ("0" if value[0] != "0" else "1") + value[1:]
                    lines = tuple(
                        line.replace(f"{field}={value}", f"{field}={changed_high}")
                        if line.startswith(
                            f"QEMU_EPUB_ORACLE_PASS pass={pass_name}"
                        )
                        else line
                        for line in FULL_PASS_LINES
                    )
                    guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                    observed_error = None
                    terminal = False
                    for line in lines:
                        observed_error, terminal = guard.inspect(line)
                        if observed_error is not None:
                            break
                    self.assertFalse(terminal)
                    self.assertEqual(
                        observed_error,
                        f"{pass_name} EPUB oracle differs from fixture",
                    )

    def test_full_acceptance_binds_stages_to_their_boot(self) -> None:
        reset = FULL_PASS_LINES.index("QEMU_EXPECT_RESET seq=0")
        boot_one = FULL_PASS_LINES.index("QEMU_BOOT seq=1")
        terminal = FULL_PASS_LINES.index(FULL_EXPECTED_MARKER)
        cases = {
            "boot_one_stages_before_reset": (
                (
                    FULL_PASS_LINES[:reset]
                    + FULL_PASS_LINES[boot_one + 1 : terminal]
                    + FULL_PASS_LINES[reset : boot_one + 1]
                    + (FULL_EXPECTED_MARKER,)
                ),
                "wrong boot",
            ),
            "boot_zero_stages_after_reset": (
                (
                    FULL_PASS_LINES[:1]
                    + FULL_PASS_LINES[reset : boot_one + 1]
                    + FULL_PASS_LINES[1:reset]
                    + FULL_PASS_LINES[boot_one + 1 :]
                ),
                "boot 0 runtime",
            ),
        }
        for mode, (lines, error) in cases.items():
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        mode,
                        timeout=0.5,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )

                    self.assertEqual(completed.returncode, 1)
                    self.assertIn(error, completed.stderr)

    def test_full_acceptance_requires_boot_zero_runtime_and_bounded_allocation(
        self,
    ) -> None:
        reset = FULL_PASS_LINES.index("QEMU_EXPECT_RESET seq=0")
        missing_boot_zero_runtime = (
            FULL_PASS_LINES[: reset - 1] + FULL_PASS_LINES[reset:]
        )
        oversized = tuple(
            line.replace("max_alloc=1000", "max_alloc=32769")
            if line.startswith("QEMU_RUNTIME ")
            else line
            for line in FULL_PASS_LINES
        )
        for mode, lines, error in (
            (
                "missing_boot_zero_runtime",
                missing_boot_zero_runtime,
                "boot 0 runtime",
            ),
            ("oversized_allocation", oversized, "PDF allocation"),
        ):
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        mode,
                        timeout=0.5,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )

                    self.assertEqual(completed.returncode, 1)
                    self.assertIn(error, completed.stderr)

    def test_cancellation_limits_are_parsed_as_integers_at_boundaries(
        self,
    ) -> None:
        for slices in (2, 10, 256):
            with self.subTest(cancel_slices=slices):
                lines = tuple(
                    line.replace("generation=1 slices=2 total=2", f"generation=1 slices={slices} total=2")
                    .replace(
                        "steps=40 cancel_slices=2",
                        f"steps={max(40, slices)} cancel_slices={slices}",
                    )
                    for line in FULL_PASS_LINES
                )
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        f"slices_{slices}",
                        timeout=2.0,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )
                    self.assertEqual(completed.returncode, 0)

        for request, accepted in ((8192, True), (8193, False)):
            with self.subTest(max_io_request=request):
                lines = tuple(
                    line.replace(
                        "max_io_request=4096",
                        f"max_io_request={request}",
                    )
                    for line in FULL_PASS_LINES
                )
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        f"request_{request}",
                        timeout=2.0 if accepted else 0.5,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )
                    self.assertEqual(completed.returncode, 0 if accepted else 1)
                    if not accepted:
                        self.assertIn("cancellation limits", completed.stderr)

        zero_summary = (
            "QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=1 slices=2 total=0 "
            "write=0 rename=0 open_read=0 request_bytes=0 callback_us=0 "
            "nonio_us=0 max_total_us=0 max_callback_us=0"
        )
        standard_cancel = (
            "QEMU_PDF_CANCEL_PASS generation=1 steps=40 cancel_slices=2 "
            "max_slice_ms=8 max_slice_us=8000 max_callback_us=7000 "
            "max_callback_kind=write max_slice_io=4 max_io_request=4096"
        )
        ordinary_timing_lines = tuple(
            zero_summary
            if line == SLOW_ATOMIC_SUMMARY
            else standard_cancel
            if line.startswith("QEMU_PDF_CANCEL_PASS")
            else line
            for line in FULL_PASS_LINES
            if line not in SLOW_ATOMIC_LINES
        )
        timing_cases = {
            "standard_slice": ("max_slice_us=8000", "max_slice_us=7999", True),
            "single_microsecond_over_limit": ("max_slice_us=8000", "max_slice_us=8001", False),
            "nine_ms_is_never_standard": ("max_slice_ms=8", "max_slice_ms=9", False),
            "callback_cannot_exceed_slice": ("max_callback_us=7000", "max_callback_us=8001", False),
        }
        for mode, (old, new, accepted) in timing_cases.items():
            with self.subTest(timing=mode):
                lines = tuple(line.replace(old, new) for line in ordinary_timing_lines)
                guard = self._load_runner_module().OutputGuard(
                    FULL_EXPECTED_MARKER
                )
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertEqual(observed_error is None, accepted)
                if not accepted:
                    self.assertFalse(terminal)
                    self.assertIn("cancellation limits", observed_error or "")

        one_over_cases = {
            "cancel_slices": ("cancel_slices=2", "cancel_slices=257"),
            "max_slice_io": ("max_slice_io=4", "max_slice_io=33"),
        }
        for field, (old, new) in one_over_cases.items():
            with self.subTest(one_over=field):
                lines = tuple(line.replace(old, new) for line in FULL_PASS_LINES)
                guard = self._load_runner_module().OutputGuard(
                    FULL_EXPECTED_MARKER
                )
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIn("cancellation limits", observed_error or "")

    def test_cached_turn_metrics_and_positive_controls(self) -> None:
        valid = tuple(
            CACHE_REOPEN_WITH_METRICS
            if line.startswith("QEMU_PDF_CACHE_REOPEN_PASS")
            else line
            for line in FULL_PASS_LINES
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(
                Path(temporary_directory), valid
            )
            completed = self._run(
                paths,
                "cached_metrics",
                timeout=2.0,
                expected_marker=FULL_EXPECTED_MARKER,
            )
            self.assertEqual(completed.returncode, 0)

        cases = {
            "extraction": (
                "extraction=0",
                "extraction=1",
                "cached-turn parser/extraction counters changed",
            ),
            "parser": (
                "parser=0",
                "parser=1",
                "cached-turn parser/extraction counters changed",
            ),
            "zero_source_opens": (
                "source_opens=1",
                "source_opens=0",
                "cached identity source open count",
            ),
            "zero_source_reads": (
                "source_reads=2",
                "source_reads=0",
                "cached identity read count",
            ),
            "excess_source_reads": (
                "source_reads=2",
                "source_reads=3",
                "cached identity read count",
            ),
            "zero_identity_request": (
                "source_max_request=4096",
                "source_max_request=0",
                "cached identity request",
            ),
            "oversized_identity_request": (
                "source_max_request=4096",
                "source_max_request=4097",
                "cached identity request",
            ),
            "zero_heap": (
                "heap_before=90000",
                "heap_before=0",
                "cached-turn resource snapshot invalid",
            ),
            "zero_largest": (
                "largest_before=60000",
                "largest_before=0",
                "cached-turn resource snapshot invalid",
            ),
            "zero_stack": (
                "stack_before=2000",
                "stack_before=0",
                "cached-turn resource snapshot invalid",
            ),
            "zero_heap_after": (
                "heap_after=90000",
                "heap_after=0",
                "cached-turn resource snapshot invalid",
            ),
            "zero_largest_after": (
                "largest_after=60000",
                "largest_after=0",
                "cached-turn resource snapshot invalid",
            ),
            "zero_stack_after": (
                "stack_after=2000",
                "stack_after=0",
                "cached-turn resource snapshot invalid",
            ),
            "heap": (
                "heap_after=90000",
                "heap_after=89999",
                "cached-turn memory eroded",
            ),
            "largest": (
                "largest_after=60000",
                "largest_after=59999",
                "cached-turn memory eroded",
            ),
            "stack": (
                "stack_after=2000",
                "stack_after=1999",
                "cached-turn memory eroded",
            ),
        }
        for mode, (old, new, error) in cases.items():
            with self.subTest(mode=mode):
                lines = tuple(line.replace(old, new) for line in valid)
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_full_fake_qemu(
                        Path(temporary_directory), lines
                    )
                    completed = self._run(
                        paths,
                        mode,
                        timeout=0.5,
                        expected_marker=FULL_EXPECTED_MARKER,
                    )
                    self.assertEqual(completed.returncode, 1)
                    self.assertIn(error, completed.stderr)

    def test_full_numeric_witnesses_reject_impossible_values(self) -> None:
        runner = self._load_runner_module()
        cases = {
            "zero_resume_work": (
                "resumed_steps=40",
                "resumed_steps=0",
                "checkpoint continuation",
            ),
            "zero_runtime_heap": (
                "heap_start=100000 min_free=90000",
                "heap_start=0 min_free=0",
                "resource floor",
            ),
            "zero_runtime_free": (
                "min_free=90000",
                "min_free=0",
                "resource floor",
            ),
            "cursor_past_total": (
                "cursor=10 percent=100",
                "cursor=11 percent=100",
                "word progress",
            ),
            "incorrect_percentage": (
                "cursor=10 percent=100",
                "cursor=10 percent=99",
                "word progress",
            ),
        }
        for mode, (old, new, expected_error) in cases.items():
            with self.subTest(mode=mode):
                lines = tuple(line.replace(old, new) for line in FULL_PASS_LINES)
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIn(expected_error, observed_error or "")

    def test_full_acceptance_rejects_fabricated_content_witnesses(self) -> None:
        runner = self._load_runner_module()
        cases = {
            "zero_typography": tuple(
                (
                    "QEMU_PDF_TYPOGRAPHY_PASS "
                    "semantic_six=0000000000000000 "
                    "semantic_seventy_two=0000000000000000 "
                    "text_six=0000000000000000 "
                    "text_seventy_two=0000000000000000 "
                    "frame_six=00000000 frame_seventy_two=00000000 "
                    "blank=9ABCDEF0 words_six=4 words_seventy_two=4 "
                    "pages_six=1 pages_seventy_two=1 "
                    "font_id=-1406445118 font_size=2 line_height=100"
                )
                if line.startswith("QEMU_PDF_TYPOGRAPHY_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "equal_image_frames": tuple(
                (
                    "QEMU_PDF_IMAGE_PASS retained=1 "
                    "frame=12345678 blank=12345678"
                )
                if line.startswith("QEMU_PDF_IMAGE_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "excess_images": tuple(
                line.replace("retained=1", "retained=2")
                if line.startswith("QEMU_PDF_IMAGE_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "alternate_progress": tuple(
                line.replace("cursor=10 percent=100", "cursor=9 percent=100")
                if line.startswith("QEMU_PDF_PROGRESS_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
            "matching_zero_epub_oracles": tuple(
                (
                    f"QEMU_EPUB_ORACLE_PASS pass="
                    f"{'uncached' if 'pass=uncached' in line else 'cached'} "
                    "xhtml0=0000000000000000 xhtml1=0000000000000000 "
                    "css=0000000000000000 cache=0000000000000000 "
                    "frame=0000000000000000"
                )
                if line.startswith("QEMU_EPUB_ORACLE_PASS")
                else line
                for line in FULL_PASS_LINES
            ),
        }
        for mode, lines in cases.items():
            with self.subTest(mode=mode):
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIsNotNone(observed_error)

    def test_positive_pdf_corpus_marker_is_required_and_mutation_controlled(
        self,
    ) -> None:
        runner = self._load_runner_module()
        cases = {
            "OCR text layer": (
                "ocr=DFAE2740CD6F6513",
                "ocr=0000000000000000",
                "positive corpus OCR witness",
            ),
            "column reading order": (
                "columns=715E72B598FFFFE3",
                "columns=0000000000000000",
                "positive corpus column witness",
            ),
            "row-major table order": (
                "table=4BD86B77E1579064",
                "table=0000000000000000",
                "positive corpus table witness",
            ),
            "JPEG retained": (
                "jpeg_words=2 retained=1 decoded=1",
                "jpeg_words=2 retained=0 decoded=1",
                "positive corpus JPEG witness",
            ),
            "JPEG decoded frame": (
                "retained=1 decoded=1 frame=2468ACE0 blank=13579BDF",
                "retained=1 decoded=0 frame=13579BDF blank=13579BDF",
                "positive corpus JPEG witness",
            ),
        }
        for name, (old, new, expected_error) in cases.items():
            with self.subTest(witness=name):
                lines = tuple(line.replace(old, new) for line in FULL_PASS_LINES)
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIn(expected_error, observed_error or "")

        missing = tuple(
            line
            for line in FULL_PASS_LINES
            if not line.startswith("QEMU_PDF_POSITIVE_PASS")
        )
        guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
        observed_error = None
        terminal = False
        for line in missing:
            observed_error, terminal = guard.inspect(line)
            if observed_error is not None:
                break
        self.assertFalse(terminal)
        self.assertIn("out-of-order acceptance marker", observed_error or "")

    def test_typography_frames_are_live_equal_and_nonblank(self) -> None:
        runner = self._load_runner_module()
        cases = {
            "zero": (
                "frame_six=12345678 frame_seventy_two=12345678",
                "frame_six=00000000 frame_seventy_two=00000000",
            ),
            "blank": (
                "frame_six=12345678 frame_seventy_two=12345678",
                "frame_six=9ABCDEF0 frame_seventy_two=9ABCDEF0",
            ),
            "mismatch": (
                "frame_six=12345678 frame_seventy_two=12345678",
                "frame_six=12345678 frame_seventy_two=87654321",
            ),
        }
        for mode, (old, new) in cases.items():
            with self.subTest(mode=mode):
                lines = tuple(line.replace(old, new) for line in FULL_PASS_LINES)
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIn("typography frame", observed_error or "")

    def test_full_acceptance_enforces_generation_and_type_bounds(self) -> None:
        runner = self._load_runner_module()
        cases = {
            "generation_mismatch": (
                "QEMU_PDF_RESUME_PASS generation=1",
                "QEMU_PDF_RESUME_PASS generation=2",
            ),
            "generation_uint32_overflow": (
                "QEMU_PDF_CANCEL_PASS generation=1",
                "QEMU_PDF_CANCEL_PASS generation=4294967296",
            ),
            "cancel_work_one_over": (
                "steps=40 cancel_slices=2",
                "steps=100257 cancel_slices=2",
            ),
            "fresh_work_one_over": (
                "fresh_steps=120 resumed_steps=40",
                "fresh_steps=100001 resumed_steps=40",
            ),
            "heap_uint32_overflow": (
                "heap_start=100000",
                "heap_start=4294967296",
            ),
            "allocation_exceeds_free": (
                "min_free=90000 min_max_alloc=60000 max_alloc=1000",
                "min_free=1000 min_max_alloc=900 max_alloc=1001",
            ),
            "largest_exceeds_free": (
                "min_free=90000 min_max_alloc=60000",
                "min_free=50000 min_max_alloc=50001",
            ),
            "cached_heap_uint32_overflow": (
                "heap_before=90000 heap_after=90000",
                "heap_before=4294967296 heap_after=4294967296",
            ),
            "font_id_int32_overflow": (
                "font_id=-1406445118",
                "font_id=2147483648",
            ),
        }
        for mode, (old, new) in cases.items():
            with self.subTest(mode=mode):
                lines = tuple(line.replace(old, new) for line in FULL_PASS_LINES)
                guard = runner.OutputGuard(runner.FULL_EXPECTED_MARKER)
                observed_error = None
                terminal = False
                for line in lines:
                    observed_error, terminal = guard.inspect(line)
                    if observed_error is not None:
                        break
                self.assertFalse(terminal)
                self.assertIsNotNone(observed_error)

    def test_forced_oom_is_required_in_the_negative_stage(self) -> None:
        valid = tuple(
            NEGATIVE_WITH_FORCED_OOM
            if line.startswith("QEMU_PDF_NEGATIVE_PASS")
            else line
            for line in FULL_PASS_LINES
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(
                Path(temporary_directory), valid
            )
            completed = self._run(
                paths,
                "forced_oom",
                timeout=2.0,
                expected_marker=FULL_EXPECTED_MARKER,
            )
            self.assertEqual(completed.returncode, 0)

        unsafe = tuple(
            line.replace("completed_cache=0", "completed_cache=1")
            for line in valid
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_full_fake_qemu(
                Path(temporary_directory), unsafe
            )
            completed = self._run(
                paths,
                "forced_oom_cache",
                timeout=0.5,
                expected_marker=FULL_EXPECTED_MARKER,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("forced OOM", completed.stderr)

    def test_real_command_uses_private_image_copies_and_no_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = self._create_fake_qemu(directory)
            flash = directory / "qemu_flash.bin"
            efuse = directory / "qemu_efuse.bin"
            elf = directory / "firmware.elf"
            flash.write_bytes(b"flash")
            efuse.write_bytes(b"efuse")
            elf.write_bytes(b"elf")

            def entry(path: Path) -> dict[str, object]:
                return {
                    "path": str(path.resolve()),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size": path.stat().st_size,
                }
            install = directory / "install.json"
            install.write_text(
                json.dumps(
                    {
                        "version": "test",
                        "executable": str(paths["qemu"].resolve()),
                    }
                ),
                encoding="utf-8",
            )
            manifest = directory / "qemu_manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "images": {
                            "flash": entry(flash),
                            "efuse": entry(efuse),
                        },
                        "artifacts": {"elf": entry(elf)},
                    }
                ),
                encoding="utf-8",
            )
            arguments_log = directory / "arguments.json"
            environment = os.environ.copy()
            environment["FAKE_QEMU_MODE"] = "real_command"
            environment["FAKE_QEMU_ARGS"] = str(arguments_log)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--install",
                    str(install),
                    "--manifest",
                    str(manifest),
                    "--expect",
                    EXPECTED_MARKER,
                    "--timeout",
                    "2",
                    "--log",
                    str(paths["log"]),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
                timeout=4.0,
            )

            self.assertEqual(completed.returncode, 0)
            arguments = json.loads(arguments_log.read_text(encoding="utf-8"))
            self.assertEqual(arguments[0:2], ["-M", "esp32c3"])
            self.assertIn("-icount", arguments)
            self.assertEqual(
                arguments[arguments.index("-icount") + 1],
                "shift=3,sleep=off",
            )
            self.assertIn("-nic", arguments)
            self.assertIn("none", arguments)
            self.assertIn("-nographic", arguments)
            self.assertEqual(arguments[-2:], ["-serial", "mon:stdio"])
            self.assertFalse(
                any("wdt_disable" in argument for argument in arguments)
            )
            drives = [
                argument
                for argument in arguments
                if argument.startswith("file=")
            ]
            self.assertEqual(len(drives), 2)
            copied_paths = [
                Path(argument.split(",", 1)[0].removeprefix("file="))
                for argument in drives
            ]
            self.assertNotIn(flash, copied_paths)
            self.assertNotIn(efuse, copied_paths)
            self.assertTrue(all(not path.exists() for path in copied_paths))
            self.assertEqual(flash.read_bytes(), b"flash")
            self.assertEqual(efuse.read_bytes(), b"efuse")

    def test_manifest_binds_flash_efuse_and_elf_by_sha256(self) -> None:
        runner = self._load_runner_module()
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            flash = directory / "qemu_flash.bin"
            efuse = directory / "qemu_efuse.bin"
            elf = directory / "firmware.elf"
            flash.write_bytes(b"flash-image")
            efuse.write_bytes(b"efuse-image")
            elf.write_bytes(b"firmware-elf")

            def entry(path: Path) -> dict[str, object]:
                return {
                    "path": str(path.resolve()),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size": path.stat().st_size,
                }

            manifest = directory / "qemu_manifest.json"

            def write_manifest() -> None:
                manifest.write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "images": {
                                "flash": entry(flash),
                                "efuse": entry(efuse),
                            },
                            "artifacts": {"elf": entry(elf)},
                        }
                    ),
                    encoding="utf-8",
                )

            write_manifest()
            self.assertEqual(runner._manifest_images(manifest), (flash, efuse))

            mutations = {
                "flash bytes": (flash, b"tampered-flash", "flash SHA-256"),
                "efuse bytes": (efuse, b"tampered-efuse", "efuse SHA-256"),
                "ELF bytes": (elf, b"tampered-elf", "ELF SHA-256"),
            }
            original = {
                flash: flash.read_bytes(),
                efuse: efuse.read_bytes(),
                elf: elf.read_bytes(),
            }
            for name, (path, contents, error) in mutations.items():
                with self.subTest(mutation=name):
                    path.write_bytes(contents)
                    with self.assertRaisesRegex(RuntimeError, error):
                        runner._manifest_images(manifest)
                    path.write_bytes(original[path])

            malformed = json.loads(manifest.read_text(encoding="utf-8"))
            del malformed["artifacts"]["elf"]["sha256"]
            manifest.write_text(json.dumps(malformed), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "ELF SHA-256"):
                runner._manifest_images(manifest)

    def test_failure_modes_are_rejected(self) -> None:
        cases = (
            ("fail_marker", "failure marker"),
            ("panic", "panic"),
            ("guru", "Guru Meditation"),
            ("abort", "abort"),
            ("watchdog", "watchdog"),
            ("restart_loop", "restart loop"),
            ("timeout", "timed out"),
            ("unexpected_exit", "unexpected exit"),
            ("missing_marker", "missing terminal marker"),
            ("unarmed_reset", "unarmed reset"),
            ("repeated_reset", "unarmed reset"),
            ("missing_stage", "missing required tracer marker"),
            ("out_of_order", "out-of-order tracer marker"),
        )
        for mode, expected_error in cases:
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_fake_qemu(Path(temporary_directory))
                    completed = self._run(paths, mode, timeout=0.3)

                    self.assertEqual(completed.returncode, 1)
                    self.assertEqual(completed.stdout.count(EXPECTED_MARKER), 0)
                    self.assertIn(expected_error, completed.stderr)

    def _run(
        self,
        paths: dict[str, Path],
        mode: str,
        *,
        timeout: float,
        expected_marker: str = EXPECTED_MARKER,
        timing_diagnostic: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["FAKE_QEMU_MODE"] = mode
        command = [
                sys.executable,
                str(RUNNER),
                "--qemu",
                str(paths["qemu"]),
                "--expect",
                expected_marker,
                "--timeout",
                str(timeout),
                "--log",
                str(paths["log"]),
            ]
        if timing_diagnostic:
            command.append("--timing-diagnostic")
        return subprocess.run(
            command,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
            timeout=4.0,
        )

    @staticmethod
    def _load_runner_module():
        spec = importlib.util.spec_from_file_location(
            "crossink_qemu_runner_contract", RUNNER
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load QEMU runner")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    @staticmethod
    def _create_full_fake_qemu(
        directory: Path, lines: tuple[str, ...]
    ) -> dict[str, Path]:
        fake_qemu = directory / "fake_full_qemu.py"
        fake_qemu.write_text(
            "\n".join(
                (
                    "import time",
                    f"lines = {lines!r}",
                    "for line in lines:",
                    "    print(line, flush=True)",
                    "time.sleep(5)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        return {"qemu": fake_qemu, "log": directory / "qemu.log"}

    @staticmethod
    def _create_fake_qemu(directory: Path) -> dict[str, Path]:
        fake_qemu = directory / "fake_qemu.py"
        fake_qemu.write_text(
            "\n".join(
                (
                    "import os",
                    "import sys",
                    "import time",
                    "",
                    "mode = os.environ['FAKE_QEMU_MODE']",
                    "",
                    "def emit(line):",
                    "    print(line, flush=True)",
                    "",
                    "emit('QEMU_BOOT seq=0')",
                    "if mode == 'pass':",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'armed_reset':",
                    "    emit('QEMU_EXPECT_RESET seq=0')",
                    "    emit('QEMU_BOOT seq=1')",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'fail_marker':",
                    "    emit('QEMU_STORAGE_FAIL reason=missing')",
                    "    time.sleep(5)",
                    "elif mode == 'panic':",
                    "    emit('panic: simulated fault')",
                    "    time.sleep(5)",
                    "elif mode == 'guru':",
                    "    emit('Guru Meditation Error: simulated fault')",
                    "    time.sleep(5)",
                    "elif mode == 'abort':",
                    "    emit('abort() was called')",
                    "    time.sleep(5)",
                    "elif mode == 'watchdog':",
                    "    emit('Task watchdog got triggered')",
                    "    time.sleep(5)",
                    "elif mode == 'restart_loop':",
                    "    emit('QEMU_BOOT seq=0')",
                    "    time.sleep(5)",
                    "elif mode == 'timeout':",
                    "    time.sleep(5)",
                    "elif mode == 'unexpected_exit':",
                    "    raise SystemExit(7)",
                    "elif mode == 'missing_marker':",
                    "    raise SystemExit(0)",
                    "elif mode == 'unarmed_reset':",
                    "    emit('QEMU_BOOT seq=1')",
                    "    time.sleep(5)",
                    "elif mode == 'repeated_reset':",
                    "    emit('QEMU_EXPECT_RESET seq=0')",
                    "    emit('QEMU_BOOT seq=1')",
                    "    emit('QEMU_BOOT seq=2')",
                    "    time.sleep(5)",
                    "elif mode == 'missing_stage':",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'out_of_order':",
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'real_command':",
                    "    import json",
                    "    from pathlib import Path",
                    "    Path(os.environ['FAKE_QEMU_ARGS']).write_text(",
                    "        json.dumps(sys.argv[1:]), encoding='utf-8'",
                    "    )",
                    "    for argument in sys.argv[1:]:",
                    "        if argument.startswith('file='):",
                    "            image = Path(argument.split(',', 1)[0][5:])",
                    "            with image.open('ab') as output:",
                    "                output.write(b'changed')",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "else:",
                    "    sys.stderr.write('unknown fake mode\\n')",
                    "    raise SystemExit(8)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        return {"qemu": fake_qemu, "log": directory / "qemu.log"}


if __name__ == "__main__":
    unittest.main()
