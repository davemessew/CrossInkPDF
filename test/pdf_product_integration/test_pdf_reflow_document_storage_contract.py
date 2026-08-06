from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "lib" / "PdfReflow" / "PdfReflowDocument.h"
SOURCE = REPO_ROOT / "lib" / "PdfReflow" / "PdfReflowDocument.cpp"


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


def storage_contract_failures(header: str, source: str) -> list[str]:
    failures: list[str] = []
    for fixed in (
        "manifestSectionSizes_",
        "manifestSectionCrcs_",
        "manifestSectionSeen_",
        "std::array<PdfCachedResourceRecord, MaxCachedResources> resources_",
    ):
        if fixed in header:
            failures.append(f"fixed validation storage: {fixed}")
    for required in (
        "std::unique_ptr<uint8_t[]> validationStorage_",
        "struct ValidationStorageLayout",
        "computeValidationStorageLayout",
        "static_assert(MaxValidationStorageBytes == 4640",
        "static_assert(sizeof(PdfReflowDocument)",
    ):
        if required not in header:
            failures.append(f"missing header contract: {required}")

    if "computeValidationStorageLayout(" not in source:
        failures.append("missing exact layout implementation")
        return failures
    layout = function_body(source, "computeValidationStorageLayout")
    for required in (
        "checkedMultiplySize",
        "checkedAddSize",
        "alignUpSize",
        "sizeof(ManifestSectionValidationRecord)",
        "sizeof(PdfCachedResourceRecord)",
    ):
        if required not in layout:
            failures.append(f"layout arithmetic: {required}")
    allocation = function_body(source, "allocateValidationStorage")
    for required in (
        "makeUniqueNoThrow<uint8_t[]>(layout.bytes)",
        "std::memset(validationStorage_.get(), 0, layout.bytes)",
        "if (layout.bytes == 0)",
        "PdfError::InsufficientMemory",
        "LOG_ERR",
    ):
        if required not in allocation:
            failures.append(f"allocation contract: {required}")
    if "MaxValidationStorageBytes" in allocation:
        failures.append("maximum-sized validation allocation")
    return failures


class PdfReflowDocumentStorageContractTest(unittest.TestCase):
    def test_validation_storage_is_exact_count_packed_and_fallible(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        self.assertEqual(storage_contract_failures(header, source), [])

    def test_fixed_capacity_and_overallocation_mutations_are_rejected(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        self.assertEqual(storage_contract_failures(header, source), [])
        mutations = {
            "fixed section array": (
                header.replace(
                    "std::unique_ptr<uint8_t[]> validationStorage_;",
                    "std::unique_ptr<uint8_t[]> validationStorage_;\n"
                    "  std::array<uint32_t, PdfMetadataLimits::MaxSections> "
                    "manifestSectionSizes_{};",
                    1,
                ),
                source,
            ),
            "fixed resource array": (
                header.replace(
                    "std::unique_ptr<uint8_t[]> validationStorage_;",
                    "std::unique_ptr<uint8_t[]> validationStorage_;\n"
                    "  std::array<PdfCachedResourceRecord, MaxCachedResources> "
                    "resources_{};",
                    1,
                ),
                source,
            ),
            "maximum allocation": (
                header,
                source.replace(
                    "makeUniqueNoThrow<uint8_t[]>(layout.bytes)",
                    "makeUniqueNoThrow<uint8_t[]>(MaxValidationStorageBytes)",
                    1,
                ),
            ),
        }
        for name, (mutated_header, mutated_source) in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual((mutated_header, mutated_source), (header, source))
                self.assertNotEqual(
                    storage_contract_failures(mutated_header, mutated_source),
                    [],
                )


if __name__ == "__main__":
    unittest.main()
