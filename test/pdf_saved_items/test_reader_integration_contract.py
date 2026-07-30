from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
SECTION_HEADER = (ROOT / "lib/Epub/Epub/Section.h").read_text(encoding="utf-8")


def test_pdf_reader_owns_one_bounded_saved_item_array_and_session():
    assert "std::unique_ptr<PdfSavedItem[]> pdfSavedItemStorage" in HEADER
    assert "PdfSavedItemsSession pdfSavedItemsSession" in HEADER
    assert (
        "makeUniqueNoThrow<PdfSavedItem[]>(PDF_SAVED_ITEMS_MAX_RECORDS)"
        in SOURCE
    )


def test_pdf_legacy_stores_load_before_session_and_epub_skips_pdf_callbacks():
    on_enter = SOURCE[SOURCE.index("void EpubReaderActivity::onEnter()") :]
    load_bookmarks = on_enter.index("BOOKMARKS.loadForBook")
    load_clippings = on_enter.index("CLIPPINGS.loadForBook")
    initialize_session = on_enter.index("initializePdfSavedItems()")
    assert load_bookmarks < initialize_session
    assert load_clippings < initialize_session
    assert "pdfSavedItemsSession.begin" in SOURCE
    assert "if (document->getFormat() == ReflowDocumentFormat::Pdf)" in SOURCE
    assert "bool EpubReaderActivity::supportsSavedItems() const" in SOURCE


def test_saved_items_flush_before_legacy_unload_and_storage_release():
    on_exit = SOURCE[SOURCE.index("void EpubReaderActivity::onExit()") :]
    flush = on_exit.index("pdfSavedItemsSession.flush()")
    unload = on_exit.index("BOOKMARKS.unload()")
    release = on_exit.index("pdfSavedItemStorage.reset()")
    assert flush < unload < release


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
    assert "pdfSavedItemsSession.add" in bookmark_case
    assert "pdfSavedItemsSession.remove" in bookmark_case

    clear_case = SOURCE[
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS") :
        SOURCE.index("case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN")
    ]
    assert "pdfSavedItemsSession.clear(PdfSavedItemKind::Bookmark)" in clear_case


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


def test_pdf_clipping_builds_parallel_semantics_and_uses_session():
    clipping_flow = SOURCE[
        SOURCE.index("void EpubReaderActivity::startClipSelection()") :
        SOURCE.index("void EpubReaderActivity::resetReadingPaceData()")
    ]
    assert "std::vector<PdfSelectableWordSemantic>" in clipping_flow
    assert "WORD_FLAG_SEMANTIC_ATTACHES" in clipping_flow
    assert "WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION" in clipping_flow
    assert "semanticUniqueWords != expectedSemanticWords" in clipping_flow
    assert "pdfSavedItemsSession.add" in clipping_flow
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
    assert "pdfSavedItemsSession.find(PdfSavedItemKind::Clipping" in highlight
