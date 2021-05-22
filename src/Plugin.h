#pragma once
void HandlePluginCmds(HWND hwnd, const WCHAR* cmd, DDEACK& ack);
LRESULT PluginHostCopyData(WindowInfo* win, const WCHAR* msg, ...);
void MakePluginWindow(WindowInfo* win, HWND hwndParent);
LRESULT SendPluginWndProcMessage(WindowInfo* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
