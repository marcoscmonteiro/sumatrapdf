#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "wingui/TreeModel.h"

#include "Annotation.h"
#include "EngineBase.h"
#include "DisplayMode.h"
#include "SettingsStructs.h"
#include "Controller.h"
#include "GlobalPrefs.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "Toolbar.h"

/* Auxiliary function to callback Plugin Host Window with a OnCopyData message
 *  Based on SumatraLaunchBrowser function on SumatraPDF.cpp file
 *  MCM 24-04-2016
 */
LRESULT PluginHostCopyData(WindowInfo* win, const WCHAR* msg, ...) {
    if (!gPluginMode || !win)
        return false;

    HWND PluginWin = win->hwndFrame;
    if (PluginWin == 0) {
        CrashIf(gWindows.empty());
        if (gWindows.empty())
            return false;
        PluginWin = gWindows.at(0)->hwndFrame;
    }

    HWND ParentWin = GetAncestor(PluginWin, GA_PARENT);
    if (!ParentWin) return false;

    // Format msg string with argment list
    va_list args;

    va_start(args, msg);
    ScopedMem<WCHAR> MsgStr(str::FmtV(msg, args));
    va_end(args);

    // Converts MsgStr0 to UTF8
    AutoFree MsgStrUTF8(strconv::WstrToUtf8(MsgStr));

    // Prepare struct and send message to plugin Host Window
    COPYDATASTRUCT cds = {0x44646558, /* Message to/from SumatraPDF Plugin */
                          (DWORD)MsgStrUTF8.size() + 1, MsgStrUTF8.Get()};
    return SendMessage(ParentWin, WM_COPYDATA, (WPARAM)PluginWin, (LPARAM)&cds);
}

void MakePluginWindow(WindowInfo* win, HWND hwndParent) {
    CrashIf(!IsWindow(hwndParent));
    CrashIf(!gPluginMode);
    
    auto hwndFrame = win->hwndFrame;
    
    long ws = GetWindowLong(hwndFrame, GWL_STYLE);
    ws &= ~(WS_POPUP | WS_BORDER | WS_CAPTION | WS_THICKFRAME);
    ws |= WS_CHILD;
    SetWindowLong(hwndFrame, GWL_STYLE, ws);

    SetParent(hwndFrame, hwndParent);
    
    // MoveWindow(hwndFrame, ParentRect);
    // Line above commented e substituted by 2 lines below.
    // MSDN Documentation recomends use of SetWindowPos after use of SetWindowLong changing frame style.
    // See remarks in reference:
    // https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlonga?redirectedfrom=MSDN&f1url=%3FappId%3DDev16IDEF1%26l%3DEN-US%26k%3Dk(WINUSER%252FSetWindowLong);k(SetWindowLong);k(DevLang-C%252B%252B);k(TargetOS-Windows)%26rd%3Dtrue
    Rect ParentRect = ClientRect(hwndParent);
    SetWindowPos(hwndFrame, HWND_BOTTOM, 0, 0, ParentRect.dx, ParentRect.dy, SWP_FRAMECHANGED);

    ShowWindow(hwndFrame, SW_SHOW);
    
    UpdateWindow(hwndFrame);

    // from here on, we depend on the plugin's host to resize us
    SetFocus(hwndFrame);
}

// Open file new file in plugin mode
static const WCHAR* HandleOpenFileCmd(WindowInfo* win, const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile, hwndPluginParentStr;
    HWND hwndPluginParent;
    const WCHAR* next = str::Parse(cmd, L"[OpenFile(\"%S\",%? \"%S\")]", &pdfFile, &hwndPluginParentStr);
    if (!next) return nullptr;

    hwndPluginParent = (HWND)(INT_PTR)_wtol(hwndPluginParentStr.Get());

    // Close previus document if already loaded in current window 
    HWND parent = GetAncestor(win->hwndFrame, GA_PARENT);
    if (win->IsDocLoaded() && hwndPluginParent == parent) {
        CloseWindow(win, false, false);
    }

    // Create new window with document and put it embeded in parent window
    WindowInfo* newWin = nullptr;   
    LoadArgs args(pdfFile, nullptr);
    args.showWin = false;
    newWin = LoadDocument(args);
    MakePluginWindow(newWin, hwndPluginParent);

    // By default show toolbar
    gGlobalPrefs->showToolbar = true;
    ShowOrHideToolbar(newWin);

    // Repaint windows canvas
    RepaintNow(newWin->hwndCanvas);

    ack.fAck = 1;
    PluginHostCopyData(newWin, L"[FileOpened()]");
    return next;
}

// DDE command  : Send a message to application plugin host with requested property(ies)
// Format       : [GetProperty("<ProperyName>")]
// eg.          : [GetProperty("Page")]
//                In this example, the message sent to application plugin host is
//                [Page(<currentpage>,"<currentnameddest>")]
static const WCHAR* HandleGetPropertyCmd(WindowInfo* win, const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr PropertyName;
    const WCHAR* next = str::Parse(cmd, L"[GetProperty(\"%S\")]", &PropertyName);

    if (!next) return nullptr;

    ack.fAck = 1;

    if (str::Eq(PropertyName, L"ToolbarVisible")) {
        PluginHostCopyData(win, L"[%s(%d)]", PropertyName.Get(), IsWindowStyleSet(win->hwndReBar, WS_VISIBLE));
        return next;
    }

    if (str::Eq(PropertyName, L"TocVisible")) {
        PluginHostCopyData(win, L"[%s(%d)]", PropertyName.Get(), win->tocVisible);
        return next;
    }

    if (str::Eq(PropertyName, L"Page")) {
        const WCHAR* pageLabel = (win->ctrl->HasPageLabels()) ? win->ctrl->GetPageLabel(win->currPageNo) : L"";
        PluginHostCopyData(win, L"[%s(%d,\"%s\")]", PropertyName.Get(), win->currPageNo, pageLabel);
        return next;
    }

    if (str::Eq(PropertyName, L"DisplayMode")) {
        PluginHostCopyData(win, L"[%s(%d)]", PropertyName.Get(), win->ctrl->GetDisplayMode());
        return next;
    }

    if (str::Eq(PropertyName, L"Zoom")) {
        PluginHostCopyData(win, L"[%s(%f,%f)]", PropertyName.Get(), win->ctrl->GetZoomVirtual(true),
                           win->ctrl->GetZoomVirtual(false));
        return next;
    }

    // Next properties requires DisplayModel
    DisplayModel* dm = win->AsFixed();
    if (dm) {
        if (str::Eq(PropertyName, L"ScrollPosition")) {
            ScrollState ss = dm->GetScrollState();
            PluginHostCopyData(win, L"[%s(%d,%f,%f)]", PropertyName.Get(), ss.page, ss.x, ss.y);
            return next;
        }

        if (str::Eq(PropertyName, L"Rotation")) {
            int rotation = dm->GetRotation();
            PluginHostCopyData(win, L"[%s(%d)]", PropertyName.Get(), rotation);
            return next;
        }
    }

    ack.fAck = 0;
    return next;
}

// DDE command  : Set a Sumatra propery from application plugin host
// Format       : [SetProperty("<ProperyName>", "value")]
// eg.          : [SetProperty("ToolbarVisible","1")]
//                In this example, the Toolbar is set to be shown
static const WCHAR* HandleSetPropertyCmd(WindowInfo* win, const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr PropertyName, PropertyValue;
    const WCHAR* next = str::Parse(cmd, L"[SetProperty(\"%S\",%? \"%S\")]", &PropertyName, &PropertyValue);

    if (!next) return nullptr;

    ack.fAck = 1;

    if (str::Eq(PropertyName, L"Page")) {
        uint page = 0;
        str::Parse(PropertyValue.Get(), L"%u", &page);
        if (!win->ctrl->ValidPageNo(page)) return next;
        win->ctrl->GoToPage(page, true);
        return next;
    }

    if (str::Eq(PropertyName, L"ToolbarVisible")) {
        gGlobalPrefs->showToolbar = !str::Eq(PropertyValue, L"0");
        ShowOrHideToolbar(win);
        return next;
    }

    if (str::Eq(PropertyName, L"TocVisible")) {
        if (!str::Eq(PropertyValue, L"0") != win->tocVisible) {
            win->tocVisible = !win->tocVisible;
            SetSidebarVisibility(win, win->tocVisible, gGlobalPrefs->showFavorites);
        }
        return next;
    }

    if (str::Eq(PropertyName, L"DisplayMode")) {
        // AutoFreeStr viewModeWstr = strconv::WstrToUtf8(PropertyValue);
        // DisplayMode mode = DisplayModeFromString(viewModeWstr.Get(), DisplayMode::Automatic);
        DisplayMode mode = DisplayMode::Automatic;
        str::Parse(PropertyValue.Get(), L"%u", &mode);
        if (mode != DisplayMode::Automatic) {
            SwitchToDisplayMode(win, mode);
        }
        return next;
    }

    if (str::Eq(PropertyName, L"Zoom")) {
        float zoom = INVALID_ZOOM;
        str::Parse(PropertyValue.Get(), L"%f", &zoom);
        if (zoom != INVALID_ZOOM) {
            ZoomToSelection(win, zoom);
        }
        return next;
    }

    // Next properties requires DisplayModel
    DisplayModel* dm = win->AsFixed();
    if (dm) {
        if (str::Eq(PropertyName, L"ScrollPosition")) {
            int page;
            double x, y;
            str::Parse(PropertyValue.Get(), L"%d,%D,%D", &page, &x, &y);
            ScrollState ss;
            ss.page = page;
            ss.x = x;
            ss.y = y;
            dm->SetScrollState(ss);
            return next;
        }

        if (str::Eq(PropertyName, L"RotateBy")) {
            int rotation;
            str::Parse(PropertyValue.Get(), L"%d", &rotation);
            dm->RotateBy(rotation);
            return next;
        }
    }


    ack.fAck = 0;
    return next;
}

// DDE command  : Do a text search in document (from current page)
// Format       : [TextSearch(<searchText>,<matchCase>)]
// eg.          : [TextSearch("Text to Search", 1)]
static const WCHAR* HandleTextSearchCmd(WindowInfo* win, const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr searchText;
    BOOL matchCase = 0;
    const WCHAR* next = str::Parse(cmd, L"[TextSearch(\"%S\",%u)]", &searchText, &matchCase);
    if (!next) return nullptr;

    DisplayModel* dm = win->AsFixed();
    if (dm) {
        ClearSearchResult(win);
        win::SetText(win->hwndFindBox, searchText);
        Edit_SetModify(win->hwndFindBox, TRUE);
        dm->textSearch->SetSensitive(matchCase);
        FindTextOnThread(win, TextSearchDirection::Forward, true);
    }

    ack.fAck = 1;
    return next;
}

// DDE command  : Repeats same text search (Forward or Backward), including same match case.
// Note         : Needs an initial text search command using Sumatra interface or "TextSearch" DDE command
// Format       : [TextSearchNext(<Forward>])]
// Note         : Use <Forward> = 1 for Search Forward or <Forward> = 0 for Backward
// eg.          : [TextSearchNext(1)]
static const WCHAR* HandleTextSearchNextCmd(WindowInfo* win, const WCHAR* cmd, DDEACK& ack) {
    BOOL direction = 0;
    const WCHAR* next = str::Parse(cmd, L"[TextSearchNext(%u)]", &direction);

    if (!next) return nullptr;

    FindTextOnThread(win, direction ? TextSearchDirection::Forward : TextSearchDirection::Backward, true);

    ack.fAck = 1;
    return next;
}

void HandlePluginCmds(HWND hwnd, const WCHAR* cmd, DDEACK& ack) {
    if (str::IsEmpty(cmd)) {
        return;
    }

    WindowInfo* win = FindWindowInfoByHwnd(hwnd);
    if (!win) return;

    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return;
        }
    }

    {
        AutoFree tmp = strconv::WstrToUtf8(cmd);
        logf("HandlePluginCmds: '%s'\n", tmp.Get());
    }

    while (!str::IsEmpty(cmd)) {
        const WCHAR* nextCmd = nextCmd = HandleTextSearchCmd(win, cmd, ack);
        if (!nextCmd) {
            nextCmd = HandleTextSearchNextCmd(win, cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleGetPropertyCmd(win, cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleSetPropertyCmd(win, cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleOpenFileCmd(win, cmd, ack);
        }
        if (!nextCmd) {
            AutoFreeWstr tmp;
            nextCmd = str::Parse(cmd, L"%S]", &tmp);
        }
        cmd = nextCmd;

        {
            AutoFree tmp = strconv::WstrToUtf8(cmd);
            logf("HandlePluginCmds: cmd='%s'\n", tmp.Get());
        }
    }
}
