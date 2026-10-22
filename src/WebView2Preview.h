// WebView2Preview.h - minimal WebView2 bootstrap for the preview panel.
// No ATL/MFC required; interface calls go through the vtable only and the
// loader DLL is resolved with LoadLibrary + GetProcAddress, so nothing from
// WebView2Loader.lib needs to be linked.
#pragma once

#include <windows.h>
#include <string>
#include <wrl/client.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

extern HINSTANCE g_hInst;   // defined in NppMarkdownPreview.cpp
void OnWebViewReady();      // implemented in NppMarkdownPreview.cpp

namespace wv2 {

inline ComPtr<ICoreWebView2Environment> g_env;
inline ComPtr<ICoreWebView2Controller> g_controller;
inline ComPtr<ICoreWebView2> g_webview;
inline bool g_ready = false;

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
                                     ICoreWebView2NavigationCompletedEventArgs* /*args*/) override {
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

class ControllerHandler final : public RefCounted<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HWND parent = nullptr;
    std::wstring html;

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* createdController) override {
        if (FAILED(errorCode) || !createdController) return S_OK;
        g_controller = createdController;
        if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || !g_webview) return S_OK;

        RECT rc = {};
        if (parent && GetClientRect(parent, &rc)) {
            g_controller->put_Bounds(rc);
        }

        EventRegistrationToken navToken = {};
        ComPtr<NavHandler> nav(new NavHandler());
        g_webview->add_NavigationCompleted(nav.Get(), &navToken);
        g_webview->NavigateToString(html.c_str());
        return S_OK;
    }
};

class EnvironmentHandler final : public RefCounted<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HWND parent = nullptr;
    std::wstring html;

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* createdEnvironment) override {
        if (FAILED(errorCode) || !createdEnvironment) {
            MessageBoxW(parent,
                        L"WebView2 runtime is not available on this system.\n\n"
                        L"Please install the Microsoft Edge WebView2 Runtime (Evergreen)\n"
                        L"and restart Notepad++.",
                        L"NppMarkdownPreview", MB_OK | MB_ICONWARNING);
            return S_OK;
        }
        g_env = createdEnvironment;
        auto* ch = new ControllerHandler();
        ch->parent = parent;
        ch->html = html;
        createdEnvironment->CreateCoreWebView2Controller(parent, ch);
        ch->Release();
        return S_OK;
    }
};

typedef HRESULT(WINAPI* PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

inline HMODULE g_loader = nullptr;

inline bool Init(HWND parent, const std::wstring& bootstrapHtml) {
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
    std::wstring userData = std::wstring(localAppData) + L"\\NppMarkdownPreview\\WebView2Cache";
    CreateDirectoryW(userData.c_str(), nullptr);   // may already exist

    auto* eh = new EnvironmentHandler();
    eh->parent = parent;
    eh->html = bootstrapHtml;
    HRESULT hr = create(nullptr, userData.c_str(), nullptr, eh);
    eh->Release();
    return SUCCEEDED(hr);
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
