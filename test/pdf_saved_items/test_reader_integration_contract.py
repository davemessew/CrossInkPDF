from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
SECTION_HEADER = (ROOT / "lib/Epub/Epub/Section.h").read_text(encoding="utf-8")
PAGE_HEADER = (ROOT / "lib/Epub/Epub/Page.h").read_text(encoding="utf-8")
PAGE_SOURCE = (ROOT / "lib/Epub/Epub/Page.cpp").read_text(encoding="utf-8")
IMAGE_SOURCE = (ROOT / "lib/Epub/Epub/blocks/ImageBlock.cpp").read_text(
    encoding="utf-8"
)
CLIP_HEADER = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text(
    encoding="utf-8"
)
CLIP_SOURCE = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text(
    encoding="utf-8"
)


def test_pdf_reader_keeps_all_pdf_session_state_behind_one_pointer():
    assert "struct PdfReaderSessionState;" in HEADER
    assert "std::unique_ptr<PdfReaderSessionState> pdfReaderSession" in HEADER
    assert "std::unique_ptr<PdfSavedItem[]> pdfSavedItemStorage" not in HEADER
    assert "PdfSavedItemsSession pdfSavedItemsSession" not in HEADER
    assert "allocatePdfReaderSessionState<PdfReaderSessionState>" in SOURCE
    assert "makeUniqueNoThrow<PdfSavedItem[]>" not in SOURCE


def test_pdf_reader_session_oom_fails_entry_with_recoverable_memory_alert():
    on_enter = SOURCE[
        SOURCE.index("void EpubReaderActivity::onEnter()") :
        SOURCE.index("void EpubReaderActivity::onExit()")
    ]
    allocation = on_enter.index(
        "allocatePdfReaderSessionState<PdfReaderSessionState>"
    )
    capture_settings = on_enter.index("captureGlobalReaderSettings()")
    oom = on_enter[allocation:capture_settings]
    assert "documentFormat == ReflowDocumentFormat::Pdf && !pdfReaderSession" in oom
    assert "STR_MEMORY_ERROR" in oom
    assert "STR_PDF_INSUFFICIENT_MEMORY" in oom
    assert "pendingAlertGoHomeOnBack.store(true" in oom
    assert "hasPendingAlert.store(true" in oom
    assert "finish();" in oom
    assert "return;" in oom

    on_exit = SOURCE[SOURCE.index("void EpubReaderActivity::onExit()") :]
    failed_entry_cleanup = on_exit[: on_exit.index("mappedInput.setReaderMode(false)")]
    assert "document->getFormat() == ReflowDocumentFormat::Pdf" in failed_entry_cleanup
    assert "!pdfReaderSession" in failed_entry_cleanup
    assert "document.reset()" in failed_entry_cleanup
    assert "return;" in failed_entry_cleanup


def test_pdf_legacy_stores_load_before_session_and_epub_skips_pdf_callbacks():
    on_enter = SOURCE[SOURCE.index("void EpubReaderActivity::onEnter()") :]
    load_bookmarks = on_enter.index("BOOKMARKS.loadForBook")
    load_clippings = on_enter.index("CLIPPINGS.loadForBook")
    initialize_session = on_enter.index("initializePdfSavedItems()")
    assert load_bookmarks < initialize_session
    assert load_clippings < initialize_session
    assert "pdfState.savedItemsSession.begin" in SOURCE
    assert "if (document->getFormat() == ReflowDocumentFormat::Pdf)" in SOURCE
    assert "bool EpubReaderActivity::supportsSavedItems() const" in SOURCE


def test_saved_items_flush_before_legacy_unload_and_storage_release():
    on_exit = SOURCE[SOURCE.index("void EpubReaderActivity::onExit()") :]
    flush = on_exit.index("pdfReaderSession->savedItemsSession.flush()")
    unload = on_exit.index("BOOKMARKS.unload()")
    section_release = on_exit.index("section.reset()")
    release = on_exit.index("pdfReaderSession.reset()")
    assert flush < unload < section_release < release


def test_pdf_pixel_cache_workspace_is_session_owned_and_not_global():
    session_state = SOURCE[
        SOURCE.index("struct EpubReaderActivity::PdfReaderSessionState") :
        SOURCE.index("EpubReaderActivity::EpubReaderActivity")
    ]
    assert "PdfPixelCacheRenderWorkspace pixelCacheRenderWorkspace" in session_state
    assert "sizeof(PdfReaderSessionState) == 11648" in SOURCE
    assert "pdfPixelCacheReadBuffer" not in IMAGE_SOURCE
    assert "pdfPixelCachePath" not in IMAGE_SOURCE
    assert "pdfPixelCacheWorkspaceInUse" not in IMAGE_SOURCE


def test_pdf_pixel_cache_workspace_propagates_through_every_reader_image_pass():
    assert "PdfPixelCacheRenderWorkspace* pdfWorkspace" in PAGE_HEADER
    assert "if (pdfWorkspace != nullptr && element->getTag() == TAG_PageImage)" in PAGE_SOURCE
    assert "imageBlock->render(renderer, xPos + xOffset, yPos + yOffset, pdfWorkspace)" in PAGE_SOURCE

    render_contents = SOURCE[
        SOURCE.index("void EpubReaderActivity::renderContents") :
        SOURCE.index("void EpubReaderActivity::renderStatusBar")
    ]
    assert "&pdfReaderSession->pixelCacheRenderWorkspace" in render_contents
    assert (
        "page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, "
        "foregroundBlack, pdfWorkspace)"
    ) in render_contents
    assert (
        "page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop, "
        "pdfWorkspace)"
    ) in render_contents
    assert "needsImageGrayscale, pdfWorkspace, tiledTimings" in render_contents

    tiled_pass = SOURCE[
        SOURCE.index("bool runTiledGrayscalePass") :
        SOURCE.index("ToastRect computeToastRect")
    ]
    assert "PdfPixelCacheRenderWorkspace* const pdfWorkspace" in tiled_pass
    assert "foregroundBlack, pdfWorkspace" in tiled_pass
    assert "marginTop, pdfWorkspace" in tiled_pass


def test_clip_page_switch_reuses_the_live_parent_pdf_workspace():
    assert "PdfPixelCacheRenderWorkspace* pdfRenderWorkspace" in CLIP_HEADER
    assert "parent EpubReaderActivity remains on the activity stack" in CLIP_HEADER

    start_clip = SOURCE[
        SOURCE.index("void EpubReaderActivity::startClipSelection()") :
        SOURCE.index("void EpubReaderActivity::resetReadingPaceData()")
    ]
    assert "&pdfReaderSession->pixelCacheRenderWorkspace" in start_clip
    assert "std::make_unique<ClipSelectionActivity>" in start_clip
    assert "pdfRenderWorkspace" in start_clip

    switch_page = CLIP_SOURCE[
        CLIP_SOURCE.index("bool ClipSelectionActivity::switchToPage") :
        CLIP_SOURCE.index("void ClipSelectionActivity::applyWordStyle")
    ]
    render_calls = re.findall(r"page->render\([^;]+?\);", switch_page, re.DOTALL)
    assert len(render_calls) == 3
    assert all("pdfRenderWorkspace" in line for line in render_calls)
    assert "makeUniqueNoThrow" not in switch_page


def test_pdf_reader_uses_the_section_cache_identity_for_semantic_saved_items():
    assert "getCacheFilePath() const" in SECTION_HEADER
    assert "pdfSavedItemsLayoutFingerprint" in SOURCE
    assert "mapPdfSavedItem" in SOURCE


def test_pdf_bookmark_mutations_flow_through_the_semantic_session():
    bookmark_case = SOURCE[
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE") :
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS")
    ]
    assert "PdfSavedItemKind::Bookmark" in bookmark_case
    assert "pdfReaderSession->savedItemsSession.add" in bookmark_case
    assert "pdfReaderSession->savedItemsSession.remove" in bookmark_case

    clear_case = SOURCE[
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS") :
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN")
    ]
    assert "pdfReaderSession->savedItemsSession.clear(PdfSavedItemKind::Bookmark)" in clear_case


def test_pdf_bookmark_marker_menu_and_toggle_use_cached_ordinal_containment():
    bookmark_lookup = SOURCE[
        SOURCE.index("const ReflowPageSemanticRange* EpubReaderActivity::currentPdfPageSemanticRange() const") :
        SOURCE.index("bool EpubReaderActivity::supportsSavedItems() const")
    ]
    assert "currentPageSemanticRange" in bookmark_lookup
    assert "semanticRangeSectionIndex" in bookmark_lookup
    assert "semanticRangePageNumber" in bookmark_lookup
    assert "findBookmarkInPage" in bookmark_lookup
    assert "mapPdfSavedItem" not in bookmark_lookup

    bookmark_case = SOURCE[
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE") :
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS")
    ]
    assert "currentPdfPageBookmark()" in bookmark_case
    assert "startGlobalWordOrdinal ==" not in bookmark_case
    assert "getSemanticRangeForPage" not in bookmark_case

    menu = SOURCE[
        SOURCE.index("void EpubReaderActivity::openReaderMenu()") :
        SOURCE.index("void EpubReaderActivity::loop()")
    ]
    status = SOURCE[
        SOURCE.index("void EpubReaderActivity::renderStatusBar() const") :
        SOURCE.index("bool EpubReaderActivity::shouldUseFootnotePreview")
    ]
    assert "currentPdfPageBookmark()" in menu
    assert "currentPdfPageBookmark()" in status


def test_pdf_mutation_failures_reload_instead_of_accepting_save_pending():
    assert "reloadPdfSavedItemsAfterMutation" in SOURCE
    assert "PdfSavedItemsSessionResult::ReloadRequired" in SOURCE
    assert "PdfSavedItemsSessionResult::SavePending" not in SOURCE


def test_pdf_ambiguous_mutation_reloads_both_legacy_stores_before_reconciliation():
    reload_path = SOURCE[
        SOURCE.index(
            "bool EpubReaderActivity::reloadPdfSavedItemsAfterMutation"
        ) : SOURCE.index("bool EpubReaderActivity::applyPendingPdfSavedItemJump")
    ]
    reload_bookmarks = reload_path.index("BOOKMARKS.reloadPdfFromDisk()")
    reload_clippings = reload_path.index("CLIPPINGS.reloadPdfFromDisk()")
    rebuild_session = reload_path.index("initializePdfSavedItems()")
    assert reload_bookmarks < rebuild_session
    assert reload_clippings < rebuild_session
    assert "BOOKMARKS.unload()" in reload_path
    assert "CLIPPINGS.unload()" in reload_path


def test_pdf_list_deletion_is_callback_driven_without_changing_epub_default():
    bookmark_header = (
        ROOT / "src/activities/reader/EpubReaderBookmarkListActivity.h"
    ).read_text(encoding="utf-8")
    clipping_header = (
        ROOT / "src/activities/reader/EpubReaderClippingListActivity.h"
    ).read_text(encoding="utf-8")
    assert "DeleteCallback" in bookmark_header
    assert "DeleteCallback" in clipping_header
    assert "removePdfBookmarkFromList" in SOURCE
    assert "removePdfClippingFromList" in SOURCE


def test_epub_default_list_deletion_does_not_copy_selected_records():
    bookmark_source = (
        ROOT / "src/activities/reader/EpubReaderBookmarkListActivity.cpp"
    ).read_text(encoding="utf-8")
    clipping_source = (
        ROOT / "src/activities/reader/EpubReaderClippingListActivity.cpp"
    ).read_text(encoding="utf-8")
    bookmark_delete = bookmark_source[
        bookmark_source.index("void EpubReaderBookmarkListActivity::deleteSelectedBookmark") :
        bookmark_source.index("void EpubReaderBookmarkListActivity::showBookmarkActionMenu")
    ]
    clipping_delete = clipping_source[
        clipping_source.index("void EpubReaderClippingListActivity::deleteSelectedClipping") :
        clipping_source.index("void EpubReaderClippingListActivity::showClippingActionMenu")
    ]
    assert "if (deleteCallback == nullptr)" in bookmark_delete
    assert "BOOKMARKS.removeBookmarkAt(static_cast<size_t>(selectedIndex))" in bookmark_delete
    assert "const Bookmark selected =" not in bookmark_delete
    assert "if (deleteCallback == nullptr)" in clipping_delete
    assert "CLIPPINGS.removeClippingAt(static_cast<size_t>(selectedIndex))" in clipping_delete
    assert "const Clipping selected =" not in clipping_delete


def test_epub_bookmark_action_lookup_ignores_pdf_only_stable_id():
    bookmark_source = (
        ROOT / "src/activities/reader/EpubReaderBookmarkListActivity.cpp"
    ).read_text(encoding="utf-8")
    lookup = bookmark_source[
        bookmark_source.index("const auto it = std::find_if") :
        bookmark_source.index("if (it != bookmarks.end())")
    ]
    assert "deleteCallback == nullptr ||" in lookup
    assert "bm.paragraphIndex == selectedBookmark.paragraphIndex" in lookup


def test_pdf_clipping_builds_parallel_semantics_and_uses_session():
    clipping_flow = SOURCE[
        SOURCE.index("void EpubReaderActivity::startClipSelection()") :
        SOURCE.index("void EpubReaderActivity::resetReadingPaceData()")
    ]
    assert "std::vector<PdfSelectableWordSemantic>" in clipping_flow
    assert "WORD_FLAG_SEMANTIC_ATTACHES" in clipping_flow
    assert "WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION" in clipping_flow
    assert "semanticUniqueWords != expectedSemanticWords" in clipping_flow
    assert "pdfReaderSession->savedItemsSession.add" in clipping_flow
    assert "PdfSavedItemKind::Clipping" in clipping_flow


def test_pdf_clipping_jump_and_highlight_map_semantics_not_legacy_ranges():
    jump = SOURCE[
        SOURCE.index("void EpubReaderActivity::handleClippingJump") :
        SOURCE.index("void EpubReaderActivity::onReaderMenuConfirm")
    ]
    assert "queueJump(PdfSavedItemKind::Clipping" in jump
    highlight = SOURCE[
        SOURCE.index("void EpubReaderActivity::drawClippingHighlights") :
        SOURCE.index("void EpubReaderActivity::renderStatusBar")
    ]
    assert "mapPdfSavedItem" not in highlight
    assert "pdfSavedItemsLayoutFingerprint" not in highlight
    assert "currentPageSemanticRange" in highlight
    assert "PdfSavedItemPageWordMapper" in highlight
    assert "pdfReaderSession->savedItemsSession.find(PdfSavedItemKind::Clipping" in highlight
