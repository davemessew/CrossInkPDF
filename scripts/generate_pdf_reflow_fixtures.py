#!/usr/bin/env python3
"""Generate the deterministic, license-safe PDF reflow fixture corpus."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import struct
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "test" / "pdf_reflow_core" / "fixtures"
QEMU_CLASSIC_OUTPUT = REPO_ROOT / "test" / "qemu" / "data" / "qemu" / "classic_text.pdf"
PDF_HEADER = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"


def pdf_string(value: str) -> bytes:
    return (
        value.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)").encode("ascii")
    )


def stream(data: bytes, dictionary: bytes = b"") -> bytes:
    entries = dictionary.strip()
    if entries.startswith(b"<<") and entries.endswith(b">>"):
        entries = entries[2:-2].strip()
    prefix = b"<< /Length " + str(len(data)).encode("ascii")
    if entries:
        prefix += b" " + entries
    return prefix + b" >>\nstream\n" + data + b"\nendstream"


class ClassicPdf:
    def __init__(self, version: str = "1.7") -> None:
        self.header = f"%PDF-{version}\n".encode("ascii") + b"%\xe2\xe3\xcf\xd3\n"
        self.objects: dict[int, bytes] = {}

    def add(self, number: int, body: bytes) -> None:
        if number <= 0 or number in self.objects:
            raise ValueError(f"invalid or duplicate object {number}")
        self.objects[number] = body

    def render(
        self,
        trailer_entries: bytes = b"/Root 1 0 R",
        startxref_override: int | None = None,
        trailer_factory: Callable[[int], bytes] | None = None,
    ) -> tuple[bytes, int]:
        output = bytearray(self.header)
        offsets: dict[int, int] = {}
        for number in sorted(self.objects):
            offsets[number] = len(output)
            output.extend(f"{number} 0 obj\n".encode("ascii"))
            output.extend(self.objects[number])
            output.extend(b"\nendobj\n")

        xref_offset = len(output)
        size = max(self.objects, default=0) + 1
        output.extend(f"xref\n0 {size}\n".encode("ascii"))
        output.extend(b"0000000000 65535 f \n")
        for number in range(1, size):
            if number in offsets:
                output.extend(f"{offsets[number]:010d} 00000 n \n".encode("ascii"))
            else:
                output.extend(b"0000000000 00000 f \n")

        entries = trailer_factory(xref_offset) if trailer_factory else trailer_entries
        output.extend(f"trailer\n<< /Size {size} ".encode("ascii"))
        output.extend(entries)
        output.extend(b" >>\nstartxref\n")
        output.extend(str(xref_offset if startxref_override is None else startxref_override).encode("ascii"))
        output.extend(b"\n%%EOF\n")
        return bytes(output), xref_offset


@dataclass(frozen=True)
class Fixture:
    pdf: bytes
    transcript: str
    geometry_order: tuple[str, ...] = ()
    outline: tuple[dict[str, object], ...] = ()
    links: tuple[dict[str, object], ...] = ()
    warning: str | None = None
    error: str | None = None
    image_hashes: tuple[tuple[str, str], ...] = ()

    def expected(self, name: str) -> bytes:
        payload = {
            "fixture": name,
            "transcript": self.transcript,
            "word_count": len(re.findall(r"\S+", self.transcript)),
            "warning": self.warning,
            "error": self.error,
            "geometry_order": list(self.geometry_order),
            "outline_link_map": {
                "outline": list(self.outline),
                "links": list(self.links),
            },
            "image_hashes": dict(self.image_hashes),
        }
        return (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")


def font_object() -> bytes:
    return b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"


def one_page_pdf(
    content: bytes,
    *,
    content_dictionary: bytes = b"",
    page_extra: bytes = b"",
    catalog_extra: bytes = b"",
    extra_objects: dict[int, bytes] | None = None,
    trailer_entries: bytes = b"/Root 1 0 R",
    startxref_override: int | None = None,
    trailer_factory: Callable[[int], bytes] | None = None,
) -> bytes:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R " + catalog_extra + b" >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R "
        + page_extra
        + b" >>",
    )
    pdf.add(4, stream(content, content_dictionary))
    pdf.add(5, font_object())
    for number, body in sorted((extra_objects or {}).items()):
        pdf.add(number, body)
    return pdf.render(
        trailer_entries=trailer_entries,
        startxref_override=startxref_override,
        trailer_factory=trailer_factory,
    )[0]


def make_classic_text() -> Fixture:
    text = "Hello PDF"
    content = b"BT /F1 12 Tf 72 720 Td (Hello PDF) Tj ET"
    return Fixture(one_page_pdf(content), text, (text,))


def make_incremental_update() -> Fixture:
    base = ClassicPdf()
    base.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    base.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    base.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    )
    base.add(4, stream(b"BT /F1 12 Tf 72 720 Td (Original revision.) Tj ET"))
    base.add(5, font_object())
    base_bytes, previous_xref = base.render()

    output = bytearray(base_bytes)
    offsets: dict[int, int] = {}
    for number, body in (
        (
            3,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
        ),
        (6, stream(b"BT /F1 12 Tf 72 720 Td (Updated revision wins.) Tj ET")),
    ):
        offsets[number] = len(output)
        output.extend(f"{number} 0 obj\n".encode("ascii") + body + b"\nendobj\n")
    xref_offset = len(output)
    output.extend(b"xref\n3 1\n")
    output.extend(f"{offsets[3]:010d} 00000 n \n".encode("ascii"))
    output.extend(b"6 1\n")
    output.extend(f"{offsets[6]:010d} 00000 n \n".encode("ascii"))
    output.extend(
        f"trailer\n<< /Size 7 /Root 1 0 R /Prev {previous_xref} >>\n"
        f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    return Fixture(bytes(output), "Updated revision wins.", ("Updated revision wins.",))


def make_incremental_xref_stream() -> Fixture:
    base = ClassicPdf()
    base.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    base.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    base.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    )
    base.add(4, stream(b"BT /F1 12 Tf 72 720 Td (Older classic revision.) Tj ET"))
    base.add(5, font_object())
    base_bytes, previous_xref = base.render()

    output = bytearray(base_bytes)
    offsets: dict[int, int] = {}
    for number, body in (
        (
            3,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
        ),
        (
            6,
            stream(
                b"BT /F1 12 Tf 72 720 Td "
                b"(Updated xref stream wins.) Tj ET"
            ),
        ),
    ):
        offsets[number] = len(output)
        output.extend(f"{number} 0 obj\n".encode("ascii") + body + b"\nendobj\n")
    xref_offset = len(output)

    def entry(entry_type: int, field2: int, field3: int) -> bytes:
        return bytes([entry_type]) + struct.pack(">I", field2) + struct.pack(">H", field3)

    xref_data = b"".join(
        (
            entry(1, offsets[3], 0),
            entry(1, offsets[6], 0),
            entry(1, xref_offset, 0),
        )
    )
    xref_stream = stream(
        zlib.compress(xref_data, 9),
        (
            f"/Type /XRef /Size 8 /Root 1 0 R /Prev {previous_xref} "
            "/W [1 4 2] /Index [3 1 6 2] /Filter /FlateDecode"
        ).encode("ascii"),
    )
    output.extend(b"7 0 obj\n" + xref_stream + b"\nendobj\n")
    output.extend(f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii"))
    text = "Updated xref stream wins."
    return Fixture(bytes(output), text, (text,))


def make_xref_stream_objstm() -> Fixture:
    content = b"BT /F1 12 Tf 72 720 Td (Compressed object stream text.) Tj ET"
    compressed_objects = (
        (1, b"<< /Type /Catalog /Pages 2 0 R >>"),
        (2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        (
            3,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        ),
    )
    object_data = bytearray()
    index_entries: list[tuple[int, int]] = []
    for number, body in compressed_objects:
        index_entries.append((number, len(object_data)))
        object_data.extend(body + b" ")
    index = b" ".join(f"{number} {offset}".encode("ascii") for number, offset in index_entries) + b" "
    object_stream = stream(
        zlib.compress(index + object_data, 9),
        (
            f"/Type /ObjStm /N {len(compressed_objects)} "
            f"/First {len(index)} /Filter /FlateDecode"
        ).encode("ascii"),
    )

    output = bytearray(PDF_HEADER)
    offsets: dict[int, int] = {}
    for number, body in ((4, font_object()), (5, stream(content)), (6, object_stream)):
        offsets[number] = len(output)
        output.extend(f"{number} 0 obj\n".encode("ascii") + body + b"\nendobj\n")
    xref_offset = len(output)

    def entry(entry_type: int, field2: int, field3: int) -> bytes:
        return bytes([entry_type]) + struct.pack(">I", field2) + struct.pack(">H", field3)

    entries = [entry(0, 0, 65535)]
    entries.extend(entry(2, 6, index_value) for index_value in range(3))
    entries.extend(entry(1, offsets[number], 0) for number in (4, 5, 6))
    entries.append(entry(1, xref_offset, 0))
    xref_data = b"".join(entries)
    xref_stream = stream(
        zlib.compress(xref_data, 9),
        b"/Type /XRef /Size 8 /Root 1 0 R /W [1 4 2] "
        b"/Index [0 2 2 3 5 3] /Filter /FlateDecode",
    )
    output.extend(b"7 0 obj\n" + xref_stream + b"\nendobj\n")
    output.extend(f"startxref\n{xref_offset}\n%%EOF\n".encode("ascii"))
    return Fixture(bytes(output), "Compressed object stream text.", ("Compressed object stream text.",))


def make_filter_matrix() -> Fixture:
    texts = (
        ("Unfiltered stream.", b""),
        ("Flate stream.", b"/Filter /FlateDecode"),
        ("ASCII hex stream.", b"/Filter /ASCIIHexDecode"),
        ("ASCII eighty five stream.", b"/Filter /ASCII85Decode"),
        ("Chained filter stream.", b"/Filter [/ASCII85Decode /FlateDecode]"),
    )
    encoded: list[tuple[bytes, bytes]] = []
    for index, (text, filter_dictionary) in enumerate(texts):
        raw = f"BT /F1 12 Tf 72 {720 - index * 40} Td ({text}) Tj ET".encode("ascii")
        if filter_dictionary == b"/Filter /FlateDecode":
            data = zlib.compress(raw, 9)
        elif filter_dictionary == b"/Filter /ASCIIHexDecode":
            data = raw.hex().upper().encode("ascii") + b">"
        elif filter_dictionary == b"/Filter /ASCII85Decode":
            data = base64.a85encode(raw, adobe=False) + b"~>"
        elif filter_dictionary.startswith(b"/Filter ["):
            data = base64.a85encode(zlib.compress(raw, 9), adobe=False) + b"~>"
        else:
            data = raw
        encoded.append((data, filter_dictionary))

    pdf = ClassicPdf()
    page_ids = [3, 5, 7, 9, 11]
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [" + b" ".join(f"{n} 0 R".encode() for n in page_ids) + b"] /Count 5 >>")
    pdf.add(13, font_object())
    for page_index, (page_id, (data, dictionary)) in enumerate(zip(page_ids, encoded)):
        content_id = page_id + 1
        pdf.add(
            page_id,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 13 0 R >> >> /Contents "
            + f"{content_id} 0 R".encode()
            + b" >>",
        )
        pdf.add(content_id, stream(data, dictionary))
    transcript = " ".join(text for text, _ in texts)
    return Fixture(pdf.render()[0], transcript, tuple(text for text, _ in texts))


def make_tounicode_simple_and_cid() -> Fixture:
    simple_cmap = (
        b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        b"1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        b"1 beginbfrange\n<20> <7E> <0020>\nendbfrange\n"
        b"endcmap\nend\nend"
    )
    cid_cmap = (
        b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        b"1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
        b"2 beginbfrange\n<0020> <007E> <0020>\n<0100> <0101> [<03A9> <03C0>]\nendbfrange\n"
        b"endcmap\nend\nend"
    )
    content = b"BT /F1 12 Tf 72 720 Td (Simple) Tj /F2 12 Tf 0 -30 Td <00430049004401000101> Tj ET"
    extras = {
        6: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding /ToUnicode 7 0 R >>",
        7: stream(simple_cmap),
        8: b"<< /Type /Font /Subtype /Type0 /BaseFont /FixtureCID /Encoding /Identity-H "
        b"/DescendantFonts [9 0 R] /ToUnicode 10 0 R >>",
        9: b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /FixtureCID "
        b"/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
        10: stream(cid_cmap),
    }
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 6 0 R /F2 8 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(4, stream(content))
    for number, body in extras.items():
        pdf.add(number, body)
    return Fixture(pdf.render()[0], "Simple CIDΩπ", ("Simple", "CIDΩπ"))


def make_operators_actualtext_forms() -> Fixture:
    page_content = (
        b"q 1 0 0 1 0 0 cm /Fm1 Do Q\n"
        b"BT /F1 10 Tf 1 0 0 1 72 680 Tm "
        b"/Span << /ActualText (Accessible replacement) >> BDC "
        b"(Visual glyphs) Tj EMC ET"
    )
    form_content = b"BT /F1 18 Tf 20 40 Td [(Form) -120 (heading)] TJ ET"
    form = stream(
        form_content,
        b"/Type /XObject /Subtype /Form /BBox [0 0 400 80] "
        b"/Resources << /Font << /F1 5 0 R >> >>",
    )
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Fm1 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(4, stream(page_content))
    pdf.add(5, font_object())
    pdf.add(6, form)
    return Fixture(
        pdf.render()[0],
        "Form heading Accessible replacement",
        ("Form heading", "Accessible replacement"),
    )


def image_object(pixels: bytes, width: int = 4, height: int = 4) -> bytes:
    return stream(
        zlib.compress(pixels, 9),
        f"/Type /XObject /Subtype /Image /Width {width} /Height {height} "
        "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode".encode("ascii"),
    )


def make_image_text_fixture(
    hidden_text: str | None,
    visible_text: str | None,
    *,
    name: str,
) -> Fixture:
    pixels = bytes(range(0, 256, 16))
    operations = [b"q 120 0 0 120 72 580 cm /Im1 Do Q"]
    if visible_text:
        operations.append(
            b"BT /F1 12 Tf 72 720 Td (" + pdf_string(visible_text) + b") Tj ET"
        )
    if hidden_text:
        operations.append(
            b"BT /F1 12 Tf 3 Tr 72 620 Td (" + pdf_string(hidden_text) + b") Tj ET"
        )
    transcript = visible_text or hidden_text or ""
    error = "ImageOnly" if not transcript else None
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Im1 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(4, stream(b"\n".join(operations)))
    pdf.add(5, font_object())
    pdf.add(6, image_object(pixels))
    return Fixture(
        pdf.render()[0],
        transcript,
        (transcript,) if transcript else (),
        error=error,
        image_hashes=(("image-6-gray", hashlib.sha256(pixels).hexdigest()),),
    )


def make_columns_table() -> Fixture:
    content = (
        b"BT /F1 11 Tf "
        b"72 720 Td (Left one.) Tj "
        b"288 0 Td (Right one.) Tj "
        b"-288 -24 Td (Left two.) Tj "
        b"288 0 Td (Right two.) Tj "
        b"-288 -60 Td (Name) Tj 140 0 Td (Value) Tj "
        b"-140 -20 Td (Alpha) Tj 140 0 Td (10) Tj "
        b"ET"
    )
    order = ("Left one.", "Left two.", "Right one.", "Right two.", "Name Value", "Alpha 10")
    return Fixture(one_page_pdf(content), " ".join(order), order)


def make_repeated_bands() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R] /Count 3 >>")
    pdf.add(9, font_object())
    bodies = ("First body.", "Second body.", "Third body.")
    for page_id, body in zip((3, 5, 7), bodies):
        content_id = page_id + 1
        content = (
            b"BT /F1 9 Tf 72 760 Td (Repeated Header) Tj "
            b"0 -80 Td /F1 12 Tf (" + pdf_string(body) + b") Tj "
            b"0 -620 Td /F1 9 Tf (Repeated Footer) Tj ET"
        )
        pdf.add(
            page_id,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 9 0 R >> >> /Contents "
            + f"{content_id} 0 R".encode()
            + b" >>",
        )
        pdf.add(content_id, stream(content))
    return Fixture(pdf.render()[0], " ".join(bodies), bodies, warning="RepeatedBandsSuppressed")


def make_dense_spill() -> Fixture:
    fragments = tuple(
        f"Run{i:03d} bounded-spill-payload-{i:03d}." for i in range(300)
    )
    operations = [b"BT /F1 8 Tf"]
    for index, fragment in enumerate(fragments):
        x = 40 + (index % 3) * 180
        y = 760 - (index // 3) * 7
        operations.append(
            f"1 0 0 1 {x} {y} Tm ".encode("ascii") + b"(" + pdf_string(fragment) + b") Tj"
        )
    operations.append(b"ET")
    return Fixture(one_page_pdf(b"\n".join(operations)), " ".join(fragments), fragments)


def make_vector_caption() -> Fixture:
    content = (
        b"0 0 0 RG 2 w 72 500 m 240 620 l 410 500 l h S "
        b"BT /F1 11 Tf 72 470 Td (Figure one: bounded vector caption.) Tj ET"
    )
    return Fixture(
        one_page_pdf(content),
        "Figure one: bounded vector caption.",
        ("Figure one: bounded vector caption.",),
        warning="VectorArtOmitted",
    )


def make_font_size(size: int) -> Fixture:
    text = "Typography uses device defaults."
    content = f"BT /F1 {size} Tf 72 650 Td ({text}) Tj ET".encode("ascii")
    return Fixture(one_page_pdf(content), text, (text,))


def make_linearized_hint() -> Fixture:
    content = b"BT /F1 12 Tf 72 720 Td (Linearized hints are ignored.) Tj ET"
    return Fixture(
        one_page_pdf(
            content,
            extra_objects={
                6: b"<< /Linearized 1 /L 999999 /H [0 0] /O 3 /E 0 /N 1 /T 0 >>"
            },
        ),
        "Linearized hints are ignored.",
        ("Linearized hints are ignored.",),
        warning="LinearizationHintsIgnored",
    )


def make_required_filter(filter_name: bytes, error: str) -> Fixture:
    content = b"Required unsupported stream data."
    return Fixture(
        one_page_pdf(content, content_dictionary=b"/Filter /" + filter_name),
        "",
        error=error,
    )


def make_bad_startxref() -> Fixture:
    content = b"BT /F1 12 Tf 72 720 Td (Unreachable because xref is bad.) Tj ET"
    return Fixture(
        one_page_pdf(content, startxref_override=99999999),
        "",
        error="MalformedXref",
    )


def make_xref_prev_cycle() -> Fixture:
    content = b"BT /F1 12 Tf 72 720 Td (Trailer cycle.) Tj ET"
    return Fixture(
        one_page_pdf(
            content,
            trailer_factory=lambda xref: f"/Root 1 0 R /Prev {xref}".encode("ascii"),
        ),
        "",
        error="TrailerCycle",
    )


def make_oversized_length() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        b"<< /Length 18446744073709551615 >>\nstream\nBT (unsafe) Tj ET\nendstream",
    )
    pdf.add(5, font_object())
    return Fixture(pdf.render()[0], "", error="InvalidStreamLength")


def make_flate_bomb() -> Fixture:
    expanded = b"A" * (1024 * 1024)
    return Fixture(
        one_page_pdf(zlib.compress(expanded, 9), content_dictionary=b"/Filter /FlateDecode"),
        "",
        error="ExpansionLimit",
    )


def make_encrypted() -> Fixture:
    content = b"BT /F1 12 Tf 72 720 Td (Encrypted content.) Tj ET"
    return Fixture(
        one_page_pdf(
            content,
            extra_objects={
                6: b"<< /Filter /Standard /V 1 /R 2 /O <00> /U <00> /P -4 >>"
            },
            trailer_entries=b"/Root 1 0 R /Encrypt 6 0 R",
        ),
        "",
        error="Encrypted",
    )


def make_page_tree_inherited() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 /Resources 10 0 R >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Contents [4 0 R 5 0 R] >>",
    )
    pdf.add(4, stream(b"BT /F1 12 Tf 72 720 Td (First A) Tj ET"))
    pdf.add(5, stream(b"BT /F1 12 Tf 0 -24 Td (First B) Tj ET"))
    pdf.add(
        6,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 7 0 R >>",
    )
    pdf.add(7, stream(b"BT /F1 12 Tf 72 720 Td (Second) Tj ET"))
    pdf.add(8, font_object())
    pdf.add(10, b"<< /Font << /F1 8 0 R >> >>")
    return Fixture(
        pdf.render()[0],
        "First A First B Second",
        ("First A", "First B", "Second"),
    )


def make_page_tree_cycle() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [2 0 R] /Count 1 >>")
    return Fixture(pdf.render()[0], "", error="PageTreeCycle")


def make_page_tree_deep() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    first_page_object = 68
    for number in range(2, first_page_object):
        child = number + 1
        pdf.add(
            number,
            f"<< /Type /Pages /Kids [{child} 0 R] /Count 1 >>".encode("ascii"),
        )
    pdf.add(
        first_page_object,
        b"<< /Type /Page /Parent 67 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 70 0 R >> >> /Contents 69 0 R >>",
    )
    pdf.add(69, stream(b"BT /F1 12 Tf 72 720 Td (Too deep.) Tj ET"))
    pdf.add(70, font_object())
    return Fixture(pdf.render()[0], "", error="PageTreeDepth")


def make_page_tree_count_overflow() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [] /Count 5001 >>")
    return Fixture(pdf.render()[0], "", error="PageCountLimit")


def build_fixtures() -> dict[str, Fixture]:
    fixtures = {
        "classic_text": make_classic_text(),
        "incremental_update": make_incremental_update(),
        "incremental_xref_stream": make_incremental_xref_stream(),
        "xref_stream_objstm": make_xref_stream_objstm(),
        "filter_matrix": make_filter_matrix(),
        "tounicode_simple_and_cid": make_tounicode_simple_and_cid(),
        "operators_actualtext_forms": make_operators_actualtext_forms(),
        "hidden_ocr": make_image_text_fixture(
            "Hidden OCR layer text.", None, name="hidden_ocr"
        ),
        "hidden_ocr_visible_duplicate": make_image_text_fixture(
            "Duplicate visible text.", "Duplicate visible text.", name="hidden_ocr_visible_duplicate"
        ),
        "scan_only": make_image_text_fixture(None, None, name="scan_only"),
        "columns_table": make_columns_table(),
        "repeated_bands": make_repeated_bands(),
        "dense_spill": make_dense_spill(),
        "vector_caption": make_vector_caption(),
        "font_size_6": make_font_size(6),
        "font_size_72": make_font_size(72),
        "linearized_hint": make_linearized_hint(),
        "lzw_required": make_required_filter(b"LZWDecode", "UnsupportedFilter"),
        "bad_startxref": make_bad_startxref(),
        "xref_prev_cycle": make_xref_prev_cycle(),
        "oversized_length": make_oversized_length(),
        "flate_bomb": make_flate_bomb(),
        "encrypted": make_encrypted(),
        "page_tree_inherited": make_page_tree_inherited(),
        "page_tree_cycle": make_page_tree_cycle(),
        "page_tree_deep": make_page_tree_deep(),
        "page_tree_count_overflow": make_page_tree_count_overflow(),
    }
    return dict(sorted(fixtures.items()))


def generated_files() -> dict[str, bytes]:
    output: dict[str, bytes] = {}
    for name, fixture in build_fixtures().items():
        output[f"{name}.pdf"] = fixture.pdf
        output[f"{name}.expected.json"] = fixture.expected(name)
    checksum_lines = [
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(output.items())
    ]
    output["SHA256SUMS"] = "".join(checksum_lines).encode("ascii")
    return output


def write_files(output_directory: Path, files: dict[str, bytes]) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    expected_names = set(files)
    for existing in output_directory.iterdir():
        if existing.is_file() and existing.name not in expected_names:
            existing.unlink()
    for name, data in files.items():
        (output_directory / name).write_bytes(data)


def check_files(
    output_directory: Path,
    files: dict[str, bytes],
    staged_classic: Path | None = None,
) -> int:
    with tempfile.TemporaryDirectory(prefix="crossink-pdf-fixtures-") as temporary:
        regenerated = Path(temporary)
        write_files(regenerated, files)
        expected_names = set(files)
        actual_names = (
            {path.name for path in output_directory.iterdir() if path.is_file()}
            if output_directory.exists()
            else set()
        )
        differences = sorted(expected_names.symmetric_difference(actual_names))
        for name in sorted(expected_names & actual_names):
            if (regenerated / name).read_bytes() != (output_directory / name).read_bytes():
                differences.append(name)
        if staged_classic is not None and (
            not staged_classic.is_file()
            or staged_classic.read_bytes() != files["classic_text.pdf"]
        ):
            differences.append("qemu/classic_text.pdf")
        if differences:
            print("PDF fixture corpus differs:")
            for name in sorted(set(differences)):
                print(f"  {name}")
            return 1
    print(f"PDF fixture corpus is deterministic ({len(files) - 1} artifacts).")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="compare regenerated bytes with committed files")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()

    files = generated_files()
    stage_qemu = arguments.output.resolve() == DEFAULT_OUTPUT.resolve()
    if arguments.check:
        return check_files(
            arguments.output,
            files,
            QEMU_CLASSIC_OUTPUT if stage_qemu else None,
        )
    write_files(arguments.output, files)
    if stage_qemu:
        QEMU_CLASSIC_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        QEMU_CLASSIC_OUTPUT.write_bytes(files["classic_text.pdf"])
    print(f"Wrote {len(files) - 1} PDF fixture artifacts to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
