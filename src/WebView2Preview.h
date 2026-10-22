// WebView2Preview.h - WebView2 layer for the preview panel.
// No ATL/MFC; interface calls go through vtables only and WebView2Loader.dll
// is resolved with LoadLibrary + GetProcAddress (nothing from WebView2Loader.lib
// needs to be linked). Assets (marked/hljs/mermaid) are extracted to disk and
// served through the virtual host http://nppmdp.local/.
#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstdarg>
#include <wrl/client.h>
#include "WebView2.h"

#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

extern HINSTANCE g_hInst;   // defined in NppMarkdownPreview.cpp
void OnWebViewReady();      // implemented in NppMarkdownPreview.cpp
void OnPreviewScroll(int docLine);  // implemented in NppMarkdownPreview.cpp

namespace wv2 {

inline ComPtr<ICoreWebView2Environment> g_env;
inline ComPtr<ICoreWebView2Controller> g_controller;
inline ComPtr<ICoreWebView2> g_webview;
inline bool g_ready = false;
inline std::wstring g_assetsDir;   // set by the plugin before Init()

// Append-only trace log at %LOCALAPPDATA%\NppMarkdownPreview\debug.log (UTF-16LE).
inline void WvLog(const wchar_t* fmt, ...) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t ts[40];
    swprintf_s(ts, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    wchar_t msg[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);
    wchar_t dir[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    std::wstring path = std::wstring(dir) + L"\\NppMarkdownPreview\\debug.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, ts, (DWORD)(wcslen(ts) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(h, msg, (DWORD)(wcslen(msg) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(h, L"\r\n", 4, &written, nullptr);
    CloseHandle(h);
}

inline std::wstring HexCode(HRESULT hr) {
    wchar_t buf[16];
    swprintf_s(buf, L"%08X", (unsigned)hr);
    return buf;
}

// Simple reference-counted base for the COM callback handlers.
template <typename I>
class RefCounted : public I {
    LONG m_refs = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(I) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refs); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return r;
    }
};

class NavHandler final : public RefCounted<ICoreWebView2NavigationCompletedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        BOOL ok = FALSE;
        if (args) args->get_IsSuccess(&ok);
        WvLog(L"NavigationCompleted: success=%d", (int)ok);
        g_ready = true;
        OnWebViewReady();
        return S_OK;
    }
};

class ExecHandler final : public RefCounted<ICoreWebView2ExecuteScriptCompletedHandler> {
public:
    // SDK 1.0.4258.31: Invoke(HRESULT errorCode, LPCWSTR result)
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT /*errorCode*/, LPCWSTR /*result*/) override {
        return S_OK;
    }
};

// ExecuteScript with captured result + event wait (message-pumped on the UI thread).
class ExecCaptureHandler final : public RefCounted<ICoreWebView2ExecuteScriptCompletedHandler> {
public:
    HANDLE done = nullptr;
    bool ok = false;
    std::wstring result;
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR jsonResult) override {
        ok = SUCCEEDED(errorCode);
        if (jsonResult) result = jsonResult;
        if (done) SetEvent(done);
        return S_OK;
    }
};

// page scroll message: "scroll:<docLine>"
class WebMessageHandler final : public RefCounted<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        PWSTR msg = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
            if (wcsncmp(msg, L"scroll:", 7) == 0) {
                int line = _wtoi(msg + 7);
                if (line >= 0) OnPreviewScroll(line);
            }
            CoTaskMemFree(msg);
        }
        return S_OK;
    }
};

// open external http(s) links in the system browser, keep nppmdp.local internal
inline bool IsInternalUrl(const wchar_t* uri) {
    static const wchar_t* kBase = L"http://nppmdp.local/";
    return wcsncmp(uri, kBase, wcslen(kBase)) == 0;
}

class NavStartingHandler final : public RefCounted<ICoreWebView2NavigationStartingEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2NavigationStartingEventArgs* args) override {
        PWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
            bool external = (wcsncmp(uri, L"http://", 7) == 0 || wcsncmp(uri, L"https://", 8) == 0) &&
                            !IsInternalUrl(uri);
            if (external) {
                WvLog(L"External link -> browser: %s", uri);
                ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                args->put_Cancel(TRUE);
            } else {
                WvLog(L"In-page navigation: %s", uri);
            }
            CoTaskMemFree(uri);
        }
        return S_OK;
    }
};

class NewWindowHandler final : public RefCounted<ICoreWebView2NewWindowRequestedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/,
                                     ICoreWebView2NewWindowRequestedEventArgs* args) override {
        PWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
            ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
            CoTaskMemFree(uri);
        }
        args->put_Handled(TRUE);
        return S_OK;
    }
};

class ControllerHandler final : public RefCounted<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HWND parent = nullptr;
    std::wstring html;   // inline HTML (used when url is empty)
    std::wstring url;    // preferred: navigate to this URL (same-origin with the asset host)

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* createdController) override {
        if (FAILED(errorCode) || !createdController) {
            MessageBoxW(parent,
                        (L"WebView2 controller creation failed.\nHRESULT: 0x" + HexCode(errorCode)).c_str(),
                        L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
            return S_OK;
        }
        g_controller = createdController;
        if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || !g_webview) return S_OK;

        RECT rc = {};
        bool hasRc = (parent && GetClientRect(parent, &rc));
        WvLog(L"Controller created: rect=(%ld,%ld,%ld,%ld)", rc.left, rc.top, rc.right, rc.bottom);
        if (hasRc) {
            g_controller->put_Bounds(rc);
        }

        // serve assets from disk through http://nppmdp.local/
        ComPtr<ICoreWebView2_3> wv3;
        if (!g_assetsDir.empty() && SUCCEEDED(g_webview.As(&wv3)) && wv3) {
            HRESULT mhr = wv3->SetVirtualHostNameToFolderMapping(
                L"nppmdp.local", g_assetsDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            WvLog(L"VirtualHostMapping: hr=0x%s dir=%s", HexCode(mhr).c_str(), g_assetsDir.c_str());
        }

        EventRegistrationToken tok = {};
        g_webview->add_WebMessageReceived(new WebMessageHandler(), &tok);
        g_webview->add_NavigationStarting(new NavStartingHandler(), &tok);
        g_webview->add_NewWindowRequested(new NewWindowHandler(), &tok);

        ComPtr<NavHandler> nav(new NavHandler());
        EventRegistrationToken navToken = {};
        g_webview->add_NavigationCompleted(nav.Get(), &navToken);
        HRESULT navHr;
        if (!url.empty()) {
            navHr = g_webview->Navigate(url.c_str());
            WvLog(L"Navigate: hr=0x%s url=%s", HexCode(navHr).c_str(), url.c_str());
        } else {
            navHr = g_webview->NavigateToString(html.c_str());
            WvLog(L"NavigateToString: hr=0x%s htmlLen=%u", HexCode(navHr).c_str(), (unsigned)html.size());
        }
        if (FAILED(navHr)) {
            MessageBoxW(parent, (L"Navigation failed.\nHRESULT: 0x" + HexCode(navHr)).c_str(),
                        L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
        }
        return S_OK;
    }
};

class EnvironmentHandler final : public RefCounted<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HWND parent = nullptr;
    std::wstring html;
    std::wstring url;

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* createdEnvironment) override {
        if (FAILED(errorCode) || !createdEnvironment) {
            MessageBoxW(parent,
                        (L"WebView2 runtime is not available.\nHRESULT: 0x" + HexCode(errorCode) +
                        L"\n\nPlease install the Microsoft Edge WebView2 Runtime (Evergreen)\n"
                        L"and restart Notepad++.").c_str(),
                        L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
            return S_OK;
        }
        g_env = createdEnvironment;
        WvLog(L"Environment created OK");
        auto* ch = new ControllerHandler();
        ch->parent = parent;
        ch->html = html;
        ch->url = url;
        createdEnvironment->CreateCoreWebView2Controller(parent, ch);
        ch->Release();
        return S_OK;
    }
};

// ---- PDF export ----
class PrintPdfHandler final : public RefCounted<ICoreWebView2PrintToPdfCompletedHandler> {
public:
    HWND owner = nullptr;
    std::wstring path;
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, BOOL result) override {
        if (SUCCEEDED(errorCode) && result) {
            MessageBoxW(owner, (L"PDF saved:\n" + path).c_str(), L"NppMarkdownPreview", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(owner, (L"PDF export failed.\nHRESULT: 0x" + HexCode(errorCode)).c_str(),
                        L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
        }
        return S_OK;
    }
};

typedef HRESULT(WINAPI* PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

inline HMODULE g_loader = nullptr;

inline bool Init(HWND parent, const std::wstring& bootstrapHtml, const std::wstring& url = L"") {
    if (g_webview) return true;   // already initialized

    // WebView2Loader.dll is shipped next to this plugin DLL.
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, selfPath, MAX_PATH);
    const wchar_t* slash = wcsrchr(selfPath, L'\\');
    std::wstring loader(selfPath);
    if (slash) loader.resize((size_t)(slash - selfPath) + 1);   // directory incl. trailing backslash
    loader += L"WebView2Loader.dll";
    g_loader = LoadLibraryW(loader.c_str());
    if (!g_loader) g_loader = LoadLibraryW(L"WebView2Loader.dll");
    WvLog(L"Init: loader='%s' loaded=%d", loader.c_str(), g_loader ? 1 : 0);
    if (!g_loader) {
        MessageBoxW(parent, L"WebView2Loader.dll was not found next to NppMarkdownPreview.dll.",
                    L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
        return false;
    }
    auto create = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
        GetProcAddress(g_loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (!create) return false;

    wchar_t localAppData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring base = std::wstring(localAppData) + L"\\NppMarkdownPreview";
    CreateDirectoryW(base.c_str(), nullptr);            // parent must exist,
    std::wstring userData = base + L"\\WebView2Cache";  // WebView2 creates the leaf but not parents
    CreateDirectoryW(userData.c_str(), nullptr);        // may already exist

    auto* eh = new EnvironmentHandler();
    eh->parent = parent;
    eh->html = bootstrapHtml;
    eh->url = url;
    HRESULT hr = create(nullptr, userData.c_str(), nullptr, eh);
    eh->Release();
    WvLog(L"Init: CreateEnvironment hr=0x%s userData='%s'", HexCode(hr).c_str(), userData.c_str());
    if (FAILED(hr)) {
        MessageBoxW(parent,
                    (L"CreateCoreWebView2Environment failed immediately.\nHRESULT: 0x" + HexCode(hr)).c_str(),
                    L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

inline void Resize(const RECT& rc) {
    if (g_controller) g_controller->put_Bounds(rc);
}

inline bool IsReady() { return g_ready && g_webview; }

inline bool ExecScript(const std::wstring& js) {
    if (!g_webview) return false;
    auto* h = new ExecHandler();
    HRESULT hr = g_webview->ExecuteScript(js.c_str(), h);
    h->Release();
    if (FAILED(hr)) WvLog(L"ExecuteScript: hr=0x%s len=%u", HexCode(hr).c_str(), (unsigned)js.size());
    return SUCCEEDED(hr);
}

// Run a script and synchronously capture its JSON-encoded result.
inline bool ExecScriptSync(const std::wstring& js, std::wstring& jsonResult) {
    if (!g_webview) return false;
    auto* h = new ExecCaptureHandler();
    h->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = g_webview->ExecuteScript(js.c_str(), h);
    if (FAILED(hr)) {
        CloseHandle(h->done);
        h->Release();
        WvLog(L"ExecScriptSync: hr=0x%s", HexCode(hr).c_str());
        return false;
    }
    while (WaitForSingleObject(h->done, 50) == WAIT_TIMEOUT) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    bool ok = h->ok;
    jsonResult = h->result;
    CloseHandle(h->done);
    h->Release();
    return ok;
}

inline void ZoomBy(double factor) {
    if (!g_controller) return;
    double z = 1.0;
    g_controller->get_ZoomFactor(&z);
    z *= factor;
    if (z < 0.25) z = 0.25;
    if (z > 5.0) z = 5.0;
    g_controller->put_ZoomFactor(z);
}

inline void ZoomReset() {
    if (g_controller) g_controller->put_ZoomFactor(1.0);
}

inline bool PrintToPdf(HWND owner, const std::wstring& path) {
    if (!g_webview) return false;
    ComPtr<ICoreWebView2_7> wv7;
    if (FAILED(g_webview.As(&wv7)) || !wv7) return false;
    ComPtr<ICoreWebView2Environment6> env6;
    if (FAILED(g_env.As(&env6)) || !env6) return false;
    ComPtr<ICoreWebView2PrintSettings> settings;
    if (FAILED(env6->CreatePrintSettings(&settings)) || !settings) return false;
    settings->put_PageWidth(8.5);
    settings->put_PageHeight(11.0);
    settings->put_Orientation(COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
    settings->put_ShouldPrintBackgrounds(TRUE);
    settings->put_ShouldPrintHeaderAndFooter(FALSE);
    auto* h = new PrintPdfHandler();
    h->owner = owner;
    h->path = path;
    HRESULT hr = wv7->PrintToPdf(path.c_str(), settings.Get(), h);
    h->Release();
    WvLog(L"PrintToPdf: hr=0x%s path=%s", HexCode(hr).c_str(), path.c_str());
    return SUCCEEDED(hr);
}

inline void Shutdown() {
    g_ready = false;
    g_webview.Reset();
    if (g_controller) {
        g_controller->Close();
        g_controller.Reset();
    }
    g_env.Reset();
}

} // namespace wv2
