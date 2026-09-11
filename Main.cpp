#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include "DwmZOrderClient.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace standalone
{
    struct Operation
    {
        ks::dwm_order::Request request;
        ks::dwm_order::Reply reply;
        std::wstring time;
    };

    std::filesystem::path ExecutableDirectory()
    {
        std::wstring path(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) throw std::runtime_error("Executable path unavailable");
        path.resize(size);
        return std::filesystem::path(path).parent_path();
    }

    bool IsAdministrator()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
        CloseHandle(token);
        return ok && elevation.TokenIsElevated;
    }

    std::wstring Timestamp()
    {
        SYSTEMTIME t{};
        GetSystemTime(&t);
        wchar_t buf[48]{};
        swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        return buf;
    }

    std::wstring StatusText(ks::dwm_order::Status status)
    {
        using S = ks::dwm_order::Status;
        switch (status)
        {
        case S::Ok: return L"回执成功 / OK";
        case S::InvalidRequest: return L"请求无效 / Invalid request";
        case S::InvalidWindow: return L"窗口失效或身份变化 / Invalid window";
        case S::DifferentDesktop: return L"窗口不在同一桌面 / Different desktop";
        case S::UnsupportedRuntime: return L"当前 DWM 模型不支持 / Unsupported DWM runtime";
        case S::HookConflict: return L"排序回调冲突 / Hook conflict";
        case S::WindowNotComposed: return L"窗口未参与合成 / Window not composed";
        case S::NativeFailure: return L"内部调用失败，可能部分生效 / Native failure, possibly partial";
        case S::VerificationFailed: return L"回读未通过 / Verification failed";
        case S::NotRunning: return L"代理未运行 / Agent not running";
        case S::AgentMismatch: return L"代理版本不同，请注销后重试 / Agent mismatch; sign out and retry";
        case S::TransportFailure: return L"连接或加载失败 / Transport failure";
        case S::Timeout: return L"超时，结果未知 / Timeout, outcome unknown";
        case S::InternalException: return L"代理异常，结果未知 / Agent exception, outcome unknown";
        }
        return L"未知状态 / Unknown status";
    }
}

namespace
{
    using namespace ks::dwm_order;
    constexpr UINT Completed = WM_APP + 1;
    enum Id { AdminId = 100, RefreshId, TargetId, ReferenceId, PositionId, MaintainId,
        QueryId, ApplyId, RestoreId, StopId, LogId };
    struct Job
    {
        HWND owner = nullptr;
        standalone::Operation operation;
        std::wstring error;
        bool closeAfterRestore = false;
    };
    struct WindowItem { std::wstring text; WindowIdentity identity; };
    struct App
    {
        HWND window = nullptr, log = nullptr;
        HWND title = nullptr, scope = nullptr, targetLabel = nullptr, referenceLabel = nullptr, positionLabel = nullptr;
        HWND target = nullptr, reference = nullptr, position = nullptr, maintain = nullptr;
        HFONT font = nullptr;
        std::vector<HWND> inputs;
        std::vector<WindowItem> windows;
        std::shared_ptr<Job> job;
        std::thread worker;
        WindowIdentity held{};
        bool busy = false, closePending = false;
        UINT dpi = 96;
    } g;

    int Px(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }
    std::wstring Text(HWND window)
    {
        const int n = GetWindowTextLengthW(window);
        std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
        s.resize(GetWindowTextW(window, s.data(), n + 1));
        return s;
    }
    void Log(const std::wstring& text) { SetWindowTextW(g.log, text.c_str()); }
    HWND Item(const wchar_t* cls, const wchar_t* text, int id, DWORD style = 0)
    {
        const DWORD extra = wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
        HWND h = CreateWindowExW(extra, cls, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!h) throw std::runtime_error("Cannot create UI control");
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
        if (id >= AdminId && id != LogId) g.inputs.push_back(h);
        return h;
    }
    void Move(HWND h, int x, int y, int w, int height) { MoveWindow(h, Px(x), Px(y), Px(w), Px(height), TRUE); }
    void MoveId(int id, int x, int y, int w, int height) { Move(GetDlgItem(g.window, id), x, y, w, height); }

    void Layout()
    {
        RECT r{};
        GetClientRect(g.window, &r);
        const int width = MulDiv(r.right, 96, static_cast<int>(g.dpi));
        const int height = MulDiv(r.bottom, 96, static_cast<int>(g.dpi));
        const int content = width - 36;
        Move(g.title, 18, 12, content, 24);
        Move(g.scope, 18, 39, content, 44);
        MoveId(AdminId, 18, 86, 188, 30);
        MoveId(RefreshId, 214, 86, content - 196, 30);
        Move(g.targetLabel, 18, 134, 117, 22);
        Move(g.target, 135, 129, content - 117, 260);
        Move(g.referenceLabel, 18, 172, 117, 22);
        Move(g.reference, 135, 167, content - 117, 260);
        Move(g.positionLabel, 18, 211, 117, 22);
        Move(g.position, 135, 205, 356, 180);
        Move(g.maintain, 506, 205, content - 488, 29);
        const int actionWidth = (content - 24) / 4;
        for (int i = 0; i < 4; ++i) MoveId(QueryId + i, 18 + i * (actionWidth + 8), 246, actionWidth, 32);
        Move(g.log, 18, 291, content, height - 326);
    }

    void UpdateEnabled()
    {
        for (HWND input : g.inputs) EnableWindow(input, !g.busy);
        if (!g.busy)
        {
            const bool elevated = standalone::IsAdministrator();
            for (int id : {QueryId, ApplyId, RestoreId, StopId}) EnableWindow(GetDlgItem(g.window, id), elevated);
            EnableWindow(GetDlgItem(g.window, AdminId), !elevated);
            EnableWindow(g.reference, SendMessageW(g.position, CB_GETCURSEL, 0, 0) >= 2);
        }
    }

    BOOL CALLBACK Enumerate(HWND window, LPARAM)
    {
        if (window == g.window || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return TRUE;
        wchar_t title[256]{};
        if (!GetWindowTextW(window, title, 256)) return TRUE;
        WindowIdentity identity;
        std::uint32_t error = 0;
        if (!CaptureWindow(reinterpret_cast<std::uint64_t>(window), identity, error)) return TRUE;
        std::wostringstream s;
        s << L"0x" << std::hex << identity.hwnd << std::dec << L"  [PID " << identity.processId << L"]  " << title;
        g.windows.push_back({s.str(), identity});
        return g.windows.size() < 4096;
    }

    void Refresh()
    {
        const auto oldTarget = Text(g.target), oldReference = Text(g.reference);
        g.windows.clear();
        EnumWindows(Enumerate, 0);
        for (HWND box : {g.target, g.reference})
        {
            SendMessageW(box, CB_RESETCONTENT, 0, 0);
            for (const auto& item : g.windows) SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.text.c_str()));
        }
        auto preserve = [](HWND box, const std::wstring& old)
        {
            const auto at = SendMessageW(box, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(old.c_str()));
            if (at != CB_ERR) SendMessageW(box, CB_SETCURSEL, at, 0);
            else SetWindowTextW(box, old.c_str());
        };
        preserve(g.target, oldTarget);
        preserve(g.reference, oldReference);
    }

    bool Identity(HWND box, WindowIdentity& result)
    {
        const auto at = SendMessageW(box, CB_GETCURSEL, 0, 0);
        const auto input = Text(box);
        if (at >= 0 && static_cast<std::size_t>(at) < g.windows.size() && input == g.windows[at].text)
        { result = g.windows[at].identity; return true; }
        const auto start = input.find_first_not_of(L" \t");
        if (start == std::wstring::npos || input[start] == L'-' || input[start] == L'+') return false;
        wchar_t* end = nullptr;
        errno = 0;
        const int base = input.compare(start, 2, L"0x") == 0 || input.compare(start, 2, L"0X") == 0 ? 16 : 10;
        const auto handle = wcstoull(input.c_str() + start, &end, base);
        while (end && (*end == L' ' || *end == L'\t')) ++end;
        std::uint32_t error = 0;
        return !errno && end && !*end && handle && CaptureWindow(handle, result, error);
    }

    void Begin(const std::shared_ptr<Job>& job)
    {
        if (g.busy) return;
        g.busy = true;
        g.job = job;
        job->owner = g.window;
        UpdateEnabled();
        Log(L"正在处理，请稍候… / Working…\r\n操作期间仍可移动窗口。 / The window remains responsive.");
        try
        {
            g.worker = std::thread([job]
            {
                try
                {
                    job->operation.time = standalone::Timestamp();
                    job->operation.reply = ExecuteRequest(job->operation.request, (standalone::ExecutableDirectory() / L"KswordDwmZOrder.dll").wstring());
                }
                catch (const std::exception& e)
                {
                    job->error = L"操作失败 / Operation failed: ";
                    for (const char* p = e.what(); *p; ++p) job->error += static_cast<wchar_t>(static_cast<unsigned char>(*p));
                    job->operation.reply.response.status = Status::InternalException;
                }
                catch (...) { job->error = L"未知异常 / Unexpected exception"; job->operation.reply.response.status = Status::InternalException; }
                PostMessageW(job->owner, Completed, 0, 0);
            });
        }
        catch (...) { g.busy = false; g.job.reset(); UpdateEnabled(); Log(L"无法启动工作线程 / Cannot start worker"); }
    }

    void Start(Action action)
    {
        auto job = std::make_shared<Job>();
        auto& request = job->operation.request;
        request.action = action;
        request.position = static_cast<Position>(SendMessageW(g.position, CB_GETCURSEL, 0, 0));
        request.maintain = SendMessageW(g.maintain, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (action != Action::Stop && !Identity(g.target, request.target))
        { Log(L"请选择有效窗口，或输入十进制 / 0x 十六进制 HWND。\r\nSelect a window or enter its decimal / 0x hexadecimal HWND."); return; }
        if (action == Action::Apply && request.position >= Position::Before
            && (!Identity(g.reference, request.reference) || request.reference.hwnd == request.target.hwnd))
        { Log(L"请选择同一桌面上的另一个参照窗口。\r\nSelect another reference window on the same desktop."); return; }
        if (action == Action::Stop && MessageBoxW(g.window,
            L"将停止当前会话排序代理的全部保持（包括 KSword 设置的保持），并恢复系统顺序。\nStop all ordering maintenance in this session, including KSword, and restore system order?",
            L"停止全部 / Stop all", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        Begin(job);
    }

    void Close()
    {
        if (g.busy) { g.closePending = true; Log(L"等待当前操作完成后关闭… / Closing after the current operation finishes…"); return; }
        g.closePending = false;
        if (g.held.hwnd)
        {
            const int choice = MessageBoxW(g.window, L"此工具可能仍在持续保持窗口。关闭前恢复吗？\n是：恢复后关闭。否：保留保持并退出。\n\nThis tool may still be maintaining a window. Restore before closing?\nYes: restore and close. No: leave maintenance running.",
                L"退出 / Exit", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) return;
            if (choice == IDYES)
            {
                auto job = std::make_shared<Job>();
                job->operation.request.action = Action::Restore; job->operation.request.target = g.held;
                job->closeAfterRestore = true; Begin(job); return;
            }
        }
        DestroyWindow(g.window);
    }

    void Finish()
    {
        if (g.worker.joinable()) g.worker.join();
        auto job = std::move(g.job);
        g.busy = false;
        if (!job) return;
        {
            const auto& r = job->operation.reply.response;
            const auto& q = job->operation.request;
            // Keep a conservative owner identity for timeout/partial outcomes.
            if (q.action == Action::Apply && q.maintain && (r.maintainedWindow == q.target.hwnd
                || r.status == Status::Timeout || r.status == Status::InternalException)) g.held = q.target;
            if (r.dwmProcessId && !(r.flags & Maintaining) && r.status == Status::Ok) g.held = {};
            std::wstring text = standalone::StatusText(r.status) + L"\r\n";
            if (r.flags & Maintaining) text += L"持续保持中 / Maintaining\r\n";
            if (r.windowCount) text += L"合成位置 / Position: " + std::to_wstring(r.index + 1) + L" / " + std::to_wstring(r.windowCount) + L" (1 = 最前 / front)\r\n";
            Log(text);
        }
        if (!job->error.empty()) Log(job->error);
        UpdateEnabled();
        if (job->closeAfterRestore)
        {
            if (job->error.empty() && job->operation.reply.response.status == Status::Ok) { DestroyWindow(g.window); return; }
            g.closePending = false;
            Log(Text(g.log) + L"\r\n\r\n恢复未确认成功，工具保持打开。\r\nRestore was not confirmed; the tool remains open.");
        }
        else if (g.closePending) Close();
    }

    void CreateUi()
    {
        g.dpi = GetDpiForWindow(g.window);
        g.font = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g.title = Item(L"STATIC", L"DWM Order Tool 1.0  ·  独立窗口排序工具", 0);
        g.scope = Item(L"STATIC", L"Win11 24H2 x64；Win10 19041 系列 x64（实验性 / experimental）。\r\n跨 Band 合成排序；鼠标命中、焦点不变。 / Composition order only; input and focus are unchanged.", 0);
        Item(L"BUTTON", L"管理员重启 / Elevate", AdminId, WS_TABSTOP);
        Item(L"BUTTON", L"刷新 / Refresh", RefreshId, WS_TABSTOP);
        g.targetLabel = Item(L"STATIC", L"目标 / Target", 0);
        g.referenceLabel = Item(L"STATIC", L"参照 / Reference", 0);
        g.positionLabel = Item(L"STATIC", L"位置 / Position", 0);
        g.target = Item(L"COMBOBOX", L"", TargetId, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP);
        g.reference = Item(L"COMBOBOX", L"", ReferenceId, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP);
        SendMessageW(g.target, CB_LIMITTEXT, 512, 0); SendMessageW(g.reference, CB_LIMITTEXT, 512, 0);
        g.position = Item(L"COMBOBOX", L"", PositionId, CBS_DROPDOWNLIST | WS_TABSTOP);
        for (const auto* text : {L"合成最前 / Front", L"合成最后 / Back", L"参照上方 / Above reference", L"参照下方 / Below reference"})
            SendMessageW(g.position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(g.position, CB_SETCURSEL, 0, 0);
        g.maintain = Item(L"BUTTON", L"持续保持 / Maintain", MaintainId, BS_AUTOCHECKBOX | WS_TABSTOP);
        SendMessageW(g.maintain, BM_SETCHECK, BST_UNCHECKED, 0);
        Item(L"BUTTON", L"连接读取 / Query", QueryId, WS_TABSTOP);
        Item(L"BUTTON", L"应用顺序 / Apply", ApplyId, WS_TABSTOP);
        Item(L"BUTTON", L"恢复目标 / Restore", RestoreId, WS_TABSTOP);
        Item(L"BUTTON", L"停止全部 / Stop all", StopId, WS_TABSTOP);
        g.log = Item(L"EDIT", L"", LogId, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
        SendMessageW(g.log, EM_SETLIMITTEXT, 200000, 0);
        Layout(); Refresh(); UpdateEnabled();
    }

    LRESULT CALLBACK MainWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        try
        {
            switch (message)
            {
            case WM_CREATE: g.window = window; CreateUi(); return 0;
            case WM_SIZE: if (g.log) Layout(); return 0;
            case WM_GETMINMAXINFO:
            {
                auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                info->ptMinTrackSize = {(std::min)(Px(790), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)),
                    (std::min)(Px(650), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top))}; return 0;
            }
            case WM_DPICHANGED:
            {
                g.dpi = HIWORD(wParam);
                const auto* rect = reinterpret_cast<RECT*>(lParam);
                LOGFONTW lf{}; GetObjectW(g.font, sizeof(lf), &lf); lf.lfHeight = -Px(15);
                HFONT replacement = CreateFontIndirectW(&lf);
                if (replacement)
                {
                    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
                    DeleteObject(g.font); g.font = replacement;
                }
                SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
                Layout(); return 0;
            }
            case Completed: Finish(); return 0;
            case WM_COMMAND:
                if (g.busy) return 0;
                switch (LOWORD(wParam))
                {
                case AdminId:
                {
                    const auto exe = standalone::ExecutableDirectory() / L"DwmOrderTool.exe";
                    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"runas", exe.c_str(), nullptr, standalone::ExecutableDirectory().c_str(), SW_SHOWNORMAL));
                    if (result > 32) DestroyWindow(window);
                    else Log(L"管理员重启未完成 / Elevation was cancelled or failed.");
                    break;
                }
                case RefreshId: Refresh(); break;
                case PositionId: UpdateEnabled(); break;
                case QueryId: Start(Action::Query); break;
                case ApplyId: Start(Action::Apply); break;
                case RestoreId: Start(Action::Restore); break;
                case StopId: Start(Action::Stop); break;
                }
                return 0;
            case WM_CLOSE: Close(); return 0;
            case WM_DESTROY:
                if (g.font) DeleteObject(g.font);
                PostQuitMessage(0); return 0;
            }
        }
        catch (...) { if (message == WM_CREATE) return -1; Log(L"界面操作失败 / UI operation failed"); }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        int argc = 0;
        auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return 1;
        if (argc != 1)
        {
            const std::wstring command = argc > 1 ? argv[1] : L"";
            LocalFree(argv);
            return 64;
        }
        LocalFree(argv);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        cls.lpfnWndProc = MainWindow; cls.lpszClassName = L"DwmOrderStandalone";
        if (!RegisterClassExW(&cls)) return 1;
        g.dpi = GetDpiForSystem();
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"DwmOrderStandalone", L"DWM Order Tool / 独立窗口排序工具",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            (std::min)(Px(860), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)),
            (std::min)(Px(780), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top)), nullptr, nullptr, instance, nullptr);
        if (!window) return 1;
        ShowWindow(window, show); UpdateWindow(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (g.worker.joinable()) g.worker.join();
        return static_cast<int>(message.wParam);
    }
    catch (...) { return 1; }
}
