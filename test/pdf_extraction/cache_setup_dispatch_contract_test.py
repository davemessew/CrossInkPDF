#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "lib" / "PdfReflow" / "PdfPreparation.h"
SOURCE = ROOT / "lib" / "PdfReflow" / "PdfPreparation.cpp"

HANDLERS = (
    ("stepSetupCheckpointAndManifest", "Idle", "OpenResumeLedger"),
    ("stepSetupDiscoveryRestore", "OpenResumeJournal", "ValidateResumePageClose"),
    ("stepSetupResumeProductValidation", "ReadResumeMetadata", "ValidateEmitSectionsImageFilesClose"),
    ("stepSetupGeneration", "SelectGeneration", "CreateSectionDirectory"),
    ("stepSetupOutputs", "ReadCapacity", "Complete"),
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def verify(header: str, source: str) -> None:
    dispatch = function_body(source, "PdfStepResult PdfPreparation::stepSetupCache(PdfWorkBudget& budget)")
    assert "if (cacheSetupStage_ == CacheSetupStage::" not in dispatch
    assert dispatch.count("return stepSetup") == len(HANDLERS)

    for index, (name, first_stage, last_stage) in enumerate(HANDLERS):
        declaration = f"PdfStepResult {name}(PdfWorkBudget& budget);"
        assert header.count(declaration) == 1, declaration
        signature = f"PdfStepResult PdfPreparation::{name}(PdfWorkBudget& budget)"
        assert source.count(signature) == 1, signature
        body = function_body(source, signature)
        assert f"CacheSetupStage::{first_stage}" in body, (name, first_stage)
        assert f"CacheSetupStage::{last_stage}" in body, (name, last_stage)
        if index + 1 < len(HANDLERS):
            next_first = HANDLERS[index + 1][1]
            assert f"cacheSetupStage_ == CacheSetupStage::{next_first}" not in body, (name, next_first)


def rejected(header: str, source: str) -> bool:
    try:
        verify(header, source)
    except (AssertionError, ValueError):
        return True
    return False


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    verify(header, source)

    dispatch_signature = "PdfStepResult PdfPreparation::stepSetupCache(PdfWorkBudget& budget)"
    dispatch = function_body(source, dispatch_signature)
    old_linear = dispatch.replace(
        "return stepSetupCheckpointAndManifest(budget);",
        "if (cacheSetupStage_ == CacheSetupStage::Idle) { return PdfStepResult::paused(); }",
        1,
    )
    old_linear_source = source.replace(dispatch, old_linear, 1)
    assert rejected(header, old_linear_source), "old per-stage linear dispatch escaped the contract"

    first_signature = "PdfStepResult PdfPreparation::stepSetupCheckpointAndManifest(PdfWorkBudget& budget)"
    first_body = function_body(source, first_signature)
    moved_body = first_body.replace("CacheSetupStage::OpenResumeLedger", "CacheSetupStage::OpenResumeJournal")
    assert moved_body != first_body
    moved_source = source.replace(first_body, moved_body, 1)
    assert rejected(header, moved_source), "moving a stage across handler ranges escaped the contract"


if __name__ == "__main__":
    main()
