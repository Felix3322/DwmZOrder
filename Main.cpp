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

namespace app
{
    const wchar_t* Local(const wchar_t* zh, const wchar_t* en)
    {
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? zh : en;
    }

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
        case S::Ok: return Local(L"操作完成。", L"Operation completed.");
        case S::InvalidRequest: return Local(L"请求参数无效，请重新选择窗口和位置。", L"Invalid request. Select the window and position again.");
        case S::InvalidWindow: return Local(L"窗口已关闭或发生变化，请刷新窗口列表后重新选择。", L"The window has closed or changed. Refresh the list and select it again.");
        case S::DifferentDesktop: return Local(L"目标与参照不在同一桌面，请选择同一桌面的窗口。", L"Select a target and reference on the same desktop.");
        case S::UnsupportedRuntime: return Local(L"此系统版本的窗口排序功能暂不受支持。", L"Window ordering is not supported on this system version.");
        case S::HookConflict: return Local(L"检测到排序控制冲突，无法完成操作。", L"A conflicting ordering hook prevented the operation.");
        case S::WindowNotComposed: return Local(L"无法在 DWM 排序列表中找到此窗口，请显示窗口后刷新重试。", L"This window was not found in DWM's ordering list. Show the window, then refresh and retry.");
        case S::NativeFailure: return Local(L"排序调用失败，部分修改可能已生效。请先读取当前顺序。", L"An ordering call failed; some changes may have taken effect. Read the current order first.");
        case S::VerificationFailed: return Local(L"无法确认窗口顺序，不能判定操作成功。请重新读取顺序。", L"The window order could not be confirmed. Read the order again before assuming success.");
        case S::NotRunning: return Local(L"排序服务尚未运行。请先读取顺序或应用排序。", L"The ordering agent is not running. Read the order or apply a position first.");
        case S::AgentMismatch: return Local(L"系统中仍加载着另一版本的排序模块，请注销并重新登录后再试。", L"A different agent version is still loaded. Sign out and sign back in before retrying.");
        case S::TransportFailure: return Local(L"无法连接窗口排序服务，请确认管理员权限和程序同目录的 DwmZOrder.dll。", L"Could not connect to the ordering agent. Check administrator privileges and DwmZOrder.dll beside the program.");
        case S::Timeout: return Local(L"等待操作完成超时，目前无法确认结果。请先读取顺序，避免重复应用。", L"The operation timed out and its result is unknown. Read the order before applying again.");
        case S::InternalException: return Local(L"排序模块发生异常，目前无法确认结果。请先读取当前顺序。", L"The ordering agent encountered an exception; the result is unknown. Read the current order first.");
        }
        return Local(L"收到无法识别的操作结果。", L"An unrecognized result was returned.");
    }

    std::wstring ResultText(const Operation& op, const std::wstring& target, const std::wstring& reference)
    {
        using namespace ks::dwm_order;
        const auto& q = op.request;
        const auto& reply = op.reply;
        const auto& r = reply.response;
        std::wostringstream out;
        const bool confirmed = r.status == Status::Ok && (r.flags & Verified);
        if (confirmed)
        {
            switch (q.action)
            {
            case Action::Query: out << Local(L"已读取窗口顺序。", L"Window order read successfully."); break;
            case Action::Apply:
                switch (q.position)
                {
                case Position::Front: out << Local(L"已将目标窗口移到最前方。", L"The target window was moved to the front."); break;
                case Position::Back: out << Local(L"已将目标窗口移到最后方。", L"The target window was moved to the back."); break;
                case Position::Before: out << Local(L"已将目标窗口移到参照窗口上方。", L"The target window was moved above the reference."); break;
                case Position::After: out << Local(L"已将目标窗口移到参照窗口下方。", L"The target window was moved below the reference."); break;
                }
                break;
            case Action::Restore: out << Local(L"已将目标窗口恢复为 Windows 当前的排序。", L"The target window was restored to the current Windows order."); break;
            case Action::Stop: out << Local(L"已停止本会话的全部持续保持，并恢复系统顺序。", L"All maintained ordering in this session has stopped and system order has been restored."); break;
            }
        }
        else
            out << StatusText(r.status == Status::Ok ? Status::VerificationFailed : r.status);

        if (q.action != Action::Stop)
        {
            out << L"\r\n\r\n" << Local(L"目标窗口：", L"Target window: ") << target;
            if (q.action == Action::Apply && q.position >= Position::Before)
                out << L"\r\n" << Local(L"参照窗口：", L"Reference window: ") << reference;
        }
        if (confirmed && q.action != Action::Stop && r.windowCount && r.index < r.windowCount)
        {
            out << L"\r\n" << Local(L"DWM 中的顺序：第 ", L"Position in DWM: ") << r.index + 1;
            out << Local(L" 位", L"");
            if (!r.index) out << Local(L"（最前方）", L" (frontmost)");
            else if (r.index + 1 == r.windowCount) out << Local(L"（最后方）", L" (backmost)");
            out << L"\r\n" << Local(L"排序列表共 ", L"The ordering list contains ") << r.windowCount
                << Local(L" 项，不等于屏幕上可见窗口的数量。", L" entries; this is not the number of visible windows on screen.");
        }
        if (confirmed && q.action != Action::Stop)
        {
            out << L"\r\n";
            if ((r.flags & Maintaining) && r.maintainedWindow == q.target.hwnd)
            {
                if (r.maintenanceStatus == Status::Ok)
                    out << Local(L"持续保持：已开启，会在系统更新顺序时重新应用设定。", L"Maintain: on. The chosen order is reapplied when the system updates it.");
                else
                    out << Local(L"持续保持：最近一次更新失败。", L"Maintain: the last update failed.") << L" " << StatusText(r.maintenanceStatus);
            }
            else if (r.flags & Maintaining)
                out << Local(L"此目标未开启持续保持；另一个窗口正在被保持。", L"This target is not maintained; another window is being maintained.");
            else
                out << Local(L"持续保持：未开启，窗口顺序可能随其他操作改变。", L"Maintain: off. Other operations may change the window order.");
        }
        if (q.action == Action::Query)
            out << L"\r\n\r\n" << Local(L"本次只读取顺序，没有移动窗口。要更改位置，请点击“应用顺序”。", L"This request only reads the order; it does not move the window. Click Apply to change its position.");
        if (!confirmed)
        {
            out << L"\r\n\r\n" << Local(L"失败环节：", L"Failed step: ");
            switch (reply.stage)
            {
            case Stage::Window: out << Local(L"检查窗口", L"checking the window"); break;
            case Stage::AgentFile: out << Local(L"检查排序模块文件", L"checking the agent file"); break;
            case Stage::DwmProcess: out << Local(L"连接 DWM 进程", L"connecting to DWM"); break;
            case Stage::PrepareAgent: out << Local(L"准备排序模块", L"preparing the agent"); break;
            case Stage::LoadAgent: out << Local(L"加载排序模块", L"loading the agent"); break;
            case Stage::Request: out << Local(L"执行排序请求", L"executing the request"); break;
            case Stage::Receipt: out << Local(L"确认操作结果", L"confirming the result"); break;
            }
            if (reply.error) out << L"; Win32 " << reply.error;
            if (r.win32Error) out << L"; agent Win32 " << r.win32Error;
            if (r.nativeResult) out << L"; HRESULT 0x" << std::hex << static_cast<std::uint32_t>(r.nativeResult);
            if (reply.loaderThreadExitCode) out << L"; loader exit 0x" << std::hex << reply.loaderThreadExitCode;
            if (reply.requestThreadExitCode) out << L"; request exit 0x" << std::hex << reply.requestThreadExitCode;
        }
        return out.str();
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
        app::Operation operation;
        std::wstring targetTitle, referenceTitle;
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
            const bool elevated = app::IsAdministrator();
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
        auto caption = [](std::uint64_t hwnd)
        {
            wchar_t title[256]{};
            GetWindowTextW(reinterpret_cast<HWND>(hwnd), title, 256);
            if (title[0]) return std::wstring(title);
            std::wostringstream text;
            text << app::Local(L"窗口 0x", L"Window 0x") << std::hex << hwnd;
            return text.str();
        };
        job->targetTitle = caption(job->operation.request.target.hwnd);
        job->referenceTitle = caption(job->operation.request.reference.hwnd);
        UpdateEnabled();
        Log(app::Local(L"正在处理，请稍候…\r\n操作期间仍可移动窗口。",
            L"Working…\r\nThe window remains responsive while the operation runs."));
        try
        {
            g.worker = std::thread([job]
            {
                try
                {
                    job->operation.time = app::Timestamp();
                    job->operation.reply = ExecuteRequest(job->operation.request, (app::ExecutableDirectory() / L"DwmZOrder.dll").wstring());
                }
                catch (const std::exception& e)
                {
                    job->error = app::Local(L"操作失败：", L"Operation failed: ");
                    for (const char* p = e.what(); *p; ++p) job->error += static_cast<wchar_t>(static_cast<unsigned char>(*p));
                    job->operation.reply.response.status = Status::InternalException;
                }
                catch (...) { job->error = app::Local(L"发生未知异常。", L"An unexpected exception occurred."); job->operation.reply.response.status = Status::InternalException; }
                PostMessageW(job->owner, Completed, 0, 0);
            });
        }
        catch (...) { g.busy = false; g.job.reset(); UpdateEnabled(); Log(app::Local(L"无法启动操作线程。", L"Could not start the operation thread.")); }
    }

    void Start(Action action)
    {
        auto job = std::make_shared<Job>();
        auto& request = job->operation.request;
        request.action = action;
        request.position = static_cast<Position>(SendMessageW(g.position, CB_GETCURSEL, 0, 0));
        request.maintain = SendMessageW(g.maintain, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (action != Action::Stop && !Identity(g.target, request.target))
        { Log(app::Local(L"请选择有效窗口，或输入十进制 / 0x 十六进制窗口句柄。", L"Select a valid window or enter its decimal or 0x hexadecimal handle.")); return; }
        if (action == Action::Apply && request.position >= Position::Before
            && (!Identity(g.reference, request.reference) || request.reference.hwnd == request.target.hwnd))
        { Log(app::Local(L"请选择同一桌面上的另一个参照窗口。", L"Select another reference window on the same desktop.")); return; }
        if (action == Action::Stop && MessageBoxW(g.window,
            app::Local(L"这会停止当前会话的全部持续保持，并恢复系统顺序。是否继续？",
                L"This stops all maintained ordering in the current session and restores system order. Continue?"),
            app::Local(L"停止全部", L"Stop all"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        Begin(job);
    }

    void Close()
    {
        if (g.busy) { g.closePending = true; Log(app::Local(L"当前操作完成后关闭…", L"Closing after the current operation finishes…")); return; }
        g.closePending = false;
        if (g.held.hwnd)
        {
            const int choice = MessageBoxW(g.window,
                app::Local(L"此工具可能仍在持续保持窗口。关闭前恢复吗？\n\n是：恢复后关闭。\n否：保留保持并退出。",
                    L"This tool may still be maintaining a window. Restore before closing?\n\nYes: restore and close.\nNo: leave maintenance running."),
                app::Local(L"退出", L"Exit"), MB_YESNOCANCEL | MB_ICONQUESTION);
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
            Log(app::ResultText(job->operation, job->targetTitle, job->referenceTitle));
        }
        if (!job->error.empty()) Log(job->error);
        UpdateEnabled();
        if (job->closeAfterRestore)
        {
            if (job->error.empty() && job->operation.reply.response.status == Status::Ok) { DestroyWindow(g.window); return; }
            g.closePending = false;
            Log(Text(g.log) + L"\r\n\r\n" + app::Local(L"恢复未确认成功，工具保持打开。", L"Restore was not confirmed; the tool remains open."));
        }
        else if (g.closePending) Close();
    }

    void CreateUi()
    {
        g.dpi = GetDpiForWindow(g.window);
        g.font = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g.title = Item(L"STATIC", app::Local(L"DWM 窗口排序 1.0.3", L"DWM Window Order 1.0.3"), 0);
        g.scope = Item(L"STATIC", app::Local(L"跨 Band 合成排序；鼠标命中和焦点不变。", L"Changes composition order across bands; input and focus stay unchanged."), 0);
        Item(L"BUTTON", app::Local(L"以管理员身份重新启动", L"Restart as administrator"), AdminId, WS_TABSTOP);
        Item(L"BUTTON", app::Local(L"刷新窗口列表", L"Refresh window list"), RefreshId, WS_TABSTOP);
        g.targetLabel = Item(L"STATIC", app::Local(L"目标窗口", L"Target window"), 0);
        g.referenceLabel = Item(L"STATIC", app::Local(L"参照窗口", L"Reference window"), 0);
        g.positionLabel = Item(L"STATIC", app::Local(L"目标位置", L"Target position"), 0);
        g.target = Item(L"COMBOBOX", L"", TargetId, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP);
        g.reference = Item(L"COMBOBOX", L"", ReferenceId, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP);
        SendMessageW(g.target, CB_LIMITTEXT, 512, 0); SendMessageW(g.reference, CB_LIMITTEXT, 512, 0);
        g.position = Item(L"COMBOBOX", L"", PositionId, CBS_DROPDOWNLIST | WS_TABSTOP);
        for (const auto* text : {app::Local(L"最前方", L"Frontmost"), app::Local(L"最后方", L"Backmost"),
            app::Local(L"参照窗口上方", L"Above reference"), app::Local(L"参照窗口下方", L"Below reference")})
            SendMessageW(g.position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(g.position, CB_SETCURSEL, 0, 0);
        g.maintain = Item(L"BUTTON", app::Local(L"持续保持此顺序", L"Maintain this order"), MaintainId, BS_AUTOCHECKBOX | WS_TABSTOP);
        SendMessageW(g.maintain, BM_SETCHECK, BST_UNCHECKED, 0);
        Item(L"BUTTON", app::Local(L"读取顺序", L"Read order"), QueryId, WS_TABSTOP);
        Item(L"BUTTON", app::Local(L"应用顺序", L"Apply"), ApplyId, WS_TABSTOP);
        Item(L"BUTTON", app::Local(L"恢复目标", L"Restore"), RestoreId, WS_TABSTOP);
        Item(L"BUTTON", app::Local(L"停止全部保持", L"Stop all maintenance"), StopId, WS_TABSTOP);
        g.log = Item(L"EDIT", L"", LogId, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
        SendMessageW(g.log, EM_SETLIMITTEXT, 200000, 0);
        Layout(); Refresh(); UpdateEnabled();
        Log(app::Local(L"请选择目标窗口。\r\n\r\n点击“读取顺序”查看它当前的位置。\r\n选择位置后点击“应用顺序”，才会调整窗口的显示顺序。",
            L"Select a target window.\r\n\r\nClick Read order to see its current position.\r\nChoose a position and click Apply to change its display order."));
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
                    const auto exe = app::ExecutableDirectory() / L"DwmOrderTool.exe";
                    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"runas", exe.c_str(), nullptr, app::ExecutableDirectory().c_str(), SW_SHOWNORMAL));
                    if (result > 32) DestroyWindow(window);
                    else Log(app::Local(L"管理员重启被取消或未完成。", L"Administrator restart was cancelled or did not complete."));
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
        catch (...) { if (message == WM_CREATE) return -1; Log(app::Local(L"界面操作失败。", L"The interface operation failed.")); }
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
        cls.lpfnWndProc = MainWindow; cls.lpszClassName = L"DwmOrderWindow";
        if (!RegisterClassExW(&cls)) return 1;
        g.dpi = GetDpiForSystem();
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"DwmOrderWindow", app::Local(L"DWM 窗口排序", L"DWM Window Order"),
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
