// Inventory Manager - Windows desktop app.
//
// Starts the inventory server on this computer and shows the app in its own window
// (Microsoft Edge WebView2). If WebView2 isn't available it opens your web browser instead.
//
// Command-line options:
//   --db <file>      use a different database file
//   --port <n>       listen on a fixed port (default: any free port)
//   --host <addr>    listen address; 0.0.0.0 lets other computers on your network connect
//   --browser        open in your web browser instead of the app window
//   --headless       run the server only (no window); used for automated testing

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>

#include "resource.h"
#include "server.h"
#include "store.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kWindowClass = L"InventoryManagerWindow";
const wchar_t* kTitle = L"Inventory Manager";
const wchar_t* kMutexName = L"InventoryManagerSingleInstance";
const UINT WM_APP_RUN = WM_APP + 1;  // runs a queued function on the window thread
const wchar_t* kRegKey = L"Software\\Inventory Manager";

struct Options {
    std::wstring db, host = L"127.0.0.1";
    int port = 0;
    bool browser = false, headless = false;
};

struct App {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring url;
    bool browser_mode = false;  // showing "running in your browser" instead of the web view
} g_app;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path data_dir() {
    PWSTR local = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
    CoTaskMemFree(local);
    if (dir.empty()) dir = exe_dir();
    return dir / L"Inventory Manager";
}

void error_box(const std::wstring& message) {
    MessageBoxW(g_app.hwnd, message.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

bool load_asset(const std::string& name, std::string& body) {
    int id = name == "index.html" ? IDR_INDEX_HTML : name == "app.js" ? IDR_APP_JS : name == "styles.css" ? IDR_STYLES_CSS : 0;
    if (!id) return false;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    body.assign(static_cast<const char*>(data), SizeofResource(nullptr, res));
    return true;
}

Options parse_args() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--db") o.db = next();
        else if (a == L"--port") o.port = _wtoi(next().c_str());
        else if (a == L"--host") o.host = next();
        else if (a == L"--browser") o.browser = true;
        else if (a == L"--headless") o.headless = true;
    }
    LocalFree(argv);
    return o;
}

// Runs fn on the window's thread after the current event finishes (dialogs can't open inside some callbacks).
void run_async(std::function<void()> fn) {
    PostMessageW(g_app.hwnd, WM_APP_RUN, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn))));
}

// Standard Windows "Save As" dialog. Returns false if the user cancels.
bool ask_save_path(const std::wstring& suggested, std::wstring& chosen) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    std::filesystem::path p(suggested);
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    COMDLG_FILTERSPEC types[2] = {{L"All files", L"*.*"}, {L"All files", L"*.*"}};
    UINT count = 1;
    if (ext == L".xlsx") { types[0] = {L"Excel workbook (*.xlsx)", L"*.xlsx"}; count = 2; }
    else if (ext == L".csv") { types[0] = {L"CSV file (*.csv)", L"*.csv"}; count = 2; }
    else if (ext == L".db") { types[0] = {L"Inventory Manager backup (*.db)", L"*.db"}; count = 2; }
    dlg->SetFileTypes(count, types);
    dlg->SetFileName(p.filename().c_str());
    if (ext.size() > 1) dlg->SetDefaultExtension(ext.c_str() + 1);
    ComPtr<IShellItem> folder;
    if (p.has_parent_path() &&
        SUCCEEDED(SHCreateItemFromParsingName(p.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetDefaultFolder(folder.Get());
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(g_app.hwnd))) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    chosen = path;
    CoTaskMemFree(path);
    return true;
}

void open_in_browser() { ShellExecuteW(nullptr, L"open", g_app.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ------------------------------------------------------------------ window position memory

void save_placement(HWND hwnd) {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(hwnd, &wp)) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&wp), sizeof wp);
        RegCloseKey(key);
    }
}

bool load_placement(WINDOWPLACEMENT& wp) {
    DWORD size = sizeof wp;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"WindowPlacement", RRF_RT_REG_BINARY, nullptr, &wp, &size) != ERROR_SUCCESS ||
        size != sizeof wp || wp.length != sizeof wp)
        return false;
    // Only reuse it if the window would still be visible on a connected screen.
    return MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL) != nullptr;
}

// ------------------------------------------------------------------ web view

void resize_webview() {
    if (!g_app.controller) return;
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    g_app.controller->put_Bounds(rc);
}

void fall_back_to_browser(bool explain) {
    g_app.browser_mode = true;
    g_app.controller.Reset();
    g_app.webview.Reset();
    InvalidateRect(g_app.hwnd, nullptr, TRUE);
    if (explain) {
        MessageBoxW(g_app.hwnd,
                    L"Inventory Manager couldn't start its window because the Microsoft Edge WebView2 Runtime "
                    L"isn't available on this computer.\n\nIt will open in your web browser instead. To use the app "
                    L"window, install the WebView2 Runtime from microsoft.com and restart Inventory Manager.",
                    kTitle, MB_OK | MB_ICONINFORMATION);
    }
    open_in_browser();
}

void create_webview() {
    std::wstring user_data = (data_dir() / L"WebView2").wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    fall_back_to_browser(true);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    g_app.hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                fall_back_to_browser(true);
                                return S_OK;
                            }
                            g_app.controller = controller;
                            controller->get_CoreWebView2(&g_app.webview);

                            ComPtr<ICoreWebView2Settings> settings;
                            g_app.webview->get_Settings(&settings);
                            settings->put_IsStatusBarEnabled(FALSE);
                            // Let the page's menu shortcuts (Ctrl+N, Ctrl+F, ...) work instead of browser ones.
                            ComPtr<ICoreWebView2Settings3> settings3;
                            if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#ifdef NDEBUG
                            settings->put_AreDevToolsEnabled(FALSE);
#endif
                            // Links that try to open a new window go to the normal web browser.
                            g_app.webview->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        if (uri) ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                                        CoTaskMemFree(uri);
                                        args->put_Handled(TRUE);
                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);

                            // File > Exit in the page's menu asks the window to close.
                            g_app.webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg && std::wstring(msg) == L"exit")
                                            PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
                                        CoTaskMemFree(msg);
                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);

                            // Downloads (exports, backups) ask where to save, like any Windows program.
                            ComPtr<ICoreWebView2_4> webview4;
                            if (SUCCEEDED(g_app.webview.As(&webview4))) {
                                webview4->add_DownloadStarting(
                                    Callback<ICoreWebView2DownloadStartingEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                                            ComPtr<ICoreWebView2Deferral> deferral;
                                            args->GetDeferral(&deferral);
                                            ComPtr<ICoreWebView2DownloadStartingEventArgs> a(args);
                                            run_async([a, deferral] {
                                                LPWSTR path = nullptr;
                                                a->get_ResultFilePath(&path);
                                                std::wstring chosen;
                                                if (ask_save_path(path ? path : L"", chosen)) a->put_ResultFilePath(chosen.c_str());
                                                else a->put_Cancel(TRUE);
                                                CoTaskMemFree(path);
                                                deferral->Complete();
                                            });
                                            return S_OK;
                                        })
                                        .Get(),
                                    nullptr);
                            }

                            resize_webview();
                            g_app.webview->Navigate(g_app.url.c_str());
                            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) fall_back_to_browser(true);
}

// ------------------------------------------------------------------ main window

void paint_browser_message(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    if (g_app.browser_mode) {
        NONCLIENTMETRICSW ncm{sizeof ncm};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        ncm.lfMessageFont.lfHeight = -MulDiv(11, GetDpiForWindow(hwnd), 72);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        std::wstring text = L"Inventory Manager is running in your web browser at\n" + g_app.url +
                            L"\n\nClick here to open it again.\nClose this window to stop Inventory Manager.";
        InflateRect(&rc, -24, -24);
        RECT measure = rc;
        DrawTextW(dc, text.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        OffsetRect(&rc, 0, ((rc.bottom - rc.top) - (measure.bottom - measure.top)) / 2);
        DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, old);
        DeleteObject(font);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            resize_webview();
            if (g_app.browser_mode) InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_DPICHANGED: {
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = GetDpiForWindow(hwnd);
            info->ptMinTrackSize = {MulDiv(420, dpi, 96), MulDiv(400, dpi, 96)};
            return 0;
        }
        case WM_SETFOCUS:
            if (g_app.controller) g_app.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_PAINT:
            paint_browser_message(hwnd);
            return 0;
        case WM_LBUTTONUP:
            if (g_app.browser_mode) open_in_browser();
            return 0;
        case WM_APP_RUN: {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
            (*fn)();
            return 0;
        }
        case WM_CLOSE:
            save_placement(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_app.controller) g_app.controller->Close();
            g_app.controller.Reset();
            g_app.webview.Reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND create_main_window(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // Default: 1280x820 (scaled for the screen), centred on the main monitor's work area.
    UINT dpi = GetDpiForSystem();
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min<int>(MulDiv(1280, dpi, 96), work.right - work.left);
    int h = std::min<int>(MulDiv(820, dpi, 96), work.bottom - work.top);
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;
    HWND hwnd = CreateWindowExW(0, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;

    WINDOWPLACEMENT wp{sizeof wp};
    if (load_placement(wp)) {
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    UpdateWindow(hwnd);
    return hwnd;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    Options opt = parse_args();

    // One copy at a time: a second launch just brings the existing window forward.
    HANDLE mutex = nullptr;
    if (!opt.headless) {
        mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::filesystem::path db_path = opt.db.empty() ? data_dir() / L"inventory.db" : std::filesystem::path(opt.db);
    std::unique_ptr<Store> store;
    try {
        std::error_code ec;
        std::filesystem::create_directories(db_path.parent_path(), ec);
        store = std::make_unique<Store>(narrow(db_path.wstring()));
    } catch (const std::exception& e) {
        error_box(L"Couldn't open the data file\n" + db_path.wstring() + L"\n\n" + widen(e.what()));
        return 1;
    }
    try {
        // First run: bring in the spreadsheet that's installed next to the app.
        store->seed_from_file(narrow((exe_dir() / L"inventory_management.xlsx").wstring()));
    } catch (const std::exception&) {
        // Not fatal - the inventory just starts empty and can be imported later.
    }
    if (!opt.headless) {
        try {
            store->auto_backup_if_due();  // daily copy in the Backups folder (Settings can turn this off)
        } catch (const std::exception&) {
        }
    }

    AppServer server(*store, load_asset);
    int port = server.start(narrow(opt.host), opt.port);
    if (port <= 0) {
        if (!opt.headless)
            error_box(L"Inventory Manager couldn't start its local server" +
                      (opt.port ? L" on port " + std::to_wstring(opt.port) : std::wstring()) +
                      L".\nAnother program may be using it.");
        return 1;
    }
    g_app.url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";

    if (opt.headless) {
        // Serve until the process is ended.
        Sleep(INFINITE);
        return 0;
    }

    g_app.hwnd = create_main_window(inst);
    if (!g_app.hwnd) {
        error_box(L"Couldn't create the app window.");
        return 1;
    }
    if (opt.browser) fall_back_to_browser(false);
    else create_webview();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    server.stop();
    store.reset();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
