// NppMarkdownPreview.cpp - native Notepad++ markdown preview plugin (ARM64 / x64).
//
// Renders the current Markdown buffer in a dockable panel using WebView2
// (Chromium/Edge) + marked.js. Pure Win32 + COM, no .NET, no ATL/MFC.
//
// Plugin license: GPLv3 (interface headers vendored from the GPLv3 Notepad++ tree).
// marked.js is MIT licensed.

#include <windows.h>
#include <string>
#include "PluginInterface.h"
#include "Docking.h"
#include "WebView2Preview.h"
#include "resource.h"

#define TIMER_RENDER   1
#define MD_MAX_BYTES   (10 * 1024 * 1024)

HINSTANCE g_hInst = nullptr;

static NppData g_npp = {};
static FuncItem g_funcItems[3] = {};

static HWND g_panel = nullptr;
static bool g_panelCreated = false;
static bool g_panelVisible = false;

static std::string g_lastMarkdown;
static bool g_hintShown = false;

// ---------------------------------------------------------------------------
// resource loading / encoding helpers
// ---------------------------------------------------------------------------

static std::string LoadRes(UINT id) {
    HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hRes) return std::string();
    HGLOBAL hGlob = LoadResource(g_hInst, hRes);
    if (!hGlob) return std::string();
    const char* pData = static_cast<const char*>(LockResource(hGlob));
    DWORD size = SizeofResource(g_hInst, hRes);
    if (!pData || !size) return std::string();
    return std::string(pData, size);
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Escape UTF-8 markdown text into the body of a JSON string literal (quotes
// excluded), widened for ICoreWebView2::ExecuteScript.
static std::wstring JsonEscapeToWide(const std::string& utf8) {
    static const char* kHex = "0123456789abcdef";
    std::string esc;
    esc.reserve(utf8.size() + 16);
    for (size_t i = 0; i < utf8.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        switch (c) {
            case '"':  esc += "\\\""; break;
            case '\\': esc += "\\\\"; break;
            case '\b': esc += "\\b";  break;
            case '\f': esc += "\\f";  break;
            case '\n': esc += "\\n";  break;
            case '\r': esc += "\\r";  break;
            case '\t': esc += "\\t";  break;
            default:
                if (c < 0x20) {
                    esc += "\\u00";
                    esc += kHex[c >> 4];
                    esc += kHex[c & 0xF];
                } else {
                    esc += static_cast<char>(c);
                }
        }
    }
    return Utf8ToWide(esc);
}

static void ReplaceAll(std::string& s, const std::string& token, const std::string& value) {
    if (token.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, token.size(), value);
        pos += value.size();
    }
}

static std::wstring BuildBootstrapHtml() {
    std::string html = LoadRes(IDR_PREVIEW_HTML);
    std::string css = LoadRes(IDR_STYLE_CSS);
    std::string marked = LoadRes(IDR_MARKED_JS);
    ReplaceAll(html, "__PLUGIN_MARKED__", marked);
    ReplaceAll(html, "__PLUGIN_CSS__", css);
    return Utf8ToWide(html);
}

// ---------------------------------------------------------------------------
// markdown extraction from the active buffer
// ---------------------------------------------------------------------------

static bool IsMarkdownPath(const wchar_t* path) {
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".md") == 0 || _wcsicmp(ext, L".markdown") == 0 ||
            _wcsicmp(ext, L".mdown") == 0 || _wcsicmp(ext, L".mkd") == 0);
}

static bool GetCurrentMarkdown(std::string& out) {
    wchar_t path[MAX_PATH] = {};
    SendMessageW(g_npp._nppHandle, NPPM_GETFULLCURRENTPATH, (WPARAM)MAX_PATH, (LPARAM)path);
    if (!IsMarkdownPath(path)) return false;

    int which = 0;
    SendMessageW(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
    HWND sci = (which == 1) ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;

    LRESULT len = SendMessageW(sci, SCI_GETLENGTH, 0, 0);
    if (len <= 0) {
        out.clear();
        return true;
    }
    if (len > MD_MAX_BYTES) len = MD_MAX_BYTES;

    out.assign((size_t)len + 1, '\0');
    SendMessageW(sci, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)&out[0]);
    out.resize((size_t)len);
    return true;
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

static void ScheduleRefresh() {
    if (g_panel && g_panelVisible) SetTimer(g_panel, TIMER_RENDER, 400, nullptr);
}

static void RenderNow() {
    if (!wv2::IsReady()) return;

    std::string md;
    if (GetCurrentMarkdown(md)) {
        g_hintShown = false;
        if (md == g_lastMarkdown) return;   // unchanged
        g_lastMarkdown = md;
        wv2::ExecScript(L"window.__update(\"" + JsonEscapeToWide(md) + L"\");");
    } else {
        g_lastMarkdown.clear();
        if (!g_hintShown) {
            g_hintShown = true;
            wv2::ExecScript(L"window.__setStatus(\"This buffer is not a Markdown file (.md)\");");
        }
    }
}

void OnWebViewReady() {
    RenderNow();
}

// ---------------------------------------------------------------------------
// dockable panel
// ---------------------------------------------------------------------------

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            wv2::Init(hwnd, BuildBootstrapHtml());
            return 0;
        case WM_SIZE: {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            wv2::Resize(rc);
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_RENDER) {
                KillTimer(hwnd, TIMER_RENDER);
                RenderNow();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;   // the WebView2 controller paints the whole client area
        case WM_DESTROY:
            wv2::Shutdown();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsurePanel() {
    if (g_panelCreated) return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NppMarkdownPreviewPanel";
    RegisterClassW(&wc);

    g_panel = CreateWindowExW(0, L"NppMarkdownPreviewPanel", L"Markdown Preview",
                              WS_CHILD, 0, 0, 0, 0,
                              g_npp._nppHandle, nullptr, g_hInst, nullptr);

    tTbData tb = {};
    tb.hClient = g_panel;
    tb.pszName = L"Markdown Preview";
    tb.dlgID = g_funcItems[0]._cmdID;
    tb.uMask = DWS_DF_CONT_RIGHT;            // dock right on first run
    tb.pszModuleName = L"NppMarkdownPreview.dll";
    tb.iPrevCont = -1;
    SendMessageW(g_npp._nppHandle, NPPM_DMMREGASDCKDLG, 0, (LPARAM)&tb);

    g_panelCreated = true;
    g_panelVisible = true;
}

// ---------------------------------------------------------------------------
// menu commands
// ---------------------------------------------------------------------------

static void cmdTogglePreview() {
    EnsurePanel();
    if (g_panelVisible) {
        SendMessageW(g_npp._nppHandle, NPPM_DMMHIDE, 0, (LPARAM)g_panel);
        g_panelVisible = false;
    } else {
        SendMessageW(g_npp._nppHandle, NPPM_DMMSHOW, 0, (LPARAM)g_panel);
        g_panelVisible = true;
        g_lastMarkdown.clear();   // force re-render
        ScheduleRefresh();
    }
}

static void cmdRefreshPreview() {
    if (!g_panelCreated || !g_panelVisible) return;
    g_lastMarkdown.clear();
    RenderNow();
}

static void cmdAbout() {
    MessageBoxW(g_npp._nppHandle,
                L"NppMarkdownPreview 1.0.0\r\n\r\n"
                L"Native ARM64 / x64 Markdown preview for Notepad++.\r\n"
                L"Rendering: WebView2 (Edge) + marked.js\r\n\r\n"
                L"Plugin: GPLv3   |   marked.js: MIT",
                L"NppMarkdownPreview", MB_OK | MB_ICONINFORMATION);
}

static void InitFuncItems() {
    wcscpy_s(g_funcItems[0]._itemName, L"Toggle Markdown Preview");
    g_funcItems[0]._pFunc = cmdTogglePreview;
    wcscpy_s(g_funcItems[1]._itemName, L"Refresh Preview");
    g_funcItems[1]._pFunc = cmdRefreshPreview;
    wcscpy_s(g_funcItems[2]._itemName, L"About...");
    g_funcItems[2]._pFunc = cmdAbout;
}

// ---------------------------------------------------------------------------
// Notepad++ plugin exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void setInfo(NppData notepadPlusData) {
    g_npp = notepadPlusData;
}

extern "C" __declspec(dllexport) const wchar_t* getName() {
    return L"NppMarkdownPreview";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbFuncItem) {
    if (!nbFuncItem) return nullptr;
    *nbFuncItem = 3;
    return g_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* scn) {
    if (!scn) return;
    switch (scn->nmhdr.code) {
        case NPPN_BUFFERACTIVATED:
        case NPPN_FILESAVED:
            ScheduleRefresh();
            break;
        case SCN_UPDATEUI:
            if (scn->updated & SC_UPDATE_CONTENT)
                ScheduleRefresh();
            break;
        case NPPN_SHUTDOWN:
            if (g_panelCreated) DestroyWindow(g_panel);
            break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    return 0;
}

extern "C" __declspec(dllexport) BOOL isUnicode() {
    return TRUE;
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID /*lpReserved*/) {
    if (reasonForCall == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        InitFuncItems();
    }
    return TRUE;
}
