// NppMarkdownPreview.cpp - native Notepad++ markdown preview plugin (ARM64 / x64).
//
// Renders the current Markdown buffer in a dockable panel using WebView2
// (Chromium/Edge) + marked.js, with highlight.js code highlighting and
// Mermaid diagrams. Pure Win32 + COM, no .NET, no ATL/MFC.
//
// Plugin license: GPLv3 (interface headers vendored from the GPLv3 Notepad++ tree).
// marked.js / highlight.js: MIT   |   Mermaid: MIT

#include <windows.h>
#include <commdlg.h>
#include <string>
#include "PluginInterface.h"
#include "Docking.h"
#include "WebView2Preview.h"
#include "resource.h"

#pragma comment(lib, "comdlg32.lib")

#define TIMER_RENDER      1
#define TIMER_SCROLLSYNC  2
#define MD_MAX_BYTES      (10 * 1024 * 1024)

HINSTANCE g_hInst = nullptr;

static NppData g_npp = {};
static FuncItem g_funcItems[8] = {};

static HWND g_panel = nullptr;
static bool g_panelCreated = false;

static std::string g_lastMarkdown;
static bool g_hintShown = false;

// scroll-sync state
static bool g_syncingFromPreview = false;   // guards echo
static DWORD g_lastEditorScrollTick = 0;

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

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Escape UTF-8 text into the body of a JSON string literal (quotes excluded).
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

// ---------------------------------------------------------------------------
// asset extraction: mermaid/hljs/marked are too large for one NavigateToString,
// so they are written next to the WebView2 cache and served via nppmdp.local
// ---------------------------------------------------------------------------

static std::wstring AssetsDir() {
    wchar_t localAppData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring dir = std::wstring(localAppData) + L"\\NppMarkdownPreview\\assets";
    return dir;
}

static bool WriteAsset(const std::wstring& dir, const wchar_t* name, const std::string& data) {
    if (data.empty()) return false;
    std::wstring path = dir + L"\\" + name;
    // Skip rewrite when the size already matches (fast path across launches).
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ULONGLONG existing = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (existing == data.size()) return true;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

static void ExtractAssets() {
    std::wstring base = AssetsDir();
    std::wstring parent = base.substr(0, base.find_last_of(L'\\'));
    CreateDirectoryW(parent.c_str(), nullptr);   // WebView2 creates the leaf but not parents
    CreateDirectoryW(base.c_str(), nullptr);

    struct Asset { const wchar_t* name; UINT id; };
    const Asset assets[] = {
        { L"marked.umd.js",        IDR_MARKED_JS },
        { L"highlight.min.js",     IDR_HIGHLIGHT_JS },
        { L"github.min.css",       IDR_HLJS_LIGHT_CSS },
        { L"github-dark.min.css",  IDR_HLJS_DARK_CSS },
        { L"mermaid.min.js",       IDR_MERMAID_JS },
    };
    for (const auto& a : assets) {
        std::string data = LoadRes(a.id);
        bool ok = WriteAsset(base, a.name, data);
        WvLog(L"Asset %s: %u bytes written=%d", a.name, (unsigned)data.size(), ok ? 1 : 0);
    }
    wv2::g_assetsDir = base;
}

static void BuildPreviewPage() {
    std::string html = LoadRes(IDR_PREVIEW_HTML);
    std::string css = LoadRes(IDR_STYLE_CSS);
    WvLog(L"Resources: html=%u css=%u", (unsigned)html.size(), (unsigned)css.size());
    ReplaceAll(html, "__PLUGIN_CSS__", css);
    // Served from the asset host so the page is same-origin with its scripts.
    WriteAsset(wv2::g_assetsDir, L"preview.html", html);
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

static HWND CurrentScintilla() {
    int which = 0;
    SendMessageW(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
    return (which == 1) ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

static std::wstring CurrentPath() {
    wchar_t path[MAX_PATH] = {};
    SendMessageW(g_npp._nppHandle, NPPM_GETFULLCURRENTPATH, (WPARAM)MAX_PATH, (LPARAM)path);
    return path;
}

static bool GetCurrentMarkdown(std::string& out) {
    std::wstring path = CurrentPath();
    if (!IsMarkdownPath(path.c_str())) return false;

    HWND sci = CurrentScintilla();
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
    // ask the window system - the dock X button hides the panel without telling us
    if (g_panel && IsWindowVisible(g_panel)) SetTimer(g_panel, TIMER_RENDER, 400, nullptr);
}

static void RenderNow() {
    if (!wv2::IsReady()) {
        WvLog(L"RenderNow skipped: webview not ready");
        return;
    }

    std::string md;
    if (GetCurrentMarkdown(md)) {
        g_hintShown = false;
        if (md == g_lastMarkdown) return;   // unchanged
        g_lastMarkdown = md;
        WvLog(L"RenderNow: rendering %u bytes", (unsigned)md.size());
        bool ok = wv2::ExecScript(L"window.__update(\"" + JsonEscapeToWide(md) + L"\");");
        WvLog(L"RenderNow: ExecuteScript %s", ok ? L"S_OK" : L"FAILED");
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

// preview -> editor: called from the WebView2 web-message handler
void OnPreviewScroll(int docLine) {
    if (!g_panel || !IsWindowVisible(g_panel)) return;
    if (!IsMarkdownPath(CurrentPath().c_str())) return;
    HWND sci = CurrentScintilla();
    // avoid echoing right back while the editor is the one scrolling
    if (GetTickCount() - g_lastEditorScrollTick < 200) return;
    int shown = (int)SendMessageW(sci, SCI_VISIBLEFROMDOCLINE, (WPARAM)docLine, 0);
    if (shown < 0) return;
    g_syncingFromPreview = true;
    SendMessageW(sci, SCI_SETFIRSTVISIBLELINE, (WPARAM)shown, 0);
    g_syncingFromPreview = false;
}

// editor -> preview
static void SyncEditorToPreview() {
    if (g_syncingFromPreview) return;
    if (!g_panel || !IsWindowVisible(g_panel) || !wv2::IsReady()) return;
    if (!IsMarkdownPath(CurrentPath().c_str())) return;
    HWND sci = CurrentScintilla();
    int firstShown = (int)SendMessageW(sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
    int docLine = (int)SendMessageW(sci, SCI_DOCLINEFROMVISIBLE, (WPARAM)firstShown, 0);
    if (docLine < 0) return;
    g_lastEditorScrollTick = GetTickCount();
    wchar_t js[128];
    swprintf_s(js, L"window.__scrollToLine(%d);", docLine);
    wv2::ExecScript(js);
}

// ---------------------------------------------------------------------------
// dockable panel
// ---------------------------------------------------------------------------

static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            ExtractAssets();
            BuildPreviewPage();
            wv2::Init(hwnd, std::wstring(), L"http://nppmdp.local/preview.html");
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
            } else if (wParam == TIMER_SCROLLSYNC) {
                KillTimer(hwnd, TIMER_SCROLLSYNC);
                SyncEditorToPreview();
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
}

// ---------------------------------------------------------------------------
// file dialogs / export
// ---------------------------------------------------------------------------

static bool AskSavePath(const wchar_t* filter, const wchar_t* defExt,
                        const std::wstring& initialName, std::wstring& out) {
    wchar_t buf[MAX_PATH] = {};
    if (!initialName.empty()) wcsncpy_s(buf, initialName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_npp._nppHandle;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

static std::wstring BaseNameNoExt() {
    std::wstring path = CurrentPath();
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name.empty() ? L"document" : name;
}

static void WriteFileUtf8(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // strip UTF-8 BOM
    const char* p = utf8.data();
    DWORD n = (DWORD)utf8.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    DWORD written = 0;
    WriteFile(h, p, n, &written, nullptr);
    CloseHandle(h);
}

static void cmdExportHtml() {
    if (!wv2::IsReady()) {
        MessageBoxW(g_npp._nppHandle, L"Open the Markdown preview first.", L"NppMarkdownPreview",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring json;
    if (!wv2::ExecScriptSync(L"window.__getHtml()", json) || json.size() < 2) {
        MessageBoxW(g_npp._nppHandle, L"Nothing to export (no rendered content).", L"NppMarkdownPreview",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    // ExecuteScript returns a JSON-encoded string literal; unescape it.
    std::wstring body;
    for (size_t i = 1; i + 1 < json.size(); ++i) {
        wchar_t c = json[i];
        if (c == L'\\' && i + 2 < json.size()) {
            wchar_t e = json[++i];
            switch (e) {
                case L'n': body += L'\n'; break;
                case L'r': body += L'\r'; break;
                case L't': body += L'\t'; break;
                case L'b': body += L'\b'; break;
                case L'f': body += L'\f'; break;
                case L'u':
                    if (i + 4 < json.size()) {
                        wchar_t hex[5] = { json[i+1], json[i+2], json[i+3], json[i+4], 0 };
                        body += (wchar_t)wcstoul(hex, nullptr, 16);
                        i += 4;
                    }
                    break;
                default: body += e;
            }
        } else {
            body += c;
        }
    }
    if (body.empty()) {
        MessageBoxW(g_npp._nppHandle, L"Nothing to export (no rendered content).", L"NppMarkdownPreview",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring outPath;
    std::wstring defaultName = BaseNameNoExt() + L".html";
    if (!AskSavePath(L"HTML Files (*.html)\0*.html\0All Files (*.*)\0*.*\0", L"html", defaultName, outPath))
        return;

    std::string css = LoadRes(IDR_STYLE_CSS);
    std::string hljsLight = LoadRes(IDR_HLJS_LIGHT_CSS);
    std::string hljsDark = LoadRes(IDR_HLJS_DARK_CSS);
    std::string mermaid = LoadRes(IDR_MERMAID_JS);

    std::string doc;
    doc += "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
    doc += "<title>" + WideToUtf8(BaseNameNoExt()) + "</title>\n";
    doc += "<style>\n" + css + "\n</style>\n";
    doc += "<style media=\"(prefers-color-scheme: light)\">\n" + hljsLight + "\n</style>\n";
    doc += "<style media=\"(prefers-color-scheme: dark)\">\n" + hljsDark + "\n</style>\n";
    doc += "</head>\n<body>\n<article class=\"content\">\n";
    doc += WideToUtf8(body);
    doc += "\n</article>\n";
    // re-run mermaid on the exported copy so diagrams render without JS tooling
    doc += "<script>\n" + mermaid + "\n</script>\n";
    doc += "<script>\n"
           "if(window.mermaid){try{mermaid.initialize({startOnLoad:true,securityLevel:'strict',"
           "theme:matchMedia('(prefers-color-scheme: dark)').matches?'dark':'default'});}catch(e){}}\n"
           "</script>\n</body>\n</html>\n";

    WriteFileUtf8(outPath, doc);
    MessageBoxW(g_npp._nppHandle, (L"HTML exported:\n" + outPath).c_str(), L"NppMarkdownPreview",
                MB_OK | MB_ICONINFORMATION);
}

static void cmdExportPdf() {
    if (!wv2::IsReady()) {
        MessageBoxW(g_npp._nppHandle, L"Open the Markdown preview first.", L"NppMarkdownPreview",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring outPath;
    std::wstring defaultName = BaseNameNoExt() + L".pdf";
    if (!AskSavePath(L"PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0", L"pdf", defaultName, outPath))
        return;
    if (!wv2::PrintToPdf(g_npp._nppHandle, outPath)) {
        MessageBoxW(g_npp._nppHandle, L"PDF export could not be started.", L"NppMarkdownPreview",
                    MB_OK | MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------------------
// menu commands
// ---------------------------------------------------------------------------

static void cmdTogglePreview() {
    if (!g_panelCreated) {
        EnsurePanel();        // create, register and show - no toggle on first call
        ScheduleRefresh();
        return;
    }
    if (IsWindowVisible(g_panel)) {
        SendMessageW(g_npp._nppHandle, NPPM_DMMHIDE, 0, (LPARAM)g_panel);
    } else {
        SendMessageW(g_npp._nppHandle, NPPM_DMMSHOW, 0, (LPARAM)g_panel);
        g_lastMarkdown.clear();   // force re-render
        ScheduleRefresh();
    }
}

static void cmdRefreshPreview() {
    if (!g_panelCreated || !IsWindowVisible(g_panel)) return;
    g_lastMarkdown.clear();
    RenderNow();
}

static void cmdZoomIn()  { wv2::ZoomBy(1.1); }
static void cmdZoomOut() { wv2::ZoomBy(1.0 / 1.1); }
static void cmdZoomReset() { wv2::ZoomReset(); }

static void cmdAbout() {
    MessageBoxW(g_npp._nppHandle,
                L"NppMarkdownPreview 1.1.0\r\n\r\n"
                L"Native ARM64 / x64 Markdown preview for Notepad++.\r\n"
                L"Rendering: WebView2 (Edge) + marked.js\r\n"
                L"Code highlighting: highlight.js\r\n"
                L"Diagrams: Mermaid\r\n"
                L"Features: live preview, two-way scroll sync,\r\n"
                L"          zoom, export HTML/PDF\r\n\r\n"
                L"Plugin: GPLv3   |   marked/hljs/mermaid: MIT",
                L"NppMarkdownPreview", MB_OK | MB_ICONINFORMATION);
}

static void InitFuncItems() {
    const wchar_t* names[8] = {
        L"Toggle Markdown Preview",
        L"Refresh Preview",
        L"Zoom In\tCtrl+Alt+=",
        L"Zoom Out\tCtrl+Alt+-",
        L"Zoom Reset\tCtrl+Alt+0",
        L"Export HTML...",
        L"Export PDF...",
        L"About...",
    };
    PFUNCPLUGINCMD funcs[8] = {
        cmdTogglePreview, cmdRefreshPreview,
        cmdZoomIn, cmdZoomOut, cmdZoomReset,
        cmdExportHtml, cmdExportPdf, cmdAbout,
    };
    for (int i = 0; i < 8; ++i) {
        wcscpy_s(g_funcItems[i]._itemName, names[i]);
        g_funcItems[i]._pFunc = funcs[i];
    }
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
    *nbFuncItem = 8;
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
            if ((scn->updated & SC_UPDATE_V_SCROLL) && g_panel && IsWindowVisible(g_panel))
                SetTimer(g_panel, TIMER_SCROLLSYNC, 60, nullptr);
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
