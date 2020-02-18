/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/BitManip.h"
#include "utils/Log.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/WinGui.h"
#include "wingui/TreeModel.h"
#include "wingui/Layout.h"
#include "wingui/Window.h"
#include "wingui/TreeCtrl.h"
#include "wingui/ButtonCtrl.h"
#include "wingui/StaticCtrl.h"

#include "resource.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SettingsStructs.h"
#include "WindowInfo.h"
#include "Menu.h"

#include "EngineBase.h"
#include "EngineMulti.h"
#include "EngineManager.h"

#include "ParseBKM.h"
#include "TocEditTitle.h"
#include "TocEditor.h"

using std::placeholders::_1;

// in TableOfContents.cpp
extern void OnTocCustomDraw(TreeItemCustomDrawEvent*);

struct TocEditorWindow {
    TocEditorArgs* tocArgs = nullptr;
    HWND hwnd = nullptr;

    ILayout* mainLayout = nullptr;
    // not owned by us but by mainLayout
    Window* mainWindow = nullptr;
    ButtonCtrl* btnAddPdf = nullptr;
    ButtonCtrl* btnRemoveTocItem = nullptr;
    ButtonCtrl* btnExit = nullptr;
    ButtonCtrl* btnSaveAsVirtual = nullptr;
    ButtonCtrl* btnSaveAsPdf = nullptr;
    StaticCtrl* labelInfo = nullptr;
    ILayout* layoutButtons = nullptr;

    TreeCtrl* treeCtrl = nullptr;

    ~TocEditorWindow();
    void SizeHandler(SizeEvent*);
    void CloseHandler(WindowCloseEvent*);
    void TreeItemChangedHandler(TreeItemChangedEvent*);
    void TreeItemSelectedHandler(TreeSelectionChangedEvent*);
    void TreeClickHandler(TreeClickEvent*);
    void GetDispInfoHandler(TreeGetDispInfoEvent*);
    void TreeItemDragStartEnd(TreeItemDraggeddEvent*);
    void TreeContextMenu(ContextMenuEvent*);
    void DropFilesHandler(DropFilesEvent*);

    void UpdateRemoveTocItemButtonStatus();
    void UpdateTreeModel();
    void SaveAsVirtual();
    void SaveAsPdf();
    void RemoveItem();
    void AddPdf();
    void AddPdfAsSibling(TocItem* ti);
    void AddPdfAsChild(TocItem* ti);
    void RemoveTocItem(TocItem* ti, bool alsoDelete);
};

static TocEditorWindow* gWindow = nullptr;

void MessageNYI() {
    HWND hwnd = gWindow->mainWindow->hwnd;
    MessageBoxA(hwnd, "Not yet implemented!", "Information", MB_OK | MB_ICONINFORMATION);
}

void ShowErrorMessage(const char* msg) {
    HWND hwnd = gWindow->mainWindow->hwnd;
    MessageBoxA(hwnd, msg, "Error", MB_OK | MB_ICONERROR);
}

void CalcEndPageNo2(TocItem* ti, int& nPages) {
    while (ti) {
        // this marks a root node for a document
        if (ti->nPages > 0) {
            nPages = ti->nPages;
            CalcEndPageNo(ti, nPages);
        } else {
            CalcEndPageNo2(ti->child, nPages);
        }
        ti = ti->next;
    }
}

void TocEditorWindow::UpdateTreeModel() {
    treeCtrl->Clear();

    VbkmFile* bookmarks = tocArgs->bookmarks;
    TocTree* tree = bookmarks->tree;
    int nPages = 0;
    CalcEndPageNo2(tree->root, nPages);
    SetTocTreeParents(tree->root);
    treeCtrl->SetTreeModel(tree);
}

static void SetTocItemFromTocEditArgs(TocItem* ti, TocEditArgs* args) {
    std::string_view newTitle = args->title.as_view();
    str::Free(ti->title);
    ti->title = strconv::Utf8ToWstr(newTitle);

    int fontFlags = 0;
    if (args->bold) {
        bit::Set(fontFlags, fontBitBold);
    }
    if (args->italic) {
        bit::Set(fontFlags, fontBitItalic);
    }
    ti->fontFlags = fontFlags;
    ti->color = args->color;
}

static TocItem* TocItemFromTocEditArgs(TocEditArgs* args) {
    if (args == nullptr) {
        return nullptr;
    }
    // we don't allow empty titles
    if (args->title.empty()) {
        return nullptr;
    }
    TocItem* ti = new TocItem();
    SetTocItemFromTocEditArgs(ti, args);
    return ti;
}

static void StartEditTocItem(HWND hwnd, TreeCtrl* treeCtrl, TocItem* ti) {
    TocEditArgs* editArgs = new TocEditArgs();
    editArgs->bold = bit::IsSet(ti->fontFlags, fontBitBold);
    editArgs->italic = bit::IsSet(ti->fontFlags, fontBitItalic);
    editArgs->title = strconv::WstrToUtf8(ti->title);
    editArgs->color = ti->color;

    StartTocEditTitle(hwnd, editArgs, [=](TocEditArgs* args) {
        delete editArgs;
        if (args == nullptr) {
            // was cancelled
            return;
        }

        SetTocItemFromTocEditArgs(ti, args);
        treeCtrl->UpdateItem(ti);
    });
}

// clang-format off
#define IDM_EDIT            100
#define IDM_ADD_SIBLING     101
#define IDM_ADD_CHILD       102
#define IDM_REMOVE          103
#define IDM_ADD_PDF_CHILD   104
#define IDM_ADD_PDF_SIBLING 105

static MenuDef menuDefContext[] = {
    {"Edit",                    IDM_EDIT, 0},
    {"Add sibling",             IDM_ADD_SIBLING, 0},
    {"Add child",               IDM_ADD_CHILD, 0},
    {"Add PDF as a child",      IDM_ADD_PDF_CHILD, 0},
    {"Add PDF as a sibling",    IDM_ADD_PDF_SIBLING, 0},
    {"Remove Item",             IDM_REMOVE, 0},
    { 0, 0, 0},
};
// clang-format on

static bool RemoveIt(TreeCtrl* treeCtrl, TocItem* ti) {
    TocItem* parent = ti->parent;
    if (parent && parent->child == ti) {
        parent->child = ti->next;
        ti->next = nullptr;
        return true;
    }

    // first sibling for ti
    TocItem* curr = nullptr;
    if (parent) {
        curr = parent->child;
    } else {
        TocTree* tree = (TocTree*)treeCtrl->treeModel;
        curr = tree->root;
        // ti is the first top-level element
        if (curr == ti) {
            tree->root = ti->next;
            return true;
        }
    }
    // remove ti from list of siblings
    while (curr) {
        if (curr->next == ti) {
            curr->next = ti->next;
            ti->next = nullptr;
            return true;
        }
        curr = curr->next;
    }
    // didn't find ti in a list of siblings, shouldn't happen
    CrashMe();
    return false;
}

// ensure is visible i.e. expand all parents of this item
static void EnsureExpanded(TocItem* ti) {
    while (ti) {
        ti->isOpenDefault = true;
        ti->isOpenToggled = false;
        ti = ti->parent;
    }
}

void TocEditorWindow::RemoveTocItem(TocItem* ti, bool alsoDelete) {
    EnsureExpanded(ti->parent);

    bool ok = RemoveIt(treeCtrl, ti);
    if (ok && alsoDelete) {
        UpdateTreeModel();
        ti->DeleteJustSelf();
    }
}

static EngineBase* ChooosePdfFile() {
    TocEditorWindow* w = gWindow;
    HWND hwnd = w->mainWindow->hwnd;

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;

    ofn.lpstrFilter = L".pdf\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;

    // OFN_ENABLEHOOK disables the new Open File dialog under Windows Vista
    // and later, so don't use it and just allocate enough memory to contain
    // several dozen file paths and hope that this is enough
    // TODO: Use IFileOpenDialog instead (requires a Vista SDK, though)
    ofn.nMaxFile = MAX_PATH * 2;
    AutoFreeWstr file = AllocArray<WCHAR>(ofn.nMaxFile);
    ofn.lpstrFile = file;

    if (!GetOpenFileNameW(&ofn)) {
        return nullptr;
    }
    WCHAR* filePath = ofn.lpstrFile;

    EngineBase* engine = EngineManager::CreateEngine(filePath);
    if (!engine) {
        ShowErrorMessage("Failed to open a file!");
        return nullptr;
    }
    return engine;
}

static TocItem* CreateWrapperItem(EngineBase* engine, TocItem* ti) {
    TocItem* tocFileRoot = nullptr;
    TocTree* tocTree = engine->GetToc();
    // it's ok if engine doesn't have toc
    if (tocTree) {
        tocFileRoot = CloneTocItemRecur(tocTree->root, false);
    }

    int nPages = engine->PageCount();
    char* filePath = (char*)strconv::WstrToUtf8(engine->FileName()).data();
    const WCHAR* title = path::GetBaseNameNoFree(engine->FileName());
    TocItem* tocWrapper = new TocItem(tocFileRoot, title, 0);
    tocWrapper->isOpenDefault = true;
    tocWrapper->child = tocFileRoot;
    tocWrapper->engineFilePath = filePath;
    tocWrapper->nPages = nPages;
    tocWrapper->pageNo = 1;
    if (tocFileRoot) {
        tocFileRoot->parent = tocWrapper;
    }
    return tocWrapper;
}

void TocEditorWindow::AddPdfAsChild(TocItem* ti) {
    EngineBase* engine = ChooosePdfFile();
    if (!engine) {
        return;
    }
    TocItem* tocWrapper = CreateWrapperItem(engine, ti);
    ti->AddChild(tocWrapper);
    UpdateTreeModel();
    delete engine;
}

void TocEditorWindow::AddPdfAsSibling(TocItem* ti) {
    EngineBase* engine = ChooosePdfFile();
    if (!engine) {
        return;
    }
    TocItem* tocWrapper = CreateWrapperItem(engine, ti);
    ti->AddSibling(tocWrapper);
    UpdateTreeModel();
    delete engine;
}

void TocEditorWindow::AddPdf() {
    EngineBase* engine = ChooosePdfFile();
    if (!engine) {
        return;
    }

    TocItem* tocWrapper = CreateWrapperItem(engine, (TocItem*)treeCtrl->treeModel->RootAt(0));
    tocArgs->bookmarks->tree->root->AddSiblingAtEnd(tocWrapper);
    UpdateTreeModel();
    delete engine;
}

static bool CanRemoveTocItem(TreeCtrl* treeCtrl, TocItem* ti) {
    if (!ti) {
        return false;
    }
    TocTree* tree = (TocTree*)treeCtrl->treeModel;
    if (tree->RootCount() == 1 && tree->root == ti) {
        // don't allow removing only remaining root node
        return false;
    }
    return true;
}

// TODO: simplify and verify is correct
static bool CanAddPdfAsChild(TocItem* tocItem) {
    bool canAddPdfChild = true;
    bool canAddPdfSibling = true;
    TocItem* ti = tocItem;
    while (ti) {
        // if ti is a n-th sibling of a file node, this sets it to file node
        // (i.e. first sibling)
        if (ti->parent) {
            ti = ti->parent->child;
        }
        if (ti->engineFilePath != nullptr) {
            // can't add as a child if this node or any parent
            // represents PDF file
            canAddPdfChild = false;
            // can't add as sibling if any parent represents PDF file
            canAddPdfSibling = (ti == tocItem);
            break;
        }
        ti = ti->parent;
    }
    return canAddPdfChild;
}

// TODO: simplify and verify is correct
static bool CanAddPdfAsSibling(TocItem* tocItem) {
    bool canAddPdfChild = true;
    bool canAddPdfSibling = true;
    TocItem* ti = tocItem;
    while (ti) {
        // if ti is a n-th sibling of a file node, this sets it to file node
        // (i.e. first sibling)
        if (ti->parent) {
            ti = ti->parent->child;
        }
        if (ti->engineFilePath != nullptr) {
            // can't add as a child if this node or any parent
            // represents PDF file
            canAddPdfChild = false;
            // can't add as sibling if any parent represents PDF file
            canAddPdfSibling = (ti == tocItem);
            break;
        }
        ti = ti->parent;
    }
    return canAddPdfSibling;
}

void TocEditorWindow::DropFilesHandler(DropFilesEvent* ev) {
    int nFiles = DragQueryFile(ev->hdrop, DRAGQUERY_NUMFILES, 0, 0);
    logf("TocEditorWindow::DropFilesHandler(): %d files\n", nFiles);
    defer {
        DragFinish(ev->hdrop);
    };

    POINT pt{};
    BOOL ok = DragQueryPoint(ev->hdrop, &pt);
    if (!ok) {
        return; // probably shouldn't happen
    }

    TocItem* ti = (TocItem*)treeCtrl->GetItemAt(pt.x, pt.y);

    // TODO: maybe accept more than 1 file?
    if (nFiles != 1) {
        return;
    }

    // we only accept pdf files
    WCHAR filePath[MAX_PATH] = {0};
    bool found = false;
    for (int i = 0; i < nFiles && !found; i++) {
        DragQueryFile(ev->hdrop, i, filePath, dimof(filePath));
        // TODO: maybe resolve .lnk files like OnDropFiles() in Canvas.cpp
        if (str::EndsWithI(filePath, L".pdf")) {
            found = true;
        }
    }

    if (!found) {
        return;
    }

    EngineBase* engine = EngineManager::CreateEngine(filePath, nullptr);
    AutoFreeStr path = strconv::WstrToUtf8(filePath);
    logf("Dropped file: '%s' at (%d, %d) on item: 0x%x, engine: 0x%x\n", path.get(), pt.x, pt.y, ti, engine);

    if (!engine) {
        return;
    }

    defer {
        delete engine;
    };

    TocItem* fileToc = (TocItem*)treeCtrl->treeModel->RootAt(0);

    // didn't drop on an existing itme: add as a last sibling
    if (ti == nullptr) {
        TocItem* tocWrapper = CreateWrapperItem(engine, fileToc);
        tocArgs->bookmarks->tree->root->AddSiblingAtEnd(tocWrapper);
        UpdateTreeModel();
        return;
    }

    bool addAsSibling = IsShiftPressed();
    if (addAsSibling) {
        if (CanAddPdfAsSibling(ti)) {
            TocItem* tocWrapper = CreateWrapperItem(engine, fileToc);
            ti->AddSibling(tocWrapper);
            UpdateTreeModel();
        }
        return;
    }

    if (CanAddPdfAsChild(ti)) {
        TocItem* tocWrapper = CreateWrapperItem(engine, fileToc);
        ti->AddChild(tocWrapper);
        UpdateTreeModel();
    }
}

void TocEditorWindow::TreeContextMenu(ContextMenuEvent* ev) {
    ev->didHandle = true;

    POINT pt{};
    TreeItem* menuTreeItem = GetOrSelectTreeItemAtPos(ev, pt);
    if (!menuTreeItem) {
        return;
    }
    TocItem* selectedTocItem = (TocItem*)menuTreeItem;
    HMENU popup = BuildMenuFromMenuDef(menuDefContext, CreatePopupMenu());

    if (!CanRemoveTocItem(treeCtrl, selectedTocItem)) {
        win::menu::SetEnabled(popup, IDM_REMOVE, false);
    }

    bool canAddPdfChild = CanAddPdfAsChild(selectedTocItem);
    bool canAddPdfSibling = CanAddPdfAsSibling(selectedTocItem);

    if (!canAddPdfChild) {
        win::menu::SetEnabled(popup, IDM_ADD_PDF_CHILD, false);
    }
    if (!canAddPdfSibling) {
        win::menu::SetEnabled(popup, IDM_ADD_PDF_SIBLING, false);
    }

    MarkMenuOwnerDraw(popup);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    INT cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, hwnd, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmd) {
        case IDM_EDIT:
            StartEditTocItem(mainWindow->hwnd, treeCtrl, selectedTocItem);
            break;
        case IDM_ADD_SIBLING:
        case IDM_ADD_CHILD: {
            TocEditArgs* editArgs = new TocEditArgs();
            StartTocEditTitle(hwnd, editArgs, [=](TocEditArgs* args) {
                delete editArgs;
                TocItem* ti = TocItemFromTocEditArgs(args);
                if (ti == nullptr) {
                    // was cancelled or invalid
                    return;
                }
                if (cmd == IDM_ADD_SIBLING) {
                    selectedTocItem->AddSibling(ti);
                } else if (cmd == IDM_ADD_CHILD) {
                    selectedTocItem->AddChild(ti);
                } else {
                    CrashMe();
                }
                EnsureExpanded(selectedTocItem);
                UpdateTreeModel();
            });
        } break;
        case IDM_ADD_PDF_CHILD:
            AddPdfAsChild(selectedTocItem);
            break;
        case IDM_ADD_PDF_SIBLING:
            AddPdfAsSibling(selectedTocItem);
            break;
        case IDM_REMOVE:
            RemoveTocItem(selectedTocItem, true);
            break;
    }
}

static void SetInfoLabelText(StaticCtrl* l, bool forDrag) {
    if (forDrag) {
        l->SetText("Press SHIFT to add as a sibling, otherwise a child");
    } else {
        l->SetText("Tip: use context menu for more actions");
    }
}

// find toc item that is a parent of a given ti that represents a pdf file
static TocItem* FindFileParentItem(TocItem* ti) {
    while (ti) {
        if (ti->engineFilePath) {
            return ti;
        }
        ti = ti->parent;
    }
    return nullptr;
}

void TocEditorWindow::TreeItemDragStartEnd(TreeItemDraggeddEvent* ev) {
    if (ev->isStart) {
        SetInfoLabelText(labelInfo, true);
        return;
    }
    SetInfoLabelText(labelInfo, false);

    TocItem* src = (TocItem*)ev->draggedItem;
    TocItem* dst = (TocItem*)ev->dragTargetItem;
    CrashIf(!src);
    if (!src) {
        return;
    }
    if (!dst) {
        // TODO: append to end?
        return;
    }

    if (src == dst) {
        return;
    }

    // entries inside a single PDF cannot be moved outside of it
    // entries outside of a PDF cannot be moved inside PDF
    TocItem* srcFileParent = FindFileParentItem(src);
    TocItem* dstFileParent = FindFileParentItem(dst);
    if (srcFileParent != dstFileParent) {
        // TODO: show error message that will go away after a while
        return;
    }
    // regular drag adds as a child. with shift adds as a sibling of
    bool addAsSibling = IsShiftPressed();

    AutoFreeStr srcTitle = strconv::WstrToUtf8(src->title);
    AutoFreeStr dstTitle = strconv::WstrToUtf8(dst->title);
    dbglogf("TreeItemDragged: dragged: %s on: %s. Add as: %s\n", srcTitle.get(), dstTitle.get(),
            addAsSibling ? "sibling" : "child");

    RemoveTocItem(src, false);
    if (addAsSibling) {
        dst->AddSibling(src);
    } else {
        dst->AddChild(src);
    }
    UpdateTreeModel();
}

TocEditorWindow::~TocEditorWindow() {
    // TODO: delete the top but not children, because
    // they are not owned
    // delete treeModel;

    // deletes all controls owned by layout
    delete mainLayout;

    delete tocArgs;
    delete mainWindow;
}

TocEditorArgs::~TocEditorArgs() {
    delete bookmarks;
}

void TocEditorWindow::RemoveItem() {
    TreeItem* sel = treeCtrl->GetSelection();
    CrashIf(!sel);
    TocItem* ti = (TocItem*)sel;
    RemoveTocItem(ti, true);
}

void TocEditorWindow::UpdateRemoveTocItemButtonStatus() {
    TreeItem* sel = treeCtrl->GetSelection();
    bool isEnabled = CanRemoveTocItem(treeCtrl, (TocItem*)sel);
    btnRemoveTocItem->SetIsEnabled(isEnabled);
}

// in TableOfContents.cpp
extern void ShowExportedBookmarksMsg(const char* path);

void TocEditorWindow::SaveAsPdf() {
    MessageNYI();
}

void TocEditorWindow::SaveAsVirtual() {
    str::WStr pathw = tocArgs->filePath.get();

    // if the source was .vbkm file, we over-write it by default
    // any other format, we add .vbkm extension by default
    if (!str::EndsWithI(pathw.Get(), L".vbkm")) {
        pathw.Append(L".vbkm");
    }
    WCHAR dstFileName[MAX_PATH]{0};
    str::BufSet(&(dstFileName[0]), dimof(dstFileName), pathw.Get());

    HWND hwnd = gWindow->mainWindow->hwnd;

    OPENFILENAME ofn{0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = L".vbkm\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"vbkm";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    // note: explicitly not setting lpstrInitialDir so that the OS
    // picks a reasonable default (in particular, we don't want this
    // in plugin mode, which is likely the main reason for saving as...)

    bool ok = GetSaveFileNameW(&ofn);
    if (!ok) {
        return;
    }
    AutoFree patha = strconv::WstrToUtf8(dstFileName);
    ok = ExportBookmarksToFile(tocArgs->bookmarks->tree, "", patha);
    if (!ok) {
        return;
    }
    // ShowExportedBookmarksMsg(patha);
}

static void CreateButtonsLayout(TocEditorWindow* w) {
    HWND hwnd = w->hwnd;
    CrashIf(!hwnd);

    auto* buttons = new HBox();

    buttons->alignMain = MainAxisAlign::Homogeneous;
    buttons->alignCross = CrossAxisAlign::CrossStart;
    {
        auto [l, b] = CreateButtonLayout(hwnd, "Add PDF", std::bind(&TocEditorWindow::AddPdf, w));
        buttons->addChild(l);
        w->btnAddPdf = b;
    }

    {
        auto [l, b] = CreateButtonLayout(hwnd, "Remove Item", std::bind(&TocEditorWindow::RemoveItem, w));
        buttons->addChild(l);
        w->btnRemoveTocItem = b;
    }

    {
        auto [l, b] = CreateButtonLayout(hwnd, "Save As PDF", std::bind(&TocEditorWindow::SaveAsPdf, w));
        buttons->addChild(l);
        w->btnSaveAsPdf = b;
    }

    {
        auto [l, b] = CreateButtonLayout(hwnd, "Save As Virtual PDF", std::bind(&TocEditorWindow::SaveAsVirtual, w));
        buttons->addChild(l);
        w->btnSaveAsVirtual = b;
    }

    {
        auto [l, b] = CreateButtonLayout(hwnd, "Exit", std::bind(&Window::Close, w->mainWindow));
        buttons->addChild(l);
        w->btnExit = b;
    }

    w->layoutButtons = buttons;
}

static void CreateMainLayout(TocEditorWindow* win) {
    HWND hwnd = win->hwnd;
    CrashIf(!hwnd);

    CreateButtonsLayout(win);

    auto* tree = new TreeCtrl(hwnd);
    gWindow->treeCtrl = tree;

    int dx = DpiScale(80);
    int dy = DpiScale(120);
    tree->idealSize = {dx, dy};

    tree->supportDragDrop = true;
    tree->withCheckboxes = true;
    tree->onTreeGetDispInfo = std::bind(&TocEditorWindow::GetDispInfoHandler, win, _1);
    tree->onDropFiles = std::bind(&TocEditorWindow::DropFilesHandler, win, _1);
    tree->onTreeItemChanged = std::bind(&TocEditorWindow::TreeItemChangedHandler, win, _1);
    tree->onTreeItemCustomDraw = OnTocCustomDraw;
    tree->onTreeSelectionChanged = std::bind(&TocEditorWindow::TreeItemSelectedHandler, win, _1);
    tree->onTreeClick = std::bind(&TocEditorWindow::TreeClickHandler, win, _1);
    tree->onTreeItemDragStart = std::bind(&TocEditorWindow::TreeItemDragStartEnd, win, _1);
    tree->onTreeItemDragEnd = std::bind(&TocEditorWindow::TreeItemDragStartEnd, win, _1);
    tree->onContextMenu = std::bind(&TocEditorWindow::TreeContextMenu, win, _1);

    bool ok = tree->Create(L"tree");
    CrashIf(!ok);

    auto treeLayout = NewTreeLayout(tree);

    win->labelInfo = new StaticCtrl(hwnd);
    SetInfoLabelText(win->labelInfo, false);
    COLORREF col = MkGray(0x33);
    win->labelInfo->SetTextColor(col);
    win->labelInfo->Create();
    ILayout* labelLayout = NewStaticLayout(win->labelInfo);

    auto* main = new VBox();
    main->alignMain = MainAxisAlign::MainStart;
    main->alignCross = CrossAxisAlign::Stretch;

    main->addChild(treeLayout, 1);
    main->addChild(labelLayout, 0);
    main->addChild(win->layoutButtons, 0);

    auto* padding = new Padding();
    padding->insets = DefaultInsets();
    padding->child = main;
    win->mainLayout = padding;
}

void TocEditorWindow::SizeHandler(SizeEvent* ev) {
    int dx = ev->dx;
    int dy = ev->dy;
    HWND hwnd = ev->hwnd;
    if (dx == 0 || dy == 0) {
        return;
    }
    Size windowSize{dx, dy};
    auto c = Tight(windowSize);
    auto size = mainLayout->Layout(c);
    Point min{0, 0};
    Point max{size.Width, size.Height};
    Rect bounds{min, max};
    mainLayout->SetBounds(bounds);
    InvalidateRect(hwnd, nullptr, false);
    ev->didHandle = true;
}

void TocEditorWindow::CloseHandler(WindowCloseEvent* ev) {
    WindowBase* w = (WindowBase*)gWindow->mainWindow;
    CrashIf(w != ev->w);
    delete gWindow;
    gWindow = nullptr;
}

void TocEditorWindow::TreeItemChangedHandler(TreeItemChangedEvent* ev) {
    if (!ev->checkedChanged) {
        return;
    }
    TocItem* ti = (TocItem*)ev->treeItem;
    ti->isUnchecked = !ev->newState.isChecked;
}

void TocEditorWindow::TreeItemSelectedHandler(TreeSelectionChangedEvent* ev) {
    UNUSED(ev);
    UpdateRemoveTocItemButtonStatus();
}

void TocEditorWindow::GetDispInfoHandler(TreeGetDispInfoEvent* ev) {
    ev->didHandle = true;

    TocItem* ti = (TocItem*)ev->treeItem;
    TVITEMEXW* tvitem = &ev->dispInfo->item;
    CrashIf(tvitem->mask != TVIF_TEXT);

    size_t cchMax = tvitem->cchTextMax;
    CrashIf(cchMax < 32);

    int sno = ti->pageNo;
    if (sno <= 0) {
        str::BufSet(tvitem->pszText, cchMax, ti->title);
        return;
    }
    int eno = ti->endPageNo;
    WCHAR* s = nullptr;
    if (eno > sno) {
        if (ti->engineFilePath) {
            const char* name = path::GetBaseNameNoFree(ti->engineFilePath);
            AutoFreeWstr nameW = strconv::Utf8ToWstr(name);
            s = str::Format(L"%s [file: %s, pages %d-%d]", ti->title, nameW.get(), sno, eno);
        } else {
            s = str::Format(L"%s [pages %d-%d]", ti->title, sno, eno);
        }
    } else {
        if (ti->engineFilePath) {
            const char* name = path::GetBaseNameNoFree(ti->engineFilePath);
            AutoFreeWstr nameW = strconv::Utf8ToWstr(name);
            s = str::Format(L"%s [file: %s, page %d]", ti->title, nameW.get(), sno);
        } else {
            s = str::Format(L"%s [page %d]", ti->title, sno);
        }
    }
    str::BufSet(tvitem->pszText, cchMax, s);
    str::Free(s);
}

void TocEditorWindow::TreeClickHandler(TreeClickEvent* ev) {
    if (!ev->isDblClick) {
        return;
    }
    if (!ev->treeItem) {
        return;
    }

    ev->didHandle = true;
    ev->result = 1;

    TocItem* ti = (TocItem*)ev->treeItem;
    StartEditTocItem(mainWindow->hwnd, treeCtrl, ti);
}

void StartTocEditor(TocEditorArgs* args) {
    HWND hwndOwner = args->hwndRelatedTo;
    if (gWindow != nullptr) {
        // TODO: maybe allow multiple windows
        gWindow->mainWindow->onDestroy = nullptr;
        delete gWindow;
        gWindow = nullptr;
    }

    auto win = new TocEditorWindow();
    gWindow = win;
    win->tocArgs = args;
    auto w = new Window();
    w->backgroundColor = MkRgb((u8)0xee, (u8)0xee, (u8)0xee);
    w->SetTitle("Table of content editor");
    int dx = DpiScale(hwndOwner, 640);
    int dy = DpiScale(hwndOwner, 800);
    w->initialSize = {dx, dy};
    PositionCloseTo(w, args->hwndRelatedTo);
    SIZE winSize = {w->initialSize.Width, w->initialSize.Height};
    LimitWindowSizeToScreen(args->hwndRelatedTo, winSize);
    w->initialSize = {winSize.cx, winSize.cy};
    bool ok = w->Create();
    CrashIf(!ok);

    win->mainWindow = w;
    win->hwnd = w->hwnd;

    CreateMainLayout(gWindow);

    w->onClose = std::bind(&TocEditorWindow::CloseHandler, win, _1);
    w->onSize = std::bind(&TocEditorWindow::SizeHandler, win, _1);

    gWindow->UpdateTreeModel();
    // important to call this after hooking up onSize to ensure
    // first layout is triggered
    w->SetIsVisible(true);
    gWindow->UpdateRemoveTocItemButtonStatus();
}
