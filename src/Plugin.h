#pragma once
void HandlePluginCmds(HWND hwnd, const WCHAR* cmd, DDEACK& ack);
LRESULT PluginHostCopyData(WindowInfo* win, const WCHAR* msg, ...);
void MakePluginWindow(WindowInfo* win, HWND hwndParent);
