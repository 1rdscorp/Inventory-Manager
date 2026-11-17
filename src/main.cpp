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
//   --shared <dir>   use this shared folder (instead of the one chosen in Settings)
//   --cache <dir>    keep the shared-folder working copy here (testing several "computers" on one PC)
//   --data-dir <dir> keep this computer's data and backups here instead of %LOCALAPPDATA%\Inventory Manager

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "server.h"
#include "shared.h"
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
    std::wstring db, host = L"127.0.0.1", shared, cache, data_dir;
    int port = 0;
    DWORD wait_for_pid = 0;  // after "restart": wait for the previous copy to finish closing
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
        else if (a == L"--shared") o.shared = next();
        else if (a == L"--cache") o.cache = next();
        else if (a == L"--data-dir") o.data_dir = next();
        else if (a == L"--wait-for-pid") o.wait_for_pid = static_cast<DWORD>(_wtoi(next().c_str()));
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

// A Windows task dialog with large option buttons. Returns the chosen button id (IDCANCEL if closed).
int ask(const std::wstring& heading, const std::wstring& text, const std::vector<std::pair<int, std::wstring>>& buttons,
        int default_id, PCWSTR icon = TD_WARNING_ICON) {
    std::vector<TASKDIALOG_BUTTON> b;
    for (const auto& [id, label] : buttons) b.push_back({id, label.c_str()});
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = g_app.hwnd;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = kTitle;
    c.pszMainIcon = icon;
    c.pszMainInstruction = heading.c_str();
    c.pszContent = text.c_str();
    c.cButtons = static_cast<UINT>(b.size());
    c.pButtons = b.data();
    c.nDefaultButton = default_id;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return IDCANCEL;
    return pressed;
}

// Standard Windows folder picker. Returns false if cancelled.
bool pick_folder(std::wstring& chosen) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose the shared folder for your inventory");
    dlg->SetOkButtonLabel(L"Use this folder");
    if (FAILED(dlg->Show(g_app.hwnd))) return false;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    chosen = path;
    CoTaskMemFree(path);
    return true;
}

// Starts a fresh copy of the app (which waits for this one to close), then closes this one.
void restart_app() {
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    // Same options as this copy was started with (a later --wait-for-pid overrides any earlier one).
    std::wstring cmd = std::wstring(GetCommandLineW()) + L" --wait-for-pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
}

void post_to_page(const json& message) {
    if (g_app.webview) g_app.webview->PostWebMessageAsJson(widen(message.dump()).c_str());
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
                                        LPWSTR raw = nullptr;
                                        std::wstring msg;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) msg = raw;
                                        CoTaskMemFree(raw);
                                        if (msg == L"exit") PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
                                        else if (msg == L"restart") run_async(restart_app);
                                        else if (msg == L"pick-folder")
                                            run_async([] {
                                                std::wstring folder;
                                                post_to_page({{"type", "folder"}, {"path", pick_folder(folder) ? narrow(folder) : ""}});
                                            });
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
    if (opt.wait_for_pid) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, opt.wait_for_pid)) {
            WaitForSingleObject(old, 30000);
            CloseHandle(old);
        }
    }

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

    const std::filesystem::path local_dir = opt.data_dir.empty() ? data_dir() : std::filesystem::path(opt.data_dir);
    const std::filesystem::path backups_dir = local_dir / L"Backups";
    const std::filesystem::path cache_dir = opt.cache.empty() ? local_dir / L"shared" : std::filesystem::path(opt.cache);
    const std::string computer = computer_name();

    // Where is the inventory? This computer's own folder, or a shared folder (Settings > Data location).
    std::unique_ptr<SharedSync> sync;
    std::string startup_note;
    std::filesystem::path db_path;
    DataLocation loc = DataLocation::load();
    if (!opt.shared.empty()) loc = {true, std::filesystem::path(opt.shared)};
    if (!opt.db.empty()) {
        db_path = opt.db;
    } else if (loc.shared) {
        sync = std::make_unique<SharedSync>(loc.folder, cache_dir, computer);
        for (;;) {
            try {
                bool view_only = false;
                LockInfo l = sync->read_lock();
                if (l.exists && !l.this_computer) {
                    std::wstring who = widen(l.computer) + (l.user.empty() ? L"" : L" (" + widen(l.user) + L")");
                    int choice;
                    if (opt.headless) {
                        choice = l.stale ? 1 : 2;
                    } else if (l.stale) {
                        choice = ask(L"The shared inventory was last open on " + who,
                                     L"That computer hasn't been heard from for " + std::to_wstring(l.minutes_since_seen) +
                                         L" minutes - it may have been turned off without closing Inventory Manager, or lost "
                                         L"its internet connection.",
                                     {{1, L"Open it here\nUse this computer to make changes"},
                                      {2, L"View only\nLook without making changes"},
                                      {3, L"Exit"}},
                                     1, TD_INFORMATION_ICON);
                    } else {
                        choice = ask(L"The shared inventory is open on " + who,
                                     L"Only one computer can make changes at a time, so changes made on two computers "
                                     L"at once don't overwrite each other.",
                                     {{2, L"View only\nLook without making changes (recommended)"},
                                      {1, L"Open anyway\nOnly if Inventory Manager isn't really open there - any unsaved "
                                          L"changes on that computer could be lost"},
                                      {3, L"Exit"}},
                                     2);
                    }
                    if (choice != 1 && choice != 2) return 0;
                    view_only = choice == 2;
                    if (view_only) sync->set_view_only(narrow(who));
                }
                startup_note = sync->prepare(view_only, backups_dir);
                if (!view_only) sync->take_lock();
                db_path = sync->working_db();
                break;
            } catch (const ApiError& e) {
                if (opt.headless) return 1;
                int choice = ask(L"Can't open the shared inventory", widen(e.what()),
                                 {{1, L"Try again"},
                                  {2, L"Use this computer's own data for now\nThe shared folder stays selected in Settings"},
                                  {3, L"Exit"}},
                                 1);
                if (choice == 1) continue;
                if (choice != 2) return 0;
                sync.reset();
                break;
            }
        }
    }
    if (db_path.empty()) db_path = local_dir / L"inventory.db";
    std::unique_ptr<Store> store;
    try {
        std::error_code ec;
        std::filesystem::create_directories(db_path.parent_path(), ec);
        store = std::make_unique<Store>(narrow(db_path.wstring()));
    } catch (const std::exception& e) {
        error_box(L"Couldn't open the data file\n" + db_path.wstring() + L"\n\n" + widen(e.what()));
        return 1;
    }
    if (sync) store->set_backups_dir(narrow(backups_dir.wstring()));
    const bool view_only = sync && sync->read_only();
    try {
        // First run: bring in the spreadsheet that's installed next to the app.
        store->seed_from_file(narrow((exe_dir() / L"inventory_management.xlsx").wstring()));
    } catch (const std::exception&) {
        // Not fatal - the inventory just starts empty and can be imported later.
    }
    if (!opt.headless && !view_only) {
        try {
            store->auto_backup_if_due();  // daily copy in the Backups folder (Settings can turn this off)
        } catch (const std::exception&) {
        }
    }

    // Shared folder: save any changes not yet in the shared folder, then keep saving in the background.
    std::atomic<bool> stop_sync{false};
    std::atomic<bool> switching{false};
    std::thread sync_thread;
    if (sync && !view_only) {
        sync->attach(*store);
        sync->push(*store);
        sync_thread = std::thread([&] {
            while (!stop_sync) {
                for (int i = 0; i < 50 && !stop_sync; ++i) Sleep(100);
                if (stop_sync) break;
                try {
                    sync->tick(*store);
                } catch (const std::exception&) {
                }
            }
        });
    }

    AppServer server(*store, load_asset);
    LocationHooks hooks;
    hooks.read_only_reason = [&]() -> std::string {
        if (switching) return "Inventory Manager is restarting to switch its data location.";
        return sync ? sync->read_only_reason() : std::string();
    };
    hooks.status = [&]() -> json {
        json s = sync ? sync->status()
                      : json{{"mode", "local"}, {"working_copy", narrow(db_path.wstring())}, {"read_only", false},
                             {"read_only_reason", ""}, {"computer", computer}};
        if (loc.shared && !sync && opt.db.empty()) {
            s["fallback"] = true;  // shared folder chosen but unavailable at startup
            s["folder"] = narrow(loc.folder.wstring());
        }
        s["notes"] = startup_note;
        s["local_folder"] = narrow(local_dir.wstring());
        s["fixed"] = !opt.db.empty();
        s["switching"] = switching.load();
        return s;
    };
    hooks.inspect = [&](const std::string& path) -> json {
        std::filesystem::path p(widen(path));
        if (path.empty() || !p.is_absolute()) throw ApiError("Choose a folder (a full path, like G:\\My Drive\\Inventory)");
        json info = inspect_folder(p);
        std::error_code ec;
        info["is_current"] = sync && std::filesystem::equivalent(p, sync->folder(), ec);
        return info;
    };
    hooks.change = [&](const json& body) -> json {
        if (!opt.db.empty() || !opt.shared.empty()) throw ApiError("The data location is fixed by how Inventory Manager was started.");
        if (!body.is_object()) throw ApiError("Invalid request");
        std::string mode = body.value("mode", ""), use = body.value("use", "");
        std::error_code ec;
        if (mode == "shared") {
            std::filesystem::path folder(widen(body.value("path", "")));
            if (folder.empty() || !folder.is_absolute()) throw ApiError("Choose a folder");
            json info = inspect_folder(folder);
            if (!info.value("exists", false)) throw ApiError("That folder doesn't exist.");
            if (!info.value("writable", false)) throw ApiError("Inventory Manager can't save files in that folder.");
            auto rel = std::filesystem::relative(folder, local_dir, ec);
            if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0)
                throw ApiError("Choose a folder outside Inventory Manager's own data folder - for example in Google Drive.");
            const json& lk = info["lock"];
            if (lk.value("exists", false) && !lk.value("this_computer", false) && !lk.value("stale", false))
                throw ApiError("The inventory in that folder is open on " + lk.value("computer", std::string("another computer")) +
                               " right now. Close Inventory Manager there first.");
            if (sync && std::filesystem::equivalent(folder, sync->folder(), ec)) throw ApiError("This computer already uses that folder.");
            if (use == "this") {
                // This computer's inventory becomes the shared one (anything already there is backed up first).
                if (info.value("has_inventory", false) || info.value("unreadable", false)) {
                    std::filesystem::create_directories(backups_dir, ec);
                    std::filesystem::copy_file(folder / L"inventory.db",
                                               backups_dir / widen("shared-folder-before-replacing-" + now_iso().substr(0, 10) + ".db"),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (sync) sync->shutdown(*store);
                long long gen = publish_inventory(*store, folder, local_dir, computer);
                // The working copy for the new folder is this same data, already in sync.
                if (!sync) {
                    std::filesystem::create_directories(cache_dir, ec);
                    store->backup_to(narrow((cache_dir / L"inventory.db").wstring()));
                }
                write_sync_state(cache_dir, folder, gen, false);
            } else if (use == "folder") {
                if (!info.value("has_inventory", false)) throw ApiError("There's no inventory in that folder yet.");
                if (sync) sync->shutdown(*store);
                write_sync_state(cache_dir, folder, -1, false);  // load the folder's inventory on restart
            } else {
                throw ApiError("Choose which inventory to use");
            }
            DataLocation{true, folder}.save();
        } else if (mode == "local") {
            if (!sync && !loc.shared) throw ApiError("This computer already keeps its own data.");
            if (use == "this" && sync) {
                // Bring the shared inventory's current contents to this computer (keeping the old local data as a backup).
                std::filesystem::path local_db = local_dir / L"inventory.db";
                if (std::filesystem::exists(local_db, ec)) {
                    std::filesystem::create_directories(backups_dir, ec);
                    std::filesystem::copy_file(local_db, backups_dir / widen("this-computer-before-switching-" + now_iso().substr(0, 10) + ".db"),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
                store->backup_to(narrow(local_db.wstring()));
                std::filesystem::path journal = local_db;
                journal += L"-journal";
                std::filesystem::remove(journal, ec);
            }
            if (sync) sync->shutdown(*store);
            DataLocation{}.save();
        } else {
            throw ApiError("Unknown data location");
        }
        switching = true;
        return {{"ok", true}, {"restart", true}};
    };
    hooks.sync_now = [&]() -> json {
        if (!sync) return {{"ok", true}};
        bool ok = sync->push(*store, false);
        json s = sync->status();
        s["ok"] = ok;
        return s;
    };
    hooks.changed = [&] {
        if (sync) sync->note_change();
    };
    server.set_location_hooks(hooks);
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
    stop_sync = true;
    if (sync_thread.joinable()) sync_thread.join();
    if (sync) sync->shutdown(*store);  // final save to the shared folder, and free it for other computers
    store.reset();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
