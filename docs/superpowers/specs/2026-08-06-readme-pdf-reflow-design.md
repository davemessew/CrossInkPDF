# README PDF Reflow Rewrite Design

## Purpose

Present this fork as CrossInk with a reading-first PDF model: supported PDFs are
processed entirely on the device and read with the same typography and flow as
an EPUB, rather than displayed as miniature fixed pages.

## Audience

The README serves Xteink owners deciding whether to install the firmware,
existing CrossInk users comparing the fork with upstream, and contributors who
need a concise map to the detailed documentation.

## Content Structure

1. Open in the personal, practical style of the existing CrossInk README: this
   fork keeps the CrossInk reading experience and adds PDFs that behave like
   EPUBs on a small e-ink display.
2. Put a prominent firmware download callout directly beneath the introduction,
   linking both the `.bin` file and its GitHub Release page.
3. Explain the model in plain language: on-device text extraction, device fonts
   and layout, preserved navigation and supported images, word-based progress,
   and reusable SD-card preparation data.
4. State the practical PDF boundary: born-digital or OCRed PDFs with selectable
   text are supported; image-only scans, encrypted files, and classical page
   rendering are not.
5. Explain safe behavior under damaged input, insufficient memory, or failed
   preparation: close the PDF and leave the rest of the device usable.
6. Summarize the existing CrossInk reading, typography, statistics, controls,
   sync, and library features without reproducing the current long list.
7. Explain the normal-font `tiny` build and use GitHub Releases as the only
   distribution mechanism; a GitHub Package is unnecessary for firmware.
8. Retain hardware/resource guidance, documentation links, development entry
   points, and clear credit to CrossInk and CrossPoint Reader.

## Presentation

- Keep the existing README's simple Markdown and small amount of HTML rather
  than introducing a badge wall or a generic product-site layout.
- Use a strong title, a short first-person introduction, and a visually distinct
  download callout above the fold.
- Do not use screenshots from CrossInk or the upstream projects. Build the page's
  visual hierarchy with native Markdown: a download callout, short sections,
  bullets, and one compact comparison table.
- Keep the page useful on both desktop and mobile GitHub views without relying
  on fixed-width layout tricks.

## Copy Rules

- Use the original CrossInk README's personal, direct voice and concrete detail.
- Avoid generic marketing phrases, repetitive slogans, and language that sounds
  machine-generated.
- Do not use internal development, test-ledger, or release-process language.
- Do not discuss QEMU, internal validation, or physical-test status in the
  README.
- Do not imply that every PDF can be reflowed or that arbitrary processor faults
  can be recovered.
- Make clear that no desktop conversion or upload-time preprocessing is needed.
- Keep the README skimmable and move technical detail to existing docs.

## Acceptance Criteria

- A new reader can explain the PDF concept after reading the opening sections.
- The README covers typography, navigation, images, progress, caching, resource
  limits, and fail-closed behavior without presenting an engineering changelog.
- Existing EPUB behavior and major CrossInk capabilities remain visible.
- The direct `.bin` download and `pdf-reflow-f349288a` Release page are prominent
  near the top, and installation does not mention GitHub Packages.
- All local documentation links resolve and public copy contains no placeholders
  or internal process notes.
