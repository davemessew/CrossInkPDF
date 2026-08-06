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
QEMU_NAVIGATION_OUTPUT = (
    REPO_ROOT / "test" / "qemu" / "data" / "qemu" / "navigation_outline.pdf"
)
QEMU_POSITIVE_OUTPUTS = {
    name: REPO_ROOT / "test" / "qemu" / "data" / "qemu" / name
    for name in ("hidden_ocr.pdf", "columns_table.pdf", "jpeg_caption.pdf")
}
PDF_HEADER = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
# Pillow 12.2.0: L, 16x16 split, quality=90, subsampling=0, progressive=False, optimize=False.
JPEGDEC_BASELINE_GRAY_SPLIT_JPEG_BASE64 = (
    b"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsK"
    b"CwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/wAALCAAQABABAREA/8QAHwAAAQUBAQEB"
    b"AQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1Fh"
    b"ByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZ"
    b"WmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
    b"x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APyqr+qiv5V6/qor/9k="
)


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


def baseline_gray_split_jpeg() -> bytes:
    """Return a deterministic 16x16 baseline JPEG with black/white block columns."""

    def segment(marker: int, payload: bytes) -> bytes:
        return b"\xff" + bytes((marker,)) + struct.pack(">H", len(payload) + 2) + payload

    output = bytearray(b"\xff\xd8")
    output.extend(segment(0xDB, b"\x00" + bytes((1,)) * 64))
    output.extend(
        segment(
            0xC0,
            b"\x08"
            + struct.pack(">HH", 16, 16)
            + b"\x01"
            + b"\x01\x11\x00",
        )
    )
    one_code = bytes((1,)) + bytes(15)
    output.extend(
        segment(
            0xC4,
            b"\x00" + one_code + b"\x0b" + b"\x10" + one_code + b"\x00",
        )
    )
    output.extend(segment(0xDA, b"\x01\x01\x00\x00\x3f\x00"))

    entropy = bytearray()
    bit_buffer = 0
    bit_count = 0

    def emit_bits(value: int, count: int) -> None:
        nonlocal bit_buffer, bit_count
        bit_buffer = (bit_buffer << count) | value
        bit_count += count
        while bit_count >= 8:
            bit_count -= 8
            byte = (bit_buffer >> bit_count) & 0xFF
            entropy.append(byte)
            if byte == 0xFF:
                entropy.append(0)
        bit_buffer &= (1 << bit_count) - 1

    predictor = 0
    for dc in (-1024, 1016, -1024, 1016):
        difference = dc - predictor
        predictor = dc
        magnitude_bits = abs(difference).bit_length()
        if magnitude_bits != 11:
            raise AssertionError("split JPEG expects 11-bit DC differences")
        amplitude = difference if difference >= 0 else difference + (1 << magnitude_bits) - 1
        emit_bits(0, 1)  # The sole DC Huffman code selects category 11.
        emit_bits(amplitude, magnitude_bits)
        emit_bits(0, 1)  # The sole AC Huffman code is EOB.
    if bit_count:
        emit_bits((1 << (8 - bit_count)) - 1, 8 - bit_count)
    output.extend(entropy)
    output.extend(b"\xff\xd9")
    return bytes(output)


def extended_sequential_gray_split_jpeg() -> bytes:
    """Return the split image as a valid SOF1 extended-sequential JPEG."""

    baseline = baseline_gray_split_jpeg()
    frame_offset = baseline.index(b"\xff\xc0")
    return baseline[: frame_offset + 1] + b"\xc1" + baseline[frame_offset + 2 :]


def progressive_gray_jpeg() -> bytes:
    """Return a deterministic valid 16x16 progressive grayscale JPEG."""

    def segment(marker: int, payload: bytes) -> bytes:
        return b"\xff" + bytes((marker,)) + struct.pack(">H", len(payload) + 2) + payload

    output = bytearray(b"\xff\xd8")
    output.extend(segment(0xDB, b"\x00" + bytes((1,)) * 64))
    output.extend(
        segment(
            0xC2,
            b"\x08"
            + struct.pack(">HH", 16, 16)
            + b"\x01"
            + b"\x01\x11\x00",
        )
    )
    one_code = bytes((1,)) + bytes(15)
    output.extend(
        segment(
            0xC4,
            b"\x00" + one_code + b"\x00" + b"\x10" + one_code + b"\x00",
        )
    )
    output.extend(segment(0xDA, b"\x01\x01\x00\x00\x00\x00"))
    output.append(0x0F)  # Four zero-difference DC codes followed by one-bit padding.
    output.extend(segment(0xDA, b"\x01\x01\x00\x01\x3f\x00"))
    output.append(0x0F)  # Four EOB AC codes followed by one-bit padding.
    output.extend(b"\xff\xd9")
    return bytes(output)


class _DeflateBitWriter:
    def __init__(self) -> None:
        self.output = bytearray()
        self.bits = 0
        self.bit_count = 0

    def write(self, value: int, count: int) -> None:
        self.bits |= (value & ((1 << count) - 1)) << self.bit_count
        self.bit_count += count
        while self.bit_count >= 8:
            self.output.append(self.bits & 0xFF)
            self.bits >>= 8
            self.bit_count -= 8

    def finish(self) -> bytes:
        if self.bit_count:
            self.output.append(self.bits & 0xFF)
        return bytes(self.output)


def _reverse_bits(value: int, count: int) -> int:
    reversed_value = 0
    for _ in range(count):
        reversed_value = (reversed_value << 1) | (value & 1)
        value >>= 1
    return reversed_value


def _write_fixed_symbol(writer: _DeflateBitWriter, symbol: int) -> None:
    if 0 <= symbol <= 143:
        code, bits = 0x30 + symbol, 8
    elif symbol <= 255:
        code, bits = 0x190 + symbol - 144, 9
    elif symbol <= 279:
        code, bits = symbol - 256, 7
    elif symbol <= 287:
        code, bits = 0xC0 + symbol - 280, 8
    else:
        raise ValueError(f"invalid fixed-Huffman symbol {symbol}")
    writer.write(_reverse_bits(code, bits), bits)


def _write_fixed_match(writer: _DeflateBitWriter, length: int, distance: int) -> None:
    length_bases = (
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        13,
        15,
        17,
        19,
        23,
        27,
        31,
        35,
        43,
        51,
        59,
        67,
        83,
        99,
        115,
        131,
        163,
        195,
        227,
        258,
    )
    length_extras = (
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        1,
        1,
        1,
        1,
        2,
        2,
        2,
        2,
        3,
        3,
        3,
        3,
        4,
        4,
        4,
        4,
        5,
        5,
        5,
        5,
        0,
    )
    distance_bases = (
        1,
        2,
        3,
        4,
        5,
        7,
        9,
        13,
        17,
        25,
        33,
        49,
        65,
        97,
        129,
        193,
        257,
        385,
        513,
        769,
        1025,
        1537,
        2049,
        3073,
        4097,
        6145,
        8193,
        12289,
        16385,
        24577,
    )
    distance_extras = (
        0,
        0,
        0,
        0,
        1,
        1,
        2,
        2,
        3,
        3,
        4,
        4,
        5,
        5,
        6,
        6,
        7,
        7,
        8,
        8,
        9,
        9,
        10,
        10,
        11,
        11,
        12,
        12,
        13,
        13,
    )
    for index, base in enumerate(length_bases):
        extra_bits = length_extras[index]
        maximum = base + ((1 << extra_bits) - 1 if extra_bits else 0)
        if length <= maximum:
            _write_fixed_symbol(writer, 257 + index)
            if extra_bits:
                writer.write(length - base, extra_bits)
            break
    else:
        raise ValueError(f"invalid DEFLATE match length {length}")
    for code, base in enumerate(distance_bases):
        extra_bits = distance_extras[code]
        maximum = base + ((1 << extra_bits) - 1 if extra_bits else 0)
        if distance <= maximum:
            writer.write(_reverse_bits(code, 5), 5)
            if extra_bits:
                writer.write(distance - base, extra_bits)
            return
    raise ValueError(f"invalid DEFLATE match distance {distance}")


def deterministic_periodic_zlib(data: bytes, period: int) -> bytes:
    """Encode periodic bytes with an explicit, cross-zlib byte contract."""

    if not data or period <= 0 or period > min(len(data), 32768):
        raise ValueError("period must address a non-empty DEFLATE window")
    if data[period:] != data[:-period]:
        raise ValueError("input does not repeat at the declared period")

    # zlib's match heuristics vary between implementations. Emit one fixed-
    # Huffman block ourselves: the first period is literal, then every later
    # byte is represented by deterministic length/distance pairs.
    writer = _DeflateBitWriter()
    writer.write(1, 1)  # BFINAL
    writer.write(1, 2)  # BTYPE=01, fixed Huffman
    for value in data[:period]:
        _write_fixed_symbol(writer, value)
    offset = period
    while offset < len(data):
        match_length = min(258, len(data) - offset)
        if match_length >= 3:
            _write_fixed_match(writer, match_length, period)
            offset += match_length
        else:
            _write_fixed_symbol(writer, data[offset])
            offset += 1
    _write_fixed_symbol(writer, 256)

    adler_a = 1
    adler_b = 0
    for value in data:
        adler_a = (adler_a + value) % 65521
        adler_b = (adler_b + adler_a) % 65521
    return b"\x78\x01" + writer.finish() + struct.pack(">I", (adler_b << 16) | adler_a)


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


def make_navigation_outline() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(
        1,
        b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R "
        b"/Names << /Dests 20 0 R >> /PageLabels 25 0 R "
        b"/Lang (de-CH) /Metadata 31 0 R >>",
    )
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R "
        b"/Annots [40 0 R 41 0 R 42 0 R 43 0 R] >>",
    )
    page_one = (
        b"BT /F1 24 Tf 72 720 Td (Contents) Tj "
        b"/F1 12 Tf 0 -36 Td (Chapter One) Tj "
        b"0 -24 Td (Chapter Two) Tj ET"
    )
    pdf.add(4, stream(page_one))
    pdf.add(5, font_object())
    pdf.add(
        6,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>",
    )
    page_two = (
        b"BT /F1 24 Tf 72 720 Td (Chapter Two) Tj "
        b"/F1 12 Tf 0 -36 Td (Index) Tj "
        b"0 -24 Td (Chapter One) Tj ET"
    )
    pdf.add(7, stream(page_two))
    pdf.add(10, b"<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 3 >>")
    pdf.add(
        11,
        b"<< /Title (Part One) /Parent 10 0 R /First 12 0 R /Last 13 0 R "
        b"/Count 2 /Dest /part-one >>",
    )
    pdf.add(
        12,
        b"<< /Title (Chapter One) /Parent 11 0 R /Next 13 0 R "
        b"/Dest [3 0 R /XYZ null 720 null] >>",
    )
    pdf.add(
        13,
        b"<< /Title (Chapter Two) /Parent 11 0 R /Prev 12 0 R "
        b"/Dest (chapter-two) >>",
    )
    pdf.add(
        20,
        b"<< /Names [(chapter-two) [6 0 R /Fit] "
        b"(part-one) [3 0 R /Fit]] >>",
    )
    pdf.add(
        25,
        b"<< /Nums [0 << /S /r >> 1 << /S /D /P (A-) /St 1 >>] >>",
    )
    pdf.add(30, b"<< /Title (Info title) /Author (Info author) >>")
    xmp = (
        b"<?xpacket begin=\"\"?>"
        b"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        b"<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        b"<rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        b"<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">XMP Navigation</rdf:li>"
        b"</rdf:Alt></dc:title>"
        b"<dc:creator><rdf:Seq><rdf:li>XMP Author</rdf:li></rdf:Seq></dc:creator>"
        b"<dc:language><rdf:Bag><rdf:li>fr</rdf:li></rdf:Bag></dc:language>"
        b"</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>"
    )
    pdf.add(31, stream(xmp, b"/Type /Metadata /Subtype /XML"))
    pdf.add(
        40,
        b"<< /Type /Annot /Subtype /Link /Rect [70 645 180 675] "
        b"/Dest (chapter-two) >>",
    )
    pdf.add(
        41,
        b"<< /Type /Annot /Subtype /Link /Rect [70 670 180 700] "
        b"/Dest [3 0 R /XYZ null 720 null] >>",
    )
    pdf.add(
        42,
        b"<< /Type /Annot /Subtype /Link /Rect [200 645 300 675] "
        b"/A << /S /URI /URI (https://example.invalid/) >> >>",
    )
    pdf.add(
        43,
        b"<< /Type /Annot /Subtype /Link /Rect [200 670 300 700] "
        b"/A << /S /JavaScript /JS (app.alert\\(1\\)) >> >>",
    )
    rendered = pdf.render(trailer_entries=b"/Root 1 0 R /Info 30 0 R")[0]
    transcript = "Contents Chapter One Chapter Two Chapter Two Index Chapter One"
    outline = (
        {
            "title": "Part One",
            "level": 1,
            "section": 0,
            "anchor": "b00000000",
            "destination": "named:part-one",
        },
        {
            "title": "Chapter One",
            "level": 2,
            "section": 0,
            "anchor": "b00000000",
            "destination": "explicit:page-0",
        },
        {
            "title": "Chapter Two",
            "level": 2,
            "section": 1,
            "anchor": "b00000003",
            "destination": "named:chapter-two",
        },
    )
    links = (
        {
            "text": "Chapter One",
            "href": "sections/000000.xhtml#b00000000",
            "kind": "same-section",
        },
        {
            "text": "Chapter Two",
            "href": "sections/000001.xhtml#b00000003",
            "kind": "cross-section",
        },
        {"source_page": 0, "page_label": "i"},
        {"source_page": 1, "page_label": "A-1"},
        {"ignored_action": "URI"},
        {"ignored_action": "JavaScript"},
    )
    return Fixture(rendered, transcript, (), outline, links)


def make_many_outline_entries_fixture(count: int) -> Fixture:
    if count < 1 or count > 32:
        raise ValueError("outline preparation fixture count must be 1..32")
    pdf = ClassicPdf()
    pdf.add(
        1,
        b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>",
    )
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"BT /F1 18 Tf 72 720 Td "
            b"(Thirty two outline entries.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    first_object = 11
    last_object = first_object + count - 1
    pdf.add(
        10,
        (
            f"<< /Type /Outlines /First {first_object} 0 R "
            f"/Last {last_object} 0 R /Count {count} >>"
        ).encode("ascii"),
    )
    outline = []
    for index in range(count):
        object_number = first_object + index
        links = []
        if index != 0:
            links.append(f"/Prev {object_number - 1} 0 R")
        if index + 1 != count:
            links.append(f"/Next {object_number + 1} 0 R")
        title = f"Outline {index + 1:02d}"
        body = (
            f"<< /Title ({title}) /Parent 10 0 R "
            f"{' '.join(links)} /Dest [3 0 R /Fit] >>"
        )
        pdf.add(object_number, body.encode("ascii"))
        outline.append(
            {
                "title": title,
                "level": 1,
                "section": 0,
                "anchor": "b00000000",
                "destination": "explicit:page-0",
            }
        )
    transcript = "Thirty two outline entries."
    return Fixture(pdf.render()[0], transcript, (), tuple(outline))


def make_navigation_heading_fallback() -> Fixture:
    content = (
        b"BT /F1 24 Tf 72 720 Td (First Heading) Tj "
        b"/F1 12 Tf 0 -36 Td (Body text.) Tj "
        b"/F1 24 Tf 0 -48 Td (Second Heading) Tj "
        b"/F1 12 Tf 0 -36 Td (More body.) Tj ET"
    )
    transcript = "First Heading Body text. Second Heading More body."
    outline = (
        {
            "title": "First Heading",
            "level": 1,
            "section": 0,
            "anchor": "b00000000",
            "destination": "heading",
        },
        {
            "title": "Second Heading",
            "level": 1,
            "section": 1,
            "anchor": "b00000002",
            "destination": "heading",
        },
    )
    return Fixture(one_page_pdf(content), transcript, (), outline)


def make_navigation_root_fallback() -> Fixture:
    text = "Plain document without a reliable heading."
    content = b"BT /F1 12 Tf 72 720 Td (Plain document without a reliable heading.) Tj ET"
    outline = (
        {
            "title": "navigation_root_fallback",
            "level": 1,
            "section": 0,
            "anchor": "b00000000",
            "destination": "fallback",
        },
    )
    return Fixture(one_page_pdf(content), text, (), outline)


def make_navigation_outline_cycle() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(4, stream(b"BT /F1 12 Tf 72 720 Td (Cycle body.) Tj ET"))
    pdf.add(5, font_object())
    pdf.add(10, b"<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 1 >>")
    pdf.add(
        11,
        b"<< /Title (Loop) /Parent 10 0 R /Next 11 0 R "
        b"/Dest [3 0 R /Fit] >>",
    )
    return Fixture(pdf.render()[0], "Cycle body.", error="OutlineCycle")


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
        b"q 1 0 0 1 0 650 cm /Fm1 Do Q\n"
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


def make_jpeg_caption_fixture() -> Fixture:
    jpeg = base64.b64decode(JPEGDEC_BASELINE_GRAY_SPLIT_JPEG_BASE64, validate=True)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Figure 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 320 0 0 240 72 400 cm /Figure Do Q "
            b"BT /F1 12 Tf 72 370 Td (Figure caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            jpeg,
            b"/Type /XObject /Subtype /Image /Width 16 /Height 16 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Figure caption.",
        ("Figure caption.",),
        image_hashes=(("image-6-jpeg", hashlib.sha256(jpeg).hexdigest()),),
    )


def make_jpeg_cover_caption_fixture() -> Fixture:
    jpeg = baseline_gray_split_jpeg()
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Cover 6 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 440 0 0 440 72 250 cm /Cover Do Q "
            b"BT /F1 12 Tf 72 220 Td (JPEG cover caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            jpeg,
            b"/Type /XObject /Subtype /Image /Width 16 /Height 16 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "JPEG cover caption.",
        ("JPEG cover caption.",),
        image_hashes=(("image-6-cover-jpeg", hashlib.sha256(jpeg).hexdigest()),),
    )


def make_preview_unsupported_jpeg_cover_fixture(
    jpeg: bytes,
    caption: str,
    title: str,
    image_hash_name: str,
) -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Cover 6 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 440 0 0 440 72 250 cm /Cover Do Q "
            + b"BT /F1 12 Tf 72 220 Td ("
            + pdf_string(caption)
            + b") Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            jpeg,
            b"/Type /XObject /Subtype /Image /Width 16 /Height 16 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode",
        ),
    )
    pdf.add(
        7,
        b"<< /Title (" + pdf_string(title) + b") /Author (Task 22) >>",
    )
    return Fixture(
        pdf.render(trailer_entries=b"/Root 1 0 R /Info 7 0 R")[0],
        caption,
        (caption,),
        image_hashes=((image_hash_name, hashlib.sha256(jpeg).hexdigest()),),
    )


def make_progressive_jpeg_cover_caption_fixture() -> Fixture:
    return make_preview_unsupported_jpeg_cover_fixture(
        progressive_gray_jpeg(),
        "Progressive JPEG cover caption.",
        "Progressive Cover",
        "image-6-progressive-cover-jpeg",
    )


def make_sof1_jpeg_cover_caption_fixture() -> Fixture:
    return make_preview_unsupported_jpeg_cover_fixture(
        extended_sequential_gray_split_jpeg(),
        "SOF1 JPEG cover caption.",
        "SOF1 Cover",
        "image-6-sof1-cover-jpeg",
    )


def make_flate_gray_caption_fixture() -> Fixture:
    pixels = bytes((0x00, 0x55, 0xAA, 0xFF))
    encoded = zlib.compress(pixels, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Raster 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Raster Do Q "
            b"BT /F1 12 Tf 72 370 Td (Raster caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Raster caption.",
        ("Raster caption.",),
        image_hashes=(("image-6-flate-gray8", hashlib.sha256(pixels).hexdigest()),),
    )


def make_raster_cover_caption_fixture() -> Fixture:
    pixels = bytes(
        (
            0x00,
            0x00,
            0xFF,
            0xFF,
        )
        * 4
    )
    encoded = zlib.compress(pixels, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Cover 6 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 440 0 0 440 72 250 cm /Cover Do Q "
            b"BT /F1 12 Tf 72 220 Td (Image cover caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 4 /Height 4 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Image cover caption.",
        ("Image cover caption.",),
        image_hashes=(("image-6-cover-gray8", hashlib.sha256(pixels).hexdigest()),),
    )


def make_discarded_then_raster_cover_fixture() -> Fixture:
    tiny_pixels = bytes((0x00,))
    cover_pixels = bytes(
        (
            0x00,
            0x00,
            0xFF,
            0xFF,
        )
        * 4
    )
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> "
        b"/XObject << /Tiny 6 0 R /Cover 7 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 12 0 0 12 72 700 cm /Tiny Do Q "
            b"q 440 0 0 440 72 250 cm /Cover Do Q "
            b"BT /F1 12 Tf 72 220 Td (Retained cover caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            zlib.compress(tiny_pixels, level=9),
            b"/Type /XObject /Subtype /Image /Width 1 /Height 1 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    pdf.add(
        7,
        stream(
            zlib.compress(cover_pixels, level=9),
            b"/Type /XObject /Subtype /Image /Width 4 /Height 4 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Retained cover caption.",
        ("Retained cover caption.",),
        image_hashes=(
            ("image-6-tiny-gray8", hashlib.sha256(tiny_pixels).hexdigest()),
            ("image-7-cover-gray8", hashlib.sha256(cover_pixels).hexdigest()),
        ),
    )


def make_rotated_crop_raster_caption_fixture() -> Fixture:
    pixels = bytes(
        (
            0x00,
            0x00,
            0xFF,
            0xFF,
        )
        * 4
    )
    encoded = zlib.compress(pixels, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [10 20 610 820] "
        b"/CropBox [100 200 500 700] /Rotate 90 "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Cover 6 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 300 0 0 400 150 250 cm /Cover Do Q "
            b"BT /F1 12 Tf 150 220 Td (Rotated crop caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 4 /Height 4 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Rotated crop caption.",
        ("Rotated crop caption.",),
        image_hashes=(("image-6-rotated-crop-gray8", hashlib.sha256(pixels).hexdigest()),),
    )


def make_malformed_flate_caption_fixture() -> Fixture:
    pixels = bytes((0x00, 0x55, 0xAA, 0xFF))
    encoded = zlib.compress(pixels, level=9)[:-2]
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Raster 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Raster Do Q "
            b"BT /F1 12 Tf 72 370 Td (Malformed raster caption survives.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Malformed raster caption survives.",
        ("Malformed raster caption survives.",),
        warning="OptionalImageOmitted",
    )


def make_indexed_caption_fixture(fully_indirect: bool) -> Fixture:
    palette = bytes.fromhex("000000555555AAAAAAFFFFFF")
    encoded = zlib.compress(bytes((0x1B,)), level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 6 0 R >> /XObject << /Im0 5 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    caption = "Indirect indexed caption." if fully_indirect else "Direct indexed caption."
    pdf.add(
        4,
        stream(
            b"q 240 0 0 120 72 500 cm /Im0 Do Q "
            b"BT /F1 12 Tf 72 470 Td (" + pdf_string(caption) + b") Tj ET"
        ),
    )
    if fully_indirect:
        color_space = b"7 0 R"
        pdf.add(7, b"[/Indexed 8 0 R 3 9 0 R]")
        pdf.add(8, b"/DeviceRGB")
        pdf.add(9, b"<" + palette.hex().upper().encode("ascii") + b">")
    else:
        color_space = (
            b"[/Indexed /DeviceRGB 3 <"
            + palette.hex().upper().encode("ascii")
            + b">]"
        )
    pdf.add(
        5,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 4 /Height 1 /ColorSpace "
            + color_space
            + b" /BitsPerComponent 2 /Filter /FlateDecode",
        ),
    )
    pdf.add(6, font_object())
    return Fixture(pdf.render()[0], caption, (caption,))


def make_large_raster_caption_fixture() -> Fixture:
    width = 800
    height = 480
    state = 0x6D2B79F5
    # Keep the encoded source larger than one preparation slice while avoiding
    # a full 384 KiB random fixture. Repeating a deterministic 1 KiB block keeps
    # every bounded firmware inflate input productive and still expands to the
    # full device page.
    pattern = bytearray(1024)
    for index in range(len(pattern)):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        pattern[index] = state & 0xFF
    pixels = (pattern * ((width * height + len(pattern) - 1) // len(pattern)))[
        : width * height
    ]
    encoded = deterministic_periodic_zlib(bytes(pixels), len(pattern))
    mask_pixels = bytes(value ^ 0xFF for value in pixels)
    mask_encoded = deterministic_periodic_zlib(mask_pixels, len(pattern))
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Raster 6 0 R >> >> "
        b"/Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 440 0 0 264 72 360 cm /Raster Do Q "
            b"BT /F1 12 Tf 72 330 Td (Large raster caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 800 /Height 480 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode "
            b"/SMask 7 0 R",
        ),
    )
    pdf.add(
        7,
        stream(
            mask_encoded,
            b"/Type /XObject /Subtype /Image /Width 800 /Height 480 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(pdf.render()[0], "Large raster caption.", ("Large raster caption.",))


def make_unsupported_jpx_caption_fixture() -> Fixture:
    encoded = b"not-a-jpx-stream"
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Optional 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Optional Do Q "
            b"BT /F1 12 Tf 72 370 Td (Text survives.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /JPXDecode",
        ),
    )
    return Fixture(pdf.render()[0], "Text survives.", ("Text survives.",))


def make_soft_mask_caption_fixture() -> Fixture:
    base = zlib.compress(bytes((0x00, 0x55, 0xAA, 0xFF)), level=9)
    mask = zlib.compress(bytes((0xFF, 0x80, 0x00, 0xFF)), level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Masked 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Masked Do Q "
            b"BT /F1 12 Tf 72 370 Td (Masked caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            base,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /SMask 7 0 R",
        ),
    )
    pdf.add(
        7,
        stream(
            mask,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(pdf.render()[0], "Masked caption.", ("Masked caption.",))


def make_explicit_mask_caption_fixture() -> Fixture:
    base = zlib.compress(bytes((0x00, 0x55, 0xAA, 0xFF)), level=9)
    mask = zlib.compress(bytes((0x40, 0x80)), level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Masked 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Masked Do Q "
            b"BT /F1 12 Tf 72 370 Td (Explicit mask caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            base,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Mask 7 0 R",
        ),
    )
    pdf.add(
        7,
        stream(
            mask,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ImageMask true /BitsPerComponent 1 /Filter /FlateDecode",
        ),
    )
    return Fixture(pdf.render()[0], "Explicit mask caption.", ("Explicit mask caption.",))


def make_rejected_soft_mask_fixture(name: str, mask_dictionary: bytes) -> Fixture:
    base = zlib.compress(bytes((0x00, 0x55, 0xAA, 0xFF)), level=9)
    mask = zlib.compress(bytes((0xFF, 0x80, 0x00, 0xFF)), level=9)
    caption = f"{name} text survives."
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Masked 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 200 0 0 200 72 400 cm /Masked Do Q "
            b"BT /F1 12 Tf 72 370 Td (" + pdf_string(caption) + b") Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            base,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /SMask 7 0 R",
        ),
    )
    pdf.add(7, stream(mask, mask_dictionary))
    return Fixture(pdf.render()[0], caption, (caption,))


def make_mismatched_soft_mask_caption_fixture() -> Fixture:
    return make_rejected_soft_mask_fixture(
        "Mismatched mask",
        b"/Type /XObject /Subtype /Image /Width 1 /Height 2 "
        b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
    )


def make_cyclic_soft_mask_caption_fixture() -> Fixture:
    return make_rejected_soft_mask_fixture(
        "Cyclic mask",
        b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
        b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /SMask 6 0 R",
    )


def make_inline_image_caption_fixture() -> Fixture:
    # The raw row deliberately contains whitespace-EI-whitespace. A scanner
    # that ignores the declared unfiltered byte count will truncate it.
    pixels = bytes((0x00, 0x20, 0x45, 0x49, 0x20))
    content = (
        b"q 200 0 0 80 72 400 cm BI /W 5 /H 1 /CS /G /BPC 8 ID\n"
        + pixels
        + b"\nEI Q BT /F1 12 Tf 72 370 Td (Inline caption.) Tj ET"
    )
    return Fixture(
        one_page_pdf(content),
        "Inline caption.",
        ("Inline caption.",),
        image_hashes=(("inline-gray8", hashlib.sha256(pixels).hexdigest()),),
    )


def make_inline_indexed_decode_parms_caption_fixture() -> Fixture:
    palette = bytes.fromhex("000000555555AAAAAAFFFFFF")
    # TIFF predictor 2 differences for the four 2-bit samples [0, 1, 2, 3].
    # /Decode [3 0] then reverses the palette lookup to [3, 2, 1, 0].
    encoded = zlib.compress(bytes((0x15,)), level=9)
    content = (
        b"q 200 0 0 80 72 400 cm "
        b"BI /W 4 /H 1 /BPC 2 "
        b"/CS [/I /RGB 3 <"
        + palette.hex().upper().encode("ascii")
        + b">] /D [3 0] /F /Fl "
        b"/DP << /Predictor 2 /Colors 1 /BitsPerComponent 2 /Columns 4 >> ID\n"
        + encoded
        + b"\nEI Q BT /F1 12 Tf 72 370 Td "
        b"(Inline indexed predictor caption.) Tj ET"
    )
    return Fixture(
        one_page_pdf(content),
        "Inline indexed predictor caption.",
        ("Inline indexed predictor caption.",),
    )


def make_filtered_inline_boundary_fixture(filter_name: str) -> Fixture:
    if filter_name == "asciihex":
        pixels = b"\x10 EI \xF0"
        encoded = pixels.hex().upper().encode("ascii") + b">"
        filter_value = b"/AHx"
        color_space = b"/G"
        bits_per_component = 8
    elif filter_name == "ascii85":
        pixels = b""
        encoded = b""
        for value in range(1_000_000):
            candidate = value.to_bytes(4, "big")
            candidate_encoded = base64.a85encode(candidate, adobe=False)
            marker = candidate_encoded.find(b"EI")
            if marker >= 0:
                pixels = candidate
                encoded = (
                    candidate_encoded[:marker]
                    + b" "
                    + candidate_encoded[marker : marker + 2]
                    + b" "
                    + candidate_encoded[marker + 2 :]
                    + b"~>"
                )
                break
        if not pixels:
            raise AssertionError("failed to construct ASCII85 EI boundary fixture")
        filter_value = b"/A85"
        color_space = b"/G"
        bits_per_component = 8
    elif filter_name == "flate":
        pixels = b"\x01 EI \x02\x03\x04"
        encoded = zlib.compress(pixels, level=0)
        if b" EI " not in encoded:
            raise AssertionError("stored Flate fixture lost embedded EI bytes")
        filter_value = b"/Fl"
        color_space = b"/G"
        bits_per_component = 8
    elif filter_name == "dct":
        pixels = bytes(
            (
                0xFF,
                0xD8,
                0xFF,
                0xFE,
                0x00,
                0x06,
                0x20,
                0x45,
                0x49,
                0x20,
                0xFF,
                0xD9,
            )
        )
        encoded = pixels
        filter_value = b"/DCT"
        color_space = b"/RGB"
        bits_per_component = 8
    else:
        raise ValueError(filter_name)
    caption = f"Inline {filter_name} boundary survives."
    content = (
        b"q 200 0 0 80 72 400 cm BI /W "
        + str(len(pixels)).encode("ascii")
        + b" /H 1 /CS "
        + color_space
        + b" /BPC "
        + str(bits_per_component).encode("ascii")
        + b" /F "
        + filter_value
        + b" ID\n"
        + encoded
        + b"\nEI Q BT /F1 12 Tf 72 370 Td ("
        + pdf_string(caption)
        + b") Tj ET"
    )
    return Fixture(one_page_pdf(content), caption, (caption,))


def make_inline_dct_one_pass_fixture() -> Fixture:
    # A valid baseline JPEG with a deterministic APP1 segment large enough to
    # cross several 4 KiB source windows. The APP payload deliberately contains
    # no FF D9 pair, so the JPEG's final two bytes remain the unambiguous
    # inline-image boundary.
    app_payload = bytearray()
    state = 0xA341316C
    for _ in range(20 * 1024):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        byte = state & 0xFF
        if app_payload and app_payload[-1] == 0xFF and byte == 0xD9:
            byte = 0xD8
        app_payload.append(byte)
    app_length = len(app_payload) + 2
    baseline = baseline_gray_split_jpeg()
    encoded = (
        baseline[:2]
        + b"\xFF\xE1"
        + app_length.to_bytes(2, "big")
        + bytes(app_payload)
        + baseline[2:]
    )
    caption = "Inline DCT one pass."
    content = (
        b"q 200 0 0 80 72 400 cm "
        b"BI /W 16 /H 16 /CS /G /BPC 8 /F /DCT ID\n"
        + encoded
        + b"\nEI Q BT /F1 12 Tf 72 370 Td ("
        + pdf_string(caption)
        + b") Tj ET"
    )
    return Fixture(one_page_pdf(content), caption, (caption,))


def make_repeated_logo_caption_fixture() -> Fixture:
    encoded = zlib.compress(bytes((0x00, 0x55, 0xAA, 0xFF)), level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << /Logo 6 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 40 0 0 40 72 600 cm /Logo Do Q "
            b"q 40 0 0 40 144 600 cm /Logo Do Q "
            b"q 40 0 0 40 216 600 cm /Logo Do Q "
            b"BT /F1 12 Tf 72 550 Td (Repeated logo caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(pdf.render()[0], "Repeated logo caption.", ("Repeated logo caption.",))


def make_mixed_form_image_caption_fixture() -> Fixture:
    encoded = zlib.compress(bytes((0x00, 0x55, 0xAA, 0xFF)), level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> "
        b"/XObject << /Vector 6 0 R /Figure 7 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 1 0 0 1 72 620 cm /Vector Do Q "
            b"q 200 0 0 200 72 400 cm /Figure Do Q "
            b"BT /F1 12 Tf 72 370 Td (Mixed resource caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            b"0 0 80 20 re S",
            b"/Type /XObject /Subtype /Form /BBox [0 0 80 20] "
            b"/Resources << >>",
        ),
    )
    pdf.add(
        7,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Mixed resource caption.",
        ("Mixed resource caption.",),
        image_hashes=(("image-7-flate-gray8", hashlib.sha256(bytes((0x00, 0x55, 0xAA, 0xFF))).hexdigest()),),
    )


def make_three_figures_one_page_fixture() -> Fixture:
    pixel_sets = (
        bytes((0x00, 0x55, 0xAA, 0xFF)),
        bytes((0xFF, 0xAA, 0x55, 0x00)),
        bytes((0x00, 0x00, 0xFF, 0xFF)),
    )
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << "
        b"/FigureA 6 0 R /FigureB 7 0 R /FigureC 8 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 140 0 0 100 72 600 cm /FigureA Do Q "
            b"BT /F1 12 Tf 72 580 Td (First figure caption.) Tj ET "
            b"q 140 0 0 100 72 420 cm /FigureB Do Q "
            b"BT /F1 12 Tf 72 400 Td (Second figure caption.) Tj ET "
            b"q 140 0 0 100 72 240 cm /FigureC Do Q "
            b"BT /F1 12 Tf 72 220 Td (Third figure caption.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    for object_number, pixels in zip((6, 7, 8), pixel_sets, strict=True):
        pdf.add(
            object_number,
            stream(
                zlib.compress(pixels, level=9),
                b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
            ),
        )
    transcript = "First figure caption. Second figure caption. Third figure caption."
    return Fixture(
        pdf.render()[0],
        transcript,
        ("First figure caption.", "Second figure caption.", "Third figure caption."),
        image_hashes=tuple(
            (f"image-{object_number}-flate-gray8", hashlib.sha256(pixels).hexdigest())
            for object_number, pixels in zip((6, 7, 8), pixel_sets, strict=True)
        ),
    )


def make_duplicate_raster_figures_fixture() -> Fixture:
    pixels = bytes((0x00, 0x55, 0xAA, 0xFF))
    encoded = zlib.compress(pixels, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << "
        b"/FigureA 6 0 R /FigureB 7 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 180 0 0 120 72 560 cm /FigureA Do Q "
            b"BT /F1 12 Tf 72 540 Td (First duplicate figure.) Tj ET "
            b"q 180 0 0 120 72 300 cm /FigureB Do Q "
            b"BT /F1 12 Tf 72 280 Td (Second duplicate figure.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    for object_number in (6, 7):
        pdf.add(
            object_number,
            stream(
                encoded,
                b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
            ),
        )
    return Fixture(
        pdf.render()[0],
        "First duplicate figure. Second duplicate figure.",
        ("First duplicate figure.", "Second duplicate figure."),
        image_hashes=tuple(
            (
                f"image-{object_number}-flate-gray8",
                hashlib.sha256(pixels).hexdigest(),
            )
            for object_number in (6, 7)
        ),
    )


def make_same_bytes_different_raster_contract_fixture() -> Fixture:
    pixels = bytes((0x00, 0x55, 0xAA, 0xFF))
    encoded = zlib.compress(pixels, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << "
        b"/Normal 6 0 R /Inverted 7 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 180 0 0 120 72 560 cm /Normal Do Q "
            b"BT /F1 12 Tf 72 540 Td (Normal contract figure.) Tj ET "
            b"q 180 0 0 120 72 300 cm /Inverted Do Q "
            b"BT /F1 12 Tf 72 280 Td (Inverted contract figure.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    pdf.add(
        6,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    pdf.add(
        7,
        stream(
            encoded,
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Decode [1 0] "
            b"/Filter /FlateDecode",
        ),
    )
    return Fixture(
        pdf.render()[0],
        "Normal contract figure. Inverted contract figure.",
        ("Normal contract figure.", "Inverted contract figure."),
        image_hashes=(
            ("image-6-normal-gray8", hashlib.sha256(pixels).hexdigest()),
            ("image-7-inverted-gray8", hashlib.sha256(pixels).hexdigest()),
        ),
    )


def make_image_mask_paint_contract_fixture() -> Fixture:
    stencil = bytes((0xA0,))
    encoded = zlib.compress(stencil, level=9)
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> /XObject << "
        b"/GrayStencil 6 0 R /BlackStencil 7 0 R >> >> /Contents 4 0 R >>",
    )
    pdf.add(
        4,
        stream(
            b"q 0.5 g 180 0 0 120 72 560 cm /GrayStencil Do Q "
            b"BT /F1 12 Tf 72 540 Td (Gray stencil figure.) Tj ET "
            b"q 180 0 0 120 72 300 cm /BlackStencil Do Q "
            b"BT /F1 12 Tf 72 280 Td (Black stencil figure.) Tj ET"
        ),
    )
    pdf.add(5, font_object())
    for object_number in (6, 7):
        pdf.add(
            object_number,
            stream(
                encoded,
                b"/Type /XObject /Subtype /Image /Width 4 /Height 1 "
                b"/ImageMask true /BitsPerComponent 1 /Filter /FlateDecode",
            ),
        )
    return Fixture(
        pdf.render()[0],
        "Gray stencil figure. Black stencil figure.",
        ("Gray stencil figure.", "Black stencil figure."),
        image_hashes=(
            ("image-6-gray-stencil", hashlib.sha256(stencil).hexdigest()),
            ("image-7-black-stencil", hashlib.sha256(stencil).hexdigest()),
        ),
    )


def make_ten_page_figures_with_repeated_header_fixture() -> Fixture:
    pdf = ClassicPdf()
    page_objects = [3 + page * 3 for page in range(10)]
    content_objects = [4 + page * 3 for page in range(10)]
    image_objects = [5 + page * 3 for page in range(10)]
    header_object = 34
    font_number = 35
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    kids = b" ".join(f"{number} 0 R".encode("ascii") for number in page_objects)
    pdf.add(2, b"<< /Type /Pages /Kids [" + kids + b"] /Count 10 >>")
    header_pixels = bytes((0x00, 0xFF, 0xFF, 0x00))
    for page_index, (page_object, content_object, image_object) in enumerate(
        zip(page_objects, content_objects, image_objects, strict=True)
    ):
        caption = f"Unique figure {page_index + 1}."
        pdf.add(
            page_object,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            + b"/Resources << /Font << /F1 "
            + f"{font_number} 0 R".encode("ascii")
            + b" >> /XObject << /Header "
            + f"{header_object} 0 R".encode("ascii")
            + b" /Figure "
            + f"{image_object} 0 R".encode("ascii")
            + b" >> >> /Contents "
            + f"{content_object} 0 R".encode("ascii")
            + b" >>",
        )
        pdf.add(
            content_object,
            stream(
                b"q 60 0 0 30 276 744 cm /Header Do Q "
                b"q 220 0 0 160 72 430 cm /Figure Do Q "
                b"BT /F1 12 Tf 72 400 Td (" + pdf_string(caption) + b") Tj ET"
            ),
        )
        pixels = bytes(
            (
                page_index * 20,
                255 - page_index * 20,
                page_index * 11,
                255 - page_index * 11,
            )
        )
        pdf.add(
            image_object,
            stream(
                zlib.compress(pixels, level=9),
                b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
            ),
        )
    pdf.add(
        header_object,
        stream(
            zlib.compress(header_pixels, level=9),
            b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
        ),
    )
    pdf.add(font_number, font_object())
    captions = tuple(f"Unique figure {page + 1}." for page in range(10))
    return Fixture(pdf.render()[0], " ".join(captions), captions)


def make_many_unique_figures_fixture(image_count: int) -> Fixture:
    images_per_page = 8
    page_count = (image_count + images_per_page - 1) // images_per_page
    pdf = ClassicPdf()
    page_objects = [3 + page for page in range(page_count)]
    content_objects = [3 + page_count + page for page in range(page_count)]
    first_image_object = 3 + page_count * 2
    image_objects = [first_image_object + index for index in range(image_count)]
    font_number = first_image_object + image_count
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    kids = b" ".join(f"{number} 0 R".encode("ascii") for number in page_objects)
    pdf.add(
        2,
        b"<< /Type /Pages /Kids ["
        + kids
        + b"] /Count "
        + str(page_count).encode("ascii")
        + b" >>",
    )
    captions: list[str] = []
    for page_index, (page_object, content_object) in enumerate(
        zip(page_objects, content_objects, strict=True)
    ):
        first = page_index * images_per_page
        last = min(image_count, first + images_per_page)
        resources = []
        operators = []
        for image_index in range(first, last):
            name = f"Figure{image_index + 1}"
            caption = f"Bounded figure {image_index + 1}."
            captions.append(caption)
            resources.append(
                f"/{name} {image_objects[image_index]} 0 R".encode("ascii")
            )
            y = 640 - (image_index - first) * 70
            operators.append(
                f"q 180 0 0 44 72 {y} cm /{name} Do Q "
                f"BT /F1 12 Tf 276 {y + 14} Td ({caption}) Tj ET".encode("ascii")
            )
        pdf.add(
            page_object,
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            + b"/Resources << /Font << /F1 "
            + f"{font_number} 0 R".encode("ascii")
            + b" >> /XObject << "
            + b" ".join(resources)
            + b" >> >> /Contents "
            + f"{content_object} 0 R".encode("ascii")
            + b" >>",
        )
        pdf.add(content_object, stream(b" ".join(operators)))
    for image_index, image_object in enumerate(image_objects):
        pixels = bytes(
            (
                (image_index * 3 + 1) & 0xFF,
                (image_index * 5 + 2) & 0xFF,
                (image_index * 7 + 3) & 0xFF,
                (image_index * 11 + 4) & 0xFF,
            )
        )
        pdf.add(
            image_object,
            stream(
                zlib.compress(pixels, level=9),
                b"/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                b"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
            ),
        )
    pdf.add(font_number, font_object())
    return Fixture(pdf.render()[0], " ".join(captions), tuple(captions))


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
    pdf.add(
        2,
        b"<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 "
        b"/MediaBox [10 20 610 820] /CropBox [-20 40 650 780] "
        b"/Rotate -90 /Resources 10 0 R >>",
    )
    pdf.add(
        3,
        b"<< /Type /Page /Parent 2 0 R /Contents [4 0 R 5 0 R] >>",
    )
    pdf.add(4, stream(b"BT /F1 12 Tf 72 720 Td (First A) Tj ET"))
    pdf.add(5, stream(b"BT /F1 12 Tf 0 -24 Td (First B) Tj ET"))
    pdf.add(
        6,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 500] "
        b"/CropBox [50 -20 350 400] /Rotate 450 /Contents 7 0 R >>",
    )
    pdf.add(7, stream(b"BT /F1 12 Tf 72 720 Td (Second) Tj ET"))
    pdf.add(8, font_object())
    pdf.add(10, b"<< /Font << /F1 8 0 R >> >>")
    return Fixture(
        pdf.render()[0],
        "First A First B Second",
        ("First A", "First B", "Second"),
    )


def make_page_tree_geometry() -> Fixture:
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(
        2,
        b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R] /Count 4 "
        b"/MediaBox [0 0 200 100] /Rotate 360 >>",
    )
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R >>")
    pdf.add(4, b"<< /Type /Page /Parent 2 0 R /Rotate 90 >>")
    pdf.add(
        5,
        b"<< /Type /Page /Parent 2 0 R /CropBox [50 -10 250 80] "
        b"/Rotate -180 >>",
    )
    pdf.add(
        6,
        b"<< /Type /Page /Parent 2 0 R /CropBox [300 300 400 400] "
        b"/Rotate -90 >>",
    )
    return Fixture(pdf.render()[0], "")


def make_page_tree_bad_geometry(kind: str) -> Fixture:
    geometry = {
        "media_box": b"/MediaBox [0 0 200 /Bad]",
        "crop_box": b"/MediaBox [0 0 200 100] /CropBox [0 0 100]",
        "rotate": b"/MediaBox [0 0 200 100] /Rotate 45",
    }[kind]
    pdf = ClassicPdf()
    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(
        2,
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 " + geometry + b" >>",
    )
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R >>")
    return Fixture(pdf.render()[0], "", error="MalformedPageGeometry")


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
        "navigation_heading_fallback": make_navigation_heading_fallback(),
        "navigation_outline": make_navigation_outline(),
        "navigation_outline_32": make_many_outline_entries_fixture(32),
        "navigation_outline_cycle": make_navigation_outline_cycle(),
        "navigation_root_fallback": make_navigation_root_fallback(),
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
        "jpeg_caption": make_jpeg_caption_fixture(),
        "jpeg_cover_caption": make_jpeg_cover_caption_fixture(),
        "progressive_jpeg_cover_caption": make_progressive_jpeg_cover_caption_fixture(),
        "sof1_jpeg_cover_caption": make_sof1_jpeg_cover_caption_fixture(),
        "flate_gray_caption": make_flate_gray_caption_fixture(),
        "raster_cover_caption": make_raster_cover_caption_fixture(),
        "discarded_then_raster_cover": make_discarded_then_raster_cover_fixture(),
        "rotated_crop_raster_caption": make_rotated_crop_raster_caption_fixture(),
        "malformed_flate_caption": make_malformed_flate_caption_fixture(),
        "direct_indexed_caption": make_indexed_caption_fixture(False),
        "fully_indirect_indexed_caption": make_indexed_caption_fixture(True),
        "large_raster_caption": make_large_raster_caption_fixture(),
        "unsupported_jpx_caption": make_unsupported_jpx_caption_fixture(),
        "soft_mask_caption": make_soft_mask_caption_fixture(),
        "explicit_mask_caption": make_explicit_mask_caption_fixture(),
        "mismatched_soft_mask_caption": make_mismatched_soft_mask_caption_fixture(),
        "cyclic_soft_mask_caption": make_cyclic_soft_mask_caption_fixture(),
        "inline_image_caption": make_inline_image_caption_fixture(),
        "inline_indexed_decode_parms_caption": make_inline_indexed_decode_parms_caption_fixture(),
        "inline_asciihex_boundary": make_filtered_inline_boundary_fixture("asciihex"),
        "inline_ascii85_boundary": make_filtered_inline_boundary_fixture("ascii85"),
        "inline_flate_boundary": make_filtered_inline_boundary_fixture("flate"),
        "inline_dct_boundary": make_filtered_inline_boundary_fixture("dct"),
        "inline_dct_one_pass": make_inline_dct_one_pass_fixture(),
        "repeated_logo_caption": make_repeated_logo_caption_fixture(),
        "mixed_form_image_caption": make_mixed_form_image_caption_fixture(),
        "three_figures_one_page": make_three_figures_one_page_fixture(),
        "duplicate_raster_figures": make_duplicate_raster_figures_fixture(),
        "same_bytes_different_raster_contract": make_same_bytes_different_raster_contract_fixture(),
        "image_mask_paint_contract": make_image_mask_paint_contract_fixture(),
        "ten_page_figures_repeated_header": make_ten_page_figures_with_repeated_header_fixture(),
        "sixty_four_unique_figures": make_many_unique_figures_fixture(64),
        "sixty_five_unique_figures": make_many_unique_figures_fixture(65),
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
        "page_tree_geometry": make_page_tree_geometry(),
        "page_tree_bad_media_box": make_page_tree_bad_geometry("media_box"),
        "page_tree_bad_crop_box": make_page_tree_bad_geometry("crop_box"),
        "page_tree_bad_rotate": make_page_tree_bad_geometry("rotate"),
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
    staged_navigation: Path | None = None,
    staged_positive: dict[str, Path] | None = None,
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
        if staged_navigation is not None and (
            not staged_navigation.is_file()
            or staged_navigation.read_bytes() != files["navigation_outline.pdf"]
        ):
            differences.append("qemu/navigation_outline.pdf")
        for name, staged in (staged_positive or {}).items():
            if not staged.is_file() or staged.read_bytes() != files[name]:
                differences.append(f"qemu/{name}")
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
            QEMU_NAVIGATION_OUTPUT if stage_qemu else None,
            QEMU_POSITIVE_OUTPUTS if stage_qemu else None,
        )
    write_files(arguments.output, files)
    if stage_qemu:
        QEMU_CLASSIC_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        QEMU_CLASSIC_OUTPUT.write_bytes(files["classic_text.pdf"])
        QEMU_NAVIGATION_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        QEMU_NAVIGATION_OUTPUT.write_bytes(files["navigation_outline.pdf"])
        for name, destination in QEMU_POSITIVE_OUTPUTS.items():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(files[name])
    print(f"Wrote {len(files) - 1} PDF fixture artifacts to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
