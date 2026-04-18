// ==WindhawkMod==
// @id              tray-mirror-all-monitors
// @name            System Tray Mirror for All Monitors
// @description     Bring the Windows 11 system tray, overflow flyouts, and key tray popups to every monitor taskbar
// @version         1.0.0
// @author          Hans Gunnar Hansen
// @github          https://github.com/hghansen
// @license         MIT
// @include         explorer.exe
// @include         ShellExperienceHost.exe
// @include         ShellHost.exe
// @include         OneDrive.exe
// @architecture    x86-64
// @compilerOptions -luser32 -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# System Tray Mirror for All Monitors

This Windhawk mod mirrors the visible system tray from the primary taskbar onto
every secondary taskbar on Windows 11, while keeping common tray flyouts usable
on the monitor where you clicked.

## How it works

- Mirrors the rendered tray area from the primary taskbar onto each secondary
    taskbar.
- Keeps tray overflow, Quick Settings, Notification Center, and OneDrive
    aligned to the monitor where the mirrored tray was used.
- Uses lightweight overlay windows and best-effort input forwarding instead of
    trying to rebuild the internal tray model.

## Why this approach

Hooking `Shell_NotifyIconW` inside `explorer.exe` doesn't provide a reliable
source of truth for all tray icons because applications register icons from
their own processes. This mod mirrors the live rendered tray and repositions
the most important flyouts in the processes that actually own them.

## Limitations

- It mirrors what is visibly rendered, not the internal Windows tray model.
- Best suited for the default horizontal Windows 11 taskbar.
- Click forwarding is best-effort and depends on the primary tray layout.
- If Windows changes the tray host structure on a future build, the source
  tray window detection may need to be updated.
- On some builds, excluding the clock may not be possible, so the mirrored
  region can still include it.

## Settings

- `refreshIntervalMs`: refresh rate for the mirrored image.
- `includeClock`: if enabled, mirrors the whole primary tray host instead of
  trying to exclude the clock area.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- refreshIntervalMs: 200
  $name: Refresh interval (ms)
  $description: Lower values update the mirror more often but use more CPU.
- includeClock: false
  $name: Include the clock area
  $description: If disabled, the mod tries to crop the mirrored source before the primary clock.
*/
// ==/WindhawkModSettings==

#include <windhawk_api.h>

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <vector>

struct Settings {
    std::atomic<int> refreshIntervalMs{200};
    std::atomic<bool> includeClock{false};
} g_settings;

struct MirrorWindow {
    HWND hWnd = nullptr;
    HWND hTaskbar = nullptr;
    RECT hoverRect{0, 0, 0, 0};
    bool hasHoverRect = false;
};

static constexpr wchar_t kMirrorWindowClass[] = L"WhTrayMirrorWnd";
static constexpr wchar_t kControllerWindowClass[] = L"WhTrayMirrorControllerWnd";
static constexpr wchar_t kSharedFlyoutStateName[] =
    L"Local\\Windhawk_" WH_MOD_ID L"_FlyoutStateV1";
static constexpr wchar_t kOneDriveFlyoutClass[] =
    L"OneDriveReactNativeWin32WindowClass";
static constexpr UINT_PTR kRefreshTimerId = 1;
static constexpr UINT kMessageRefresh = WM_APP + 1;
static constexpr UINT kMessageApplySettings = WM_APP + 2;
static constexpr ULONGLONG kFlyoutRepositionTimeoutMs = 4000;

enum class Target {
    Explorer,
    ShellExperienceHost,
    ShellHost,
    OneDrive,
    Unknown,
};

struct SharedFlyoutState {
    volatile LONG sequence = 0;
    ULONGLONG tick = 0;
    RECT taskbarRect{0, 0, 0, 0};
};

LRESULT CALLBACK ControllerWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
bool AdjustFlyoutPosition(HWND hWnd,
                         int* x,
                         int* y,
                         int cx,
                         int cy,
                         UINT uFlags);

std::vector<MirrorWindow> g_mirrorWindows;

HINSTANCE g_hInstance = nullptr;
HWND g_controllerWindow = nullptr;
HANDLE g_workerThread = nullptr;
DWORD g_workerThreadId = 0;
Target g_target = Target::Unknown;
HANDLE g_sharedFlyoutStateMapping = nullptr;
SharedFlyoutState* g_sharedFlyoutState = nullptr;

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original = nullptr;

using MoveWindow_t = decltype(&MoveWindow);
MoveWindow_t MoveWindow_Original = nullptr;

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original = nullptr;

using ShowWindowAsync_t = decltype(&ShowWindowAsync);
ShowWindowAsync_t ShowWindowAsync_Original = nullptr;

Target DetectTargetProcess() {
    wchar_t modulePath[MAX_PATH];
    switch (GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) {
    case 0:
    case ARRAYSIZE(modulePath):
        return Target::Unknown;
    }

    PCWSTR moduleName = wcsrchr(modulePath, L'\\');
    moduleName = moduleName ? moduleName + 1 : modulePath;

    if (_wcsicmp(moduleName, L"explorer.exe") == 0) {
        return Target::Explorer;
    }

    if (_wcsicmp(moduleName, L"ShellExperienceHost.exe") == 0) {
        return Target::ShellExperienceHost;
    }

    if (_wcsicmp(moduleName, L"ShellHost.exe") == 0) {
        return Target::ShellHost;
    }

    if (_wcsicmp(moduleName, L"OneDrive.exe") == 0) {
        return Target::OneDrive;
    }

    return Target::Unknown;
}

bool IsExplorerTarget() {
    return g_target == Target::Explorer;
}

bool InitSharedFlyoutState() {
    g_sharedFlyoutStateMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SharedFlyoutState),
        kSharedFlyoutStateName
    );
    if (!g_sharedFlyoutStateMapping) {
        return false;
    }

    const bool mappingAlreadyExisted = GetLastError() == ERROR_ALREADY_EXISTS;

    g_sharedFlyoutState = reinterpret_cast<SharedFlyoutState*>(
        MapViewOfFile(g_sharedFlyoutStateMapping,
                      FILE_MAP_READ | FILE_MAP_WRITE,
                      0,
                      0,
                      sizeof(SharedFlyoutState))
    );
    if (!g_sharedFlyoutState) {
        CloseHandle(g_sharedFlyoutStateMapping);
        g_sharedFlyoutStateMapping = nullptr;
        return false;
    }

    if (!mappingAlreadyExisted) {
        ZeroMemory(g_sharedFlyoutState, sizeof(*g_sharedFlyoutState));
    }

    return true;
}

void CleanupSharedFlyoutState() {
    if (g_sharedFlyoutState) {
        UnmapViewOfFile(g_sharedFlyoutState);
        g_sharedFlyoutState = nullptr;
    }

    if (g_sharedFlyoutStateMapping) {
        CloseHandle(g_sharedFlyoutStateMapping);
        g_sharedFlyoutStateMapping = nullptr;
    }
}

void RecordRecentFlyoutAnchor(const RECT& taskbarRect) {
    if (!g_sharedFlyoutState) {
        return;
    }

    InterlockedIncrement(&g_sharedFlyoutState->sequence);
    g_sharedFlyoutState->taskbarRect = taskbarRect;
    MemoryBarrier();
    g_sharedFlyoutState->tick = GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement(&g_sharedFlyoutState->sequence);
}

bool GetRecentFlyoutAnchorRect(RECT* taskbarRect) {
    if (!g_sharedFlyoutState) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        LONG sequenceBefore = g_sharedFlyoutState->sequence;
        if (sequenceBefore & 1) {
            continue;
        }

        MemoryBarrier();
        ULONGLONG tick = g_sharedFlyoutState->tick;
        RECT rect = g_sharedFlyoutState->taskbarRect;
        MemoryBarrier();

        LONG sequenceAfter = g_sharedFlyoutState->sequence;
        if (sequenceBefore != sequenceAfter || (sequenceAfter & 1)) {
            continue;
        }

        if (!tick || GetTickCount64() - tick > kFlyoutRepositionTimeoutMs) {
            return false;
        }

        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            return false;
        }

        *taskbarRect = rect;
        return true;
    }

    return false;
}

bool GetThreadDescriptionEquals(DWORD threadId, PCWSTR expectedDescription) {
    HMODULE kernel32Module = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32Module) {
        return false;
    }

    auto getThreadDescription = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PWSTR*)>(
        GetProcAddress(kernel32Module, "GetThreadDescription")
    );
    if (!getThreadDescription) {
        return false;
    }

    HANDLE threadHandle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (!threadHandle) {
        return false;
    }

    PWSTR threadDescription = nullptr;
    HRESULT hr = getThreadDescription(threadHandle, &threadDescription);
    CloseHandle(threadHandle);

    bool matched = SUCCEEDED(hr) && threadDescription &&
                   _wcsicmp(threadDescription, expectedDescription) == 0;
    if (threadDescription) {
        LocalFree(threadDescription);
    }

    return matched;
}

int ClampRefreshInterval(int value) {
    if (value < 16) {
        return 16;
    }

    if (value > 2000) {
        return 2000;
    }

    return value;
}

void LoadSettings() {
    g_settings.refreshIntervalMs.store(
        ClampRefreshInterval(Wh_GetIntSetting(L"refreshIntervalMs"))
    );
    g_settings.includeClock.store(Wh_GetIntSetting(L"includeClock") != 0);
}

bool IsWindowFromCurrentProcess(HWND hWnd) {
    DWORD processId = 0;
    if (!GetWindowThreadProcessId(hWnd, &processId)) {
        return false;
    }

    return processId == GetCurrentProcessId();
}

bool ClassNameEquals(HWND hWnd, PCWSTR className) {
    wchar_t buffer[64];
    int length = GetClassNameW(hWnd, buffer, ARRAYSIZE(buffer));
    return length > 0 && _wcsicmp(buffer, className) == 0;
}

HWND FindDescendantByClass(HWND parent, PCWSTR className) {
    for (HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr);
         child;
         child = FindWindowExW(parent, child, nullptr, nullptr)) {
        if (ClassNameEquals(child, className)) {
            return child;
        }

        if (HWND nested = FindDescendantByClass(child, className)) {
            return nested;
        }
    }

    return nullptr;
}

HWND FindPrimaryTaskbarWindow() {
    HWND result = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            if (!IsWindowFromCurrentProcess(hWnd) ||
                !ClassNameEquals(hWnd, L"Shell_TrayWnd")) {
                return TRUE;
            }

            *reinterpret_cast<HWND*>(lParam) = hWnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&result)
    );

    return result;
}

std::vector<HWND> FindSecondaryTaskbars() {
    std::vector<HWND> result;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            if (!IsWindowFromCurrentProcess(hWnd) ||
                !ClassNameEquals(hWnd, L"Shell_SecondaryTrayWnd")) {
                return TRUE;
            }

            auto* windows = reinterpret_cast<std::vector<HWND>*>(lParam);
            windows->push_back(hWnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result)
    );

    return result;
}

bool IsHorizontalTaskbar(HWND hTaskbar) {
    RECT rc{};
    if (!GetClientRect(hTaskbar, &rc)) {
        return false;
    }

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    return width > 0 && height > 0 && width >= height * 2;
}

bool GetWindowRectInParentClient(HWND hChild, HWND hParent, RECT* rect) {
    if (!GetWindowRect(hChild, rect)) {
        return false;
    }

    MapWindowPoints(HWND_DESKTOP, hParent,
                    reinterpret_cast<POINT*>(rect), 2);
    return true;
}

bool GetTaskbarRectOnScreen(HWND hTaskbar, RECT* rect) {
    if (!GetWindowRect(hTaskbar, rect)) {
        return false;
    }

    return rect->right > rect->left && rect->bottom > rect->top;
}

bool RectEquals(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

bool GetMonitorWorkArea(HMONITOR monitor, RECT* rect) {
    MONITORINFO monitorInfo{
        .cbSize = sizeof(MONITORINFO),
    };

    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    *rect = monitorInfo.rcWork;
    return true;
}

bool GetSourceTrayRect(RECT* rect) {
    if (!IsExplorerTarget()) {
        return false;
    }

    HWND hTaskbar = FindPrimaryTaskbarWindow();
    if (!hTaskbar || !IsHorizontalTaskbar(hTaskbar)) {
        return false;
    }

    HWND hTrayNotify = FindDescendantByClass(hTaskbar, L"TrayNotifyWnd");
    if (!hTrayNotify) {
        return false;
    }

    if (!GetWindowRect(hTrayNotify, rect)) {
        return false;
    }

    if (!g_settings.includeClock.load()) {
        HWND hClock = FindDescendantByClass(hTrayNotify, L"TrayClockWClass");
        if (!hClock) {
            hClock = FindDescendantByClass(hTaskbar, L"TrayClockWClass");
        }

        RECT clockRect{};
        if (hClock && GetWindowRect(hClock, &clockRect) &&
            clockRect.left > rect->left && clockRect.left < rect->right) {
            rect->right = clockRect.left;
        }
    }

    return rect->right > rect->left && rect->bottom > rect->top;
}

RECT CalcMirrorRect(HWND hTaskbar) {
    RECT clientRect{};
    GetClientRect(hTaskbar, &clientRect);

    RECT sourceRect{};
    if (!GetSourceTrayRect(&sourceRect) || !IsHorizontalTaskbar(hTaskbar)) {
        return RECT{0, 0, 0, 0};
    }

    const int taskbarHeight = static_cast<int>(clientRect.bottom - clientRect.top);
    const int taskbarWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int sourceWidth = static_cast<int>(sourceRect.right - sourceRect.left);
    const int sourceHeight = std::max(1, static_cast<int>(sourceRect.bottom - sourceRect.top));

    int mirrorWidth = MulDiv(sourceWidth, taskbarHeight, sourceHeight);
    mirrorWidth = std::min(mirrorWidth, taskbarWidth);

    int anchorLeft = clientRect.right;

    HWND hTrayNotify = FindDescendantByClass(hTaskbar, L"TrayNotifyWnd");
    RECT anchorRect{};
    if (hTrayNotify && GetWindowRectInParentClient(hTrayNotify, hTaskbar, &anchorRect)) {
        anchorLeft = anchorRect.left;
    } else {
        HWND hClock = FindDescendantByClass(hTaskbar, L"TrayClockWClass");
        if (hClock && GetWindowRectInParentClient(hClock, hTaskbar, &anchorRect)) {
            anchorLeft = anchorRect.left;
        }
    }

    if (anchorLeft <= clientRect.left) {
        anchorLeft = clientRect.right;
    }

    RECT mirrorRect{};
    mirrorRect.left = std::max(clientRect.left, static_cast<LONG>(anchorLeft - mirrorWidth));
    mirrorRect.top = clientRect.top;
    mirrorRect.right = anchorLeft;
    mirrorRect.bottom = clientRect.bottom;
    return mirrorRect;
}

RECT CalcMirrorRectScreen(HWND hTaskbar) {
    RECT mirrorRect = CalcMirrorRect(hTaskbar);
    RECT taskbarRect{};
    if (!GetTaskbarRectOnScreen(hTaskbar, &taskbarRect)) {
        return RECT{0, 0, 0, 0};
    }

    OffsetRect(&mirrorRect, taskbarRect.left, taskbarRect.top);
    return mirrorRect;
}

MirrorWindow* FindMirrorByTaskbar(HWND hTaskbar) {
    auto it = std::find_if(
        g_mirrorWindows.begin(),
        g_mirrorWindows.end(),
        [hTaskbar](const MirrorWindow& mirror) {
            return mirror.hTaskbar == hTaskbar;
        }
    );

    return it != g_mirrorWindows.end() ? &*it : nullptr;
}

MirrorWindow* FindMirrorByWindow(HWND hWnd) {
    auto it = std::find_if(
        g_mirrorWindows.begin(),
        g_mirrorWindows.end(),
        [hWnd](const MirrorWindow& mirror) {
            return mirror.hWnd == hWnd;
        }
    );

    return it != g_mirrorWindows.end() ? &*it : nullptr;
}

void RecordRecentMirroredClick(HWND hMirror) {
    const MirrorWindow* mirror = FindMirrorByWindow(hMirror);
    if (!mirror || !mirror->hTaskbar || !IsWindow(mirror->hTaskbar)) {
        return;
    }

    RECT taskbarRect{};
    if (!GetTaskbarRectOnScreen(mirror->hTaskbar, &taskbarRect)) {
        return;
    }

    RecordRecentFlyoutAnchor(taskbarRect);
}

bool IsExplorerFlyoutClass(HWND hWnd, PCWSTR className) {
    if (_wcsicmp(className, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
        _wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0) {
        return true;
    }

    if (_wcsicmp(className, L"Xaml_WindowedPopupClass") != 0) {
        return false;
    }

    HWND rootOwner = GetAncestor(hWnd, GA_ROOTOWNER);
    return rootOwner &&
           (ClassNameEquals(rootOwner, L"TopLevelWindowForOverflowXamlIsland") ||
            ClassNameEquals(rootOwner, L"XamlExplorerHostIslandWindow"));
}

bool IsQuickSettingsFlyoutWindow(HWND hWnd, PCWSTR className) {
    if (g_target == Target::ShellHost) {
        return _wcsicmp(className, L"ControlCenterWindow") == 0;
    }

    if (g_target != Target::ShellExperienceHost ||
        _wcsicmp(className, L"Windows.UI.Core.CoreWindow") != 0) {
        return false;
    }

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    return threadId &&
           (GetThreadDescriptionEquals(threadId, L"QuickActions") ||
            GetThreadDescriptionEquals(threadId, L"ActionCenter"));
}

bool IsOneDriveKnownFlyoutClass(PCWSTR className) {
    return _wcsicmp(className, kOneDriveFlyoutClass) == 0;
}

bool IsOneDriveFlyoutWindow(HWND hWnd, int cx, int cy, UINT uFlags) {
    if (g_target != Target::OneDrive) {
        return false;
    }

    wchar_t className[64];
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) == 0) {
        return false;
    }

    if (IsOneDriveKnownFlyoutClass(className)) {
        return true;
    }

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);

    if ((uFlags & SWP_HIDEWINDOW) || (style & WS_CHILD)) {
        return false;
    }

    HWND rootWindow = GetAncestor(hWnd, GA_ROOT);
    if (rootWindow && rootWindow != hWnd) {
        return false;
    }

    const int minWidth = MulDiv(180, GetDpiForWindow(hWnd), 96);
    const int minHeight = MulDiv(120, GetDpiForWindow(hWnd), 96);
    if (cx < minWidth || cy < minHeight) {
        return false;
    }

    if (!(style & WS_POPUP) && !(exStyle & WS_EX_TOOLWINDOW)) {
        return false;
    }

    if (style & WS_CAPTION) {
        return false;
    }

    return true;
}

bool AdjustExistingFlyoutWindow(HWND hWnd) {
    RECT rect{};
    if (!GetWindowRect(hWnd, &rect)) {
        return false;
    }

    int x = rect.left;
    int y = rect.top;
    int cx = rect.right - rect.left;
    int cy = rect.bottom - rect.top;
    if (!AdjustFlyoutPosition(hWnd, &x, &y, cx, cy, 0)) {
        return false;
    }

    return SetWindowPos_Original(hWnd,
                                 nullptr,
                                 x,
                                 y,
                                 0,
                                 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

bool IsTargetFlyoutWindow(HWND hWnd, PCWSTR className, int cx, int cy, UINT uFlags) {
    switch (g_target) {
    case Target::Explorer:
        return IsExplorerFlyoutClass(hWnd, className);

    case Target::ShellExperienceHost:
    case Target::ShellHost:
        return IsQuickSettingsFlyoutWindow(hWnd, className);

    case Target::OneDrive:
        return IsOneDriveFlyoutWindow(hWnd, cx, cy, uFlags);

    case Target::Unknown:
        return false;
    }

    return false;
}

bool AdjustFlyoutPosition(HWND hWnd,
                         int* x,
                         int* y,
                         int cx,
                         int cy,
                         UINT uFlags) {
    if ((uFlags & SWP_NOMOVE) || cx <= 0 || cy <= 0) {
        return false;
    }

    RECT taskbarRect{};
    if (!GetRecentFlyoutAnchorRect(&taskbarRect)) {
        return false;
    }

    wchar_t className[64];
    if (GetClassNameW(hWnd, className, ARRAYSIZE(className)) == 0) {
        return false;
    }

    if (!IsTargetFlyoutWindow(hWnd, className, cx, cy, uFlags)) {
        return false;
    }

    const HMONITOR monitor = MonitorFromRect(&taskbarRect, MONITOR_DEFAULTTONEAREST);
    RECT workArea{};
    if (!GetMonitorWorkArea(monitor, &workArea)) {
        return false;
    }

    const int margin = MulDiv(8, GetDpiForWindow(hWnd), 96);
    int xNew = taskbarRect.right - cx;
    int yNew = taskbarRect.top - cy - margin;

    if (xNew < workArea.left) {
        xNew = workArea.left;
    } else if (xNew > workArea.right - cx) {
        xNew = workArea.right - cx;
    }

    if (yNew < workArea.top) {
        yNew = workArea.top;
    } else if (yNew > workArea.bottom - cy) {
        yNew = workArea.bottom - cy;
    }

    if (*x == xNew && *y == yNew) {
        return false;
    }

    *x = xNew;
    *y = yNew;
    Wh_Log(L"Adjusting flyout %s to %d,%d on secondary monitor", className, xNew, yNew);
    return true;
}

bool MapMirrorPointToSource(HWND hMirror, POINT mirrorPoint, POINT* sourcePoint) {
    RECT sourceRect{};
    RECT clientRect{};
    if (!GetSourceTrayRect(&sourceRect) || !GetClientRect(hMirror, &clientRect)) {
        return false;
    }

    const int sourceWidth = static_cast<int>(sourceRect.right - sourceRect.left);
    const int sourceHeight = static_cast<int>(sourceRect.bottom - sourceRect.top);
    const int mirrorWidth = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int mirrorHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));

    sourcePoint->x = sourceRect.left + MulDiv(mirrorPoint.x, sourceWidth, mirrorWidth);
    sourcePoint->y = sourceRect.top + MulDiv(mirrorPoint.y, sourceHeight, mirrorHeight);
    return true;
}

bool MapSourceRectToMirror(HWND hMirror, const RECT& sourceItemRect, RECT* mirrorItemRect) {
    RECT sourceRect{};
    RECT clientRect{};
    if (!GetSourceTrayRect(&sourceRect) || !GetClientRect(hMirror, &clientRect)) {
        return false;
    }

    RECT clippedRect{};
    if (!IntersectRect(&clippedRect, &sourceItemRect, &sourceRect)) {
        return false;
    }

    const int sourceWidth = std::max(1, static_cast<int>(sourceRect.right - sourceRect.left));
    const int sourceHeight = std::max(1, static_cast<int>(sourceRect.bottom - sourceRect.top));
    const int mirrorWidth = static_cast<int>(clientRect.right - clientRect.left);
    const int mirrorHeight = static_cast<int>(clientRect.bottom - clientRect.top);

    mirrorItemRect->left = MulDiv(clippedRect.left - sourceRect.left, mirrorWidth, sourceWidth);
    mirrorItemRect->top = MulDiv(clippedRect.top - sourceRect.top, mirrorHeight, sourceHeight);
    mirrorItemRect->right = MulDiv(clippedRect.right - sourceRect.left, mirrorWidth, sourceWidth);
    mirrorItemRect->bottom = MulDiv(clippedRect.bottom - sourceRect.top, mirrorHeight, sourceHeight);

    mirrorItemRect->left = std::max(clientRect.left, mirrorItemRect->left);
    mirrorItemRect->top = std::max(clientRect.top, mirrorItemRect->top);
    mirrorItemRect->right = std::min(clientRect.right, mirrorItemRect->right);
    mirrorItemRect->bottom = std::min(clientRect.bottom, mirrorItemRect->bottom);

    return mirrorItemRect->right > mirrorItemRect->left &&
           mirrorItemRect->bottom > mirrorItemRect->top;
}

bool TryGetHoverRectForSourcePoint(POINT sourcePoint, RECT* hoverRect) {
    RECT sourceTrayRect{};
    if (!GetSourceTrayRect(&sourceTrayRect)) {
        return false;
    }

    HWND hTarget = WindowFromPoint(sourcePoint);
    if (!hTarget || !GetWindowRect(hTarget, hoverRect)) {
        return false;
    }

    RECT clippedRect{};
    if (!IntersectRect(&clippedRect, hoverRect, &sourceTrayRect)) {
        return false;
    }

    const int clippedWidth = static_cast<int>(clippedRect.right - clippedRect.left);
    const int clippedHeight = static_cast<int>(clippedRect.bottom - clippedRect.top);
    const int sourceWidth = static_cast<int>(sourceTrayRect.right - sourceTrayRect.left);
    const int sourceHeight = static_cast<int>(sourceTrayRect.bottom - sourceTrayRect.top);

    if (clippedWidth <= 0 || clippedHeight <= 0 ||
        clippedWidth >= sourceWidth || clippedHeight >= sourceHeight) {
        return false;
    }

    *hoverRect = clippedRect;
    return true;
}

bool ScreenPointToAbsolute(POINT point, LONG* absoluteX, LONG* absoluteY) {
    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (virtualWidth <= 1 || virtualHeight <= 1) {
        return false;
    }

    *absoluteX = MulDiv(point.x - virtualLeft, 65535, virtualWidth - 1);
    *absoluteY = MulDiv(point.y - virtualTop, 65535, virtualHeight - 1);
    return true;
}

DWORD GetMouseButtonDownFlag(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        return MOUSEEVENTF_LEFTDOWN;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP:
        return MOUSEEVENTF_RIGHTDOWN;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return MOUSEEVENTF_MIDDLEDOWN;
    }

    return 0;
}

DWORD GetMouseButtonUpFlag(UINT message) {
    switch (message) {
    case WM_LBUTTONUP:
        return MOUSEEVENTF_LEFTUP;

    case WM_RBUTTONUP:
        return MOUSEEVENTF_RIGHTUP;

    case WM_MBUTTONUP:
        return MOUSEEVENTF_MIDDLEUP;
    }

    return 0;
}

void FillAbsoluteMouseInput(INPUT* input, LONG absoluteX, LONG absoluteY, DWORD flags) {
    input->type = INPUT_MOUSE;
    input->mi.dx = absoluteX;
    input->mi.dy = absoluteY;
    input->mi.mouseData = 0;
    input->mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
    input->mi.time = 0;
    input->mi.dwExtraInfo = 0;
}

bool InjectMouseFlagsAtPoint(POINT sourcePoint, DWORD flags) {
    POINT originalCursor{};
    if (!GetCursorPos(&originalCursor)) {
        return false;
    }

    LONG sourceAbsoluteX = 0;
    LONG sourceAbsoluteY = 0;
    LONG originalAbsoluteX = 0;
    LONG originalAbsoluteY = 0;
    if (!ScreenPointToAbsolute(sourcePoint, &sourceAbsoluteX, &sourceAbsoluteY) ||
        !ScreenPointToAbsolute(originalCursor, &originalAbsoluteX, &originalAbsoluteY)) {
        return false;
    }

    INPUT inputs[3]{};
    FillAbsoluteMouseInput(&inputs[0], sourceAbsoluteX, sourceAbsoluteY, MOUSEEVENTF_MOVE);
    FillAbsoluteMouseInput(&inputs[1], sourceAbsoluteX, sourceAbsoluteY, flags);
    FillAbsoluteMouseInput(&inputs[2], originalAbsoluteX, originalAbsoluteY, MOUSEEVENTF_MOVE);

    return SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)) == ARRAYSIZE(inputs);
}

bool InjectMouseClickToSource(POINT sourcePoint, UINT message) {
    return InjectMouseFlagsAtPoint(sourcePoint,
                                   GetMouseButtonDownFlag(message) |
                                       GetMouseButtonUpFlag(message));
}

void SetMirrorHoverRect(HWND hMirror, const RECT* hoverRect) {
    MirrorWindow* mirror = FindMirrorByWindow(hMirror);
    if (!mirror) {
        return;
    }

    bool changed = false;
    if (hoverRect) {
        if (!mirror->hasHoverRect || !RectEquals(mirror->hoverRect, *hoverRect)) {
            mirror->hoverRect = *hoverRect;
            mirror->hasHoverRect = true;
            changed = true;
        }
    } else if (mirror->hasHoverRect) {
        mirror->hasHoverRect = false;
        mirror->hoverRect = RECT{0, 0, 0, 0};
        changed = true;
    }

    if (changed) {
        InvalidateRect(hMirror, nullptr, FALSE);
    }
}

void UpdateMirrorHover(HWND hMirror, POINT mirrorPoint) {
    TRACKMOUSEEVENT trackMouseEvent{
        sizeof(TRACKMOUSEEVENT),
        TME_LEAVE,
        hMirror,
        HOVER_DEFAULT,
    };
    TrackMouseEvent(&trackMouseEvent);

    POINT sourcePoint{};
    if (!MapMirrorPointToSource(hMirror, mirrorPoint, &sourcePoint)) {
        SetMirrorHoverRect(hMirror, nullptr);
        return;
    }

    RECT sourceHoverRect{};
    if (!TryGetHoverRectForSourcePoint(sourcePoint, &sourceHoverRect)) {
        SetMirrorHoverRect(hMirror, nullptr);
        return;
    }

    RECT mirrorHoverRect{};
    if (!MapSourceRectToMirror(hMirror, sourceHoverRect, &mirrorHoverRect)) {
        SetMirrorHoverRect(hMirror, nullptr);
        return;
    }

    SetMirrorHoverRect(hMirror, &mirrorHoverRect);
}

void EnsureMirrorTopmost(HWND hMirror) {
    SetWindowPos(hMirror,
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void DrawMirrorHoverRect(HWND hWnd, HDC hdc) {
    const MirrorWindow* mirror = FindMirrorByWindow(hWnd);
    if (!mirror || !mirror->hasHoverRect) {
        return;
    }

    HBRUSH outerBrush = CreateSolidBrush(RGB(235, 242, 255));
    FrameRect(hdc, &mirror->hoverRect, outerBrush);
    DeleteObject(outerBrush);

    RECT innerRect = mirror->hoverRect;
    InflateRect(&innerRect, -1, -1);
    if (innerRect.right > innerRect.left && innerRect.bottom > innerRect.top) {
        HBRUSH innerBrush = CreateSolidBrush(RGB(164, 198, 255));
        FrameRect(hdc, &innerRect, innerBrush);
        DeleteObject(innerBrush);
    }
}

void ForwardMouseToSource(HWND hMirror, UINT message, WPARAM wParam, LPARAM lParam) {
    POINT sourcePoint{};
    POINT mirrorPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (!MapMirrorPointToSource(hMirror, mirrorPoint, &sourcePoint)) {
        return;
    }

    EnsureMirrorTopmost(hMirror);

    if (message == WM_MOUSEMOVE) {
        UpdateMirrorHover(hMirror, mirrorPoint);
    }

    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
        message == WM_RBUTTONDBLCLK) {
        SetCapture(hMirror);
        return;
    }

    if (message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP) {
        ReleaseCapture();
        RecordRecentMirroredClick(hMirror);
        InjectMouseClickToSource(sourcePoint, message);
        return;
    }

    HWND hTarget = WindowFromPoint(sourcePoint);
    if (!hTarget) {
        return;
    }

    POINT targetClientPoint = sourcePoint;
    ScreenToClient(hTarget, &targetClientPoint);
    LPARAM targetLParam = MAKELPARAM(targetClientPoint.x, targetClientPoint.y);

    if (message != WM_MOUSEMOVE) {
        PostMessageW(hTarget, WM_MOUSEMOVE, 0, targetLParam);
    }

    PostMessageW(hTarget, message, wParam, targetLParam);
}

void PaintMirrorWindow(HWND hWnd, HDC hdc) {
    RECT clientRect{};
    GetClientRect(hWnd, &clientRect);

    RECT sourceRect{};
    if (!GetSourceTrayRect(&sourceRect)) {
        HBRUSH backgroundBrush = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdc, &clientRect, backgroundBrush);
        DeleteObject(backgroundBrush);
        return;
    }

    const int clientWidth = clientRect.right - clientRect.left;
    const int clientHeight = clientRect.bottom - clientRect.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
        return;
    }

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return;
    }

    HDC memoryDc = CreateCompatibleDC(hdc);
    if (!memoryDc) {
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HBITMAP bitmap = CreateCompatibleBitmap(hdc, clientWidth, clientHeight);
    if (!bitmap) {
        DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    HBRUSH backgroundBrush = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(memoryDc, &clientRect, backgroundBrush);
    DeleteObject(backgroundBrush);

    SetStretchBltMode(memoryDc, HALFTONE);
    SetBrushOrgEx(memoryDc, 0, 0, nullptr);

    StretchBlt(memoryDc,
               0,
               0,
               clientWidth,
               clientHeight,
               screenDc,
               sourceRect.left,
               sourceRect.top,
               sourceRect.right - sourceRect.left,
               sourceRect.bottom - sourceRect.top,
               SRCCOPY);

    BitBlt(hdc, 0, 0, clientWidth, clientHeight, memoryDc, 0, 0, SRCCOPY);
    DrawMirrorHoverRect(hWnd, hdc);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}

LRESULT CALLBACK MirrorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paintStruct{};
        HDC hdc = BeginPaint(hWnd, &paintStruct);
        PaintMirrorWindow(hWnd, hdc);
        EndPaint(hWnd, &paintStruct);
        return 0;
    }

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        ForwardMouseToSource(hWnd, message, wParam, lParam);
        return 0;

    case WM_CAPTURECHANGED:
        return 0;

    case WM_MOUSELEAVE:
        SetMirrorHoverRect(hWnd, nullptr);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

void RegisterWindowClasses() {
    WNDCLASSEXW mirrorClass{};
    mirrorClass.cbSize = sizeof(mirrorClass);
    mirrorClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    mirrorClass.lpfnWndProc = MirrorWindowProc;
    mirrorClass.hInstance = g_hInstance;
    mirrorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mirrorClass.lpszClassName = kMirrorWindowClass;
    RegisterClassExW(&mirrorClass);

    WNDCLASSEXW controllerClass{};
    controllerClass.cbSize = sizeof(controllerClass);
    controllerClass.lpfnWndProc = ControllerWindowProc;
    controllerClass.hInstance = g_hInstance;
    controllerClass.lpszClassName = kControllerWindowClass;
    RegisterClassExW(&controllerClass);
}

void DestroyAllMirrors() {
    for (const auto& mirror : g_mirrorWindows) {
        if (mirror.hWnd && IsWindow(mirror.hWnd)) {
            DestroyWindow(mirror.hWnd);
        }
    }

    g_mirrorWindows.clear();
}

void EnsureMirrorWindow(HWND hTaskbar) {
    if (FindMirrorByTaskbar(hTaskbar)) {
        return;
    }

    RECT rect = CalcMirrorRectScreen(hTaskbar);
    HWND hMirror = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kMirrorWindowClass,
        L"",
        WS_POPUP | WS_VISIBLE,
        rect.left,
        rect.top,
        std::max(0, static_cast<int>(rect.right - rect.left)),
        std::max(0, static_cast<int>(rect.bottom - rect.top)),
        nullptr,
        nullptr,
        g_hInstance,
        nullptr
    );

    if (!hMirror) {
        Wh_Log(L"CreateWindowExW failed for secondary taskbar %p", hTaskbar);
        return;
    }

    g_mirrorWindows.push_back({hMirror, hTaskbar});
}

void RefreshMirrorWindows() {
    const auto secondaryTaskbars = FindSecondaryTaskbars();

    g_mirrorWindows.erase(
        std::remove_if(
            g_mirrorWindows.begin(),
            g_mirrorWindows.end(),
            [&secondaryTaskbars](const MirrorWindow& mirror) {
                if (!mirror.hTaskbar || !IsWindow(mirror.hTaskbar)) {
                    if (mirror.hWnd && IsWindow(mirror.hWnd)) {
                        DestroyWindow(mirror.hWnd);
                    }
                    return true;
                }

                if (std::find(secondaryTaskbars.begin(), secondaryTaskbars.end(),
                              mirror.hTaskbar) == secondaryTaskbars.end()) {
                    if (mirror.hWnd && IsWindow(mirror.hWnd)) {
                        DestroyWindow(mirror.hWnd);
                    }
                    return true;
                }

                return false;
            }
        ),
        g_mirrorWindows.end()
    );

    for (HWND hTaskbar : secondaryTaskbars) {
        EnsureMirrorWindow(hTaskbar);
    }

    RECT sourceRect{};
    const bool haveSource = GetSourceTrayRect(&sourceRect);

    for (const auto& mirror : g_mirrorWindows) {
        if (!mirror.hWnd || !IsWindow(mirror.hWnd) || !mirror.hTaskbar ||
            !IsWindow(mirror.hTaskbar) || !haveSource ||
            !IsHorizontalTaskbar(mirror.hTaskbar)) {
            if (mirror.hWnd && IsWindow(mirror.hWnd)) {
                ShowWindow(mirror.hWnd, SW_HIDE);
            }
            continue;
        }

        RECT rect = CalcMirrorRectScreen(mirror.hTaskbar);
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            ShowWindow(mirror.hWnd, SW_HIDE);
            continue;
        }

        RECT currentRect{};
        const bool haveCurrentRect = GetWindowRect(mirror.hWnd, &currentRect) != FALSE;
        const bool rectChanged = !haveCurrentRect || !RectEquals(currentRect, rect);
        const bool visible = IsWindowVisible(mirror.hWnd) != FALSE;

        if (rectChanged || !visible) {
            UINT flags = SWP_NOACTIVATE;
            if (visible) {
                flags |= SWP_NOZORDER;
            } else {
                flags |= SWP_SHOWWINDOW;
            }

            SetWindowPos(mirror.hWnd,
                         visible ? nullptr : HWND_TOPMOST,
                         rect.left,
                         rect.top,
                         rect.right - rect.left,
                         rect.bottom - rect.top,
                         flags);
        }

        EnsureMirrorTopmost(mirror.hWnd);

        InvalidateRect(mirror.hWnd, nullptr, FALSE);
    }
}

BOOL WINAPI SetWindowPos_Hook(HWND hWnd,
                              HWND hWndInsertAfter,
                              int X,
                              int Y,
                              int cx,
                              int cy,
                              UINT uFlags) {
    AdjustFlyoutPosition(hWnd, &X, &Y, cx, cy, uFlags);
    return SetWindowPos_Original(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

BOOL WINAPI MoveWindow_Hook(HWND hWnd,
                            int X,
                            int Y,
                            int nWidth,
                            int nHeight,
                            BOOL bRepaint) {
    AdjustFlyoutPosition(hWnd, &X, &Y, nWidth, nHeight, 0);
    return MoveWindow_Original(hWnd, X, Y, nWidth, nHeight, bRepaint);
}

BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    BOOL result = ShowWindow_Original(hWnd, nCmdShow);

    if (g_target == Target::OneDrive &&
        nCmdShow != SW_HIDE &&
        nCmdShow != SW_MINIMIZE &&
        nCmdShow != SW_SHOWMINIMIZED &&
        nCmdShow != SW_FORCEMINIMIZE) {
        AdjustExistingFlyoutWindow(hWnd);
    }

    return result;
}

BOOL WINAPI ShowWindowAsync_Hook(HWND hWnd, int nCmdShow) {
    BOOL result = ShowWindowAsync_Original(hWnd, nCmdShow);

    if (g_target == Target::OneDrive &&
        nCmdShow != SW_HIDE &&
        nCmdShow != SW_MINIMIZE &&
        nCmdShow != SW_SHOWMINIMIZED &&
        nCmdShow != SW_FORCEMINIMIZE) {
        AdjustExistingFlyoutWindow(hWnd);
    }

    return result;
}

LRESULT CALLBACK ControllerWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SetTimer(hWnd,
                 kRefreshTimerId,
                 g_settings.refreshIntervalMs.load(),
                 nullptr);
        PostMessageW(hWnd, kMessageRefresh, 0, 0);
        return 0;

    case WM_TIMER:
        if (wParam == kRefreshTimerId) {
            RefreshMirrorWindows();
        }
        return 0;

    case kMessageRefresh:
        RefreshMirrorWindows();
        return 0;

    case kMessageApplySettings:
        KillTimer(hWnd, kRefreshTimerId);
        SetTimer(hWnd,
                 kRefreshTimerId,
                 g_settings.refreshIntervalMs.load(),
                 nullptr);
        RefreshMirrorWindows();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, kRefreshTimerId);
        DestroyAllMirrors();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

DWORD WINAPI WorkerThreadProc(void*) {
    if (!IsExplorerTarget()) {
        return 0;
    }

    g_hInstance = GetModuleHandleW(nullptr);
    RegisterWindowClasses();

    g_controllerWindow = CreateWindowExW(
        0,
        kControllerWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        g_hInstance,
        nullptr
    );
    if (!g_controllerWindow) {
        return 0;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_controllerWindow = nullptr;
    return 0;
}

void StartWorkerThread() {
    if (g_workerThread) {
        return;
    }

    g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0,
                                  &g_workerThreadId);
    if (!g_workerThread) {
        Wh_Log(L"CreateThread failed");
    }
}

void StopWorkerThread() {
    if (!g_workerThread) {
        return;
    }

    if (g_controllerWindow) {
        PostMessageW(g_controllerWindow, WM_CLOSE, 0, 0);
    } else if (g_workerThreadId) {
        PostThreadMessageW(g_workerThreadId, WM_QUIT, 0, 0);
    }

    WaitForSingleObject(g_workerThread, 3000);
    CloseHandle(g_workerThread);
    g_workerThread = nullptr;
    g_workerThreadId = 0;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init %s", WH_MOD_VERSION);
    g_target = DetectTargetProcess();

    if (g_target == Target::Unknown) {
        Wh_Log(L"Unsupported target process");
        return FALSE;
    }

    LoadSettings();

    if (!InitSharedFlyoutState()) {
        Wh_Log(L"Failed to initialize shared flyout state");
        return FALSE;
    }

    if (!Wh_SetFunctionHook((void*)SetWindowPos,
                            (void*)SetWindowPos_Hook,
                            (void**)&SetWindowPos_Original)) {
        Wh_Log(L"Failed to hook SetWindowPos");
        CleanupSharedFlyoutState();
        return FALSE;
    }

    if (!Wh_SetFunctionHook((void*)MoveWindow,
                            (void*)MoveWindow_Hook,
                            (void**)&MoveWindow_Original)) {
        Wh_Log(L"Failed to hook MoveWindow");
        CleanupSharedFlyoutState();
        return FALSE;
    }

    if (g_target == Target::OneDrive) {
        if (!Wh_SetFunctionHook((void*)ShowWindow,
                                (void*)ShowWindow_Hook,
                                (void**)&ShowWindow_Original)) {
            Wh_Log(L"Failed to hook ShowWindow");
            CleanupSharedFlyoutState();
            return FALSE;
        }

        if (!Wh_SetFunctionHook((void*)ShowWindowAsync,
                                (void*)ShowWindowAsync_Hook,
                                (void**)&ShowWindowAsync_Original)) {
            Wh_Log(L"Failed to hook ShowWindowAsync");
            CleanupSharedFlyoutState();
            return FALSE;
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"AfterInit");
    if (IsExplorerTarget()) {
        StartWorkerThread();
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"BeforeUninit");
    if (IsExplorerTarget()) {
        StopWorkerThread();
    }

    CleanupSharedFlyoutState();
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");
    LoadSettings();

    if (IsExplorerTarget() && g_controllerWindow) {
        PostMessageW(g_controllerWindow, kMessageApplySettings, 0, 0);
    }
}
