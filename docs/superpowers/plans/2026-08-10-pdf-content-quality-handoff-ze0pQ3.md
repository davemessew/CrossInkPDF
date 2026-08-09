# PDF reflow quality checkpoint — 2026-08-10

Status: implementation checkpoint only. The focused regressions described below are green, but the latest combined tree has not yet completed the full seven-PDF acceptance run, firmware build, release, or upload.

## Scope in this checkpoint

The pending diff is confined to `lib/PdfReflow/` and its focused tests under `test/pdf_extraction/` and `test/pdf_reflow_core/`.

- Preserves soft hyphens and `/ActualText` after the fixed 256-run workspace fills, so overflow extraction no longer produces splits such as `degra dation` and `tech nology`.
- Improves Unicode/font fallback and text-showing extraction, including long `TJ` arrays and spacing hints.
- Improves reading order, columns, tables, paragraph continuation, heading recognition, drop-cap handling, and whitespace normalization.
- Recognizes bold, all-caps subheadings in the upper content band while continuing to exclude running headers at the page edge.
- Repairs conservative split-capital heading joins such as `MA TTER` and `ACTUALL Y` without joining ordinary text such as `PROCESS T O`.

The unrelated working-tree edit in `lib/Epub/Epub/Section.cpp` is not part of this checkpoint and must remain untouched.

## Evidence already collected

### Cplett soft-hyphen regression

Four focused run-workspace/soft-hyphen contracts pass, including:

- `PdfContentInterpreterTest.PreservesSoftHyphenWordJoinAfterRunWorkspaceFills`
- `PdfContentInterpreterTest.RetainsActualTextSoftHyphenAfterRunWorkspaceFills`
- `SemanticWriterTest.RejoinsSoftHyphenAcrossSeparatedExtractionBlocks`

The real `j.cplett.2021.138378.pdf` replay completed under one CPU and 128 MB of container RAM. Its generated XHTML reported:

- `degra_space=0`
- `tech_space=0`
- `degradation=46`
- `technology=3`
- U+FFFD replacement glyphs: 0
- NBSPs: 0
- words: 7,005
- fixed PDF preparation peak: 63,488 bytes

The local output is under `.tmp/quality-current2/` and is intentionally not committed.

### Atomic Habits headings

The focused preparation contract `PdfPreparation.RecognizesBodySizeBoldAllCapsSubheadingsInTheTopContentBand` passes.

The latest focused writer contract `SemanticWriterTest.RejoinsSplitCapitalLettersInsideHeadings` passes after the final conservative suffix refinement.

The last real Atomic Habits replay, run immediately before that final one-line refinement, completed with 40 XHTML sections and correctly emitted headings including:

- `THE REAL REASON HABITS MATTER`
- `HOW LONG DOES IT ACTUALLY TAKE TO FORM A NEW HABIT?`

Because the real replay predates the final one-line refinement, it must be rerun before claiming final Atomic output quality. The old output is under `.tmp/quality-current4/` and is not committed.

### Resource constraints

- No framebuffer borrowing was introduced.
- The preparation workspace peak remains 63,488 bytes in the measured real-PDF replays.
- The real PDF checks above used `--cpus=1 --memory=128m`.

## First work tomorrow

1. Rebuild `AtomicPdfRepro` so it includes the final `PdfSemanticWriter` refinement.
2. Rerun Atomic Habits under one CPU and 128 MB.
3. Verify all 40 XHTML files, including the two headings above and the non-join control `PROCESS T O`.
4. If the faithful replay is green, run the complete seven-PDF corpus under the same limits.

Atomic build command:

```powershell
docker run --rm --cpus=2 --memory=768m -v "${PWD}:/workspace" -w /workspace crossink-pdf-simulator:ubuntu24.04-sdl2-v1 cmake --build /workspace/.tmp/atomic-repro-build-fix --target AtomicPdfRepro -j 1
```

Atomic replay command:

```powershell
docker run --rm --cpus=1 --memory=128m -e CROSSINK_DUMP_DIR=/workspace/.tmp/quality-final -v "${PWD}:/workspace" -v "C:\Users\David\Desktop:/input:ro" -w /workspace crossink-pdf-simulator:ubuntu24.04-sdl2-v1 /workspace/.tmp/atomic-repro-build-fix/AtomicPdfRepro "/input/James Clear Atomic Habits.pdf"
```

## Seven-PDF acceptance corpus

- `C:\Users\David\Desktop\James Clear Atomic Habits.pdf`
- `C:\Users\David\Downloads\j.cplett.2021.138378.pdf`
- `C:\Users\David\Downloads\50310861M.pdf`
- `C:\Users\David\Downloads\20260114_Corporate_Design_Manual.pdf`
- `C:\Users\David\Downloads\1.1745237.pdf`
- `C:\Users\David\Downloads\pola.27048.pdf`
- `C:\Users\David\Downloads\science.244.4907.997.pdf`

For every generated XHTML file, check:

- valid XML;
- no U+FFFD replacement glyphs;
- no NBSPs or invalid control characters;
- no accidental doubled spaces;
- sensible headings and paragraph breaks;
- sensible drop caps;
- correct table structure and row order;
- correct multi-column reading order.

Representative source/output comparisons:

- Cplett: pages 2, 4, and 9
- Sony manual: pages 215, 315, and 354
- Corporate design manual: pages 4, 14, 25, and 32
- `1.1745237.pdf`: pages 2, 8, and 12
- Pola: pages 2, 3, and 4
- Science: page 1
- Atomic Habits: pages 41 and 121, plus representative chapter and table pages

## Remaining verification and release work

1. Run the relevant focused and broader `PdfReflowCoreTest` and `PdfExtractionTest` suites after the real corpus replay.
2. Inspect the existing `PdfContentInterpreterTest.HandlesTextStateShowingOperatorsAndContentArrays` expectation before changing it. It expects `A B C D E F G` and seven runs, while the more accurate operator/TJ joining currently produces `A B CD E F G` and five runs. Do not update it blindly.
3. Build the real device firmware once at the end with `pio run -e default`. Do not repeatedly rebuild while content work is still changing.
4. Add the final changelog/version notes only after corpus and firmware acceptance.
5. Resume the separate mission-critical preparation-speed and 86% stall work only after this content-quality gate is stable.
6. No GitHub push, release replacement, binary upload, or device flashing has been done for this checkpoint.

## Suggested next-session skills

- `diagnose` for any faithful real-PDF failure or output regression.
- `verification-before-completion` before firmware/release claims.
- `scope-discipline` from `.claude/skills/` to keep PDF work isolated from EPUB and unrelated firmware code.
