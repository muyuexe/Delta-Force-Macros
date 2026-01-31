#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <ShellScalingApi.h>
#include <tlhelp32.h>
#include <mmsystem.h>

#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Winmm.lib")

// AIM_SKIP: 特殊操作码。当 dwExtraInfo 为此值时，Hook 不会更新 Aim 业务账本
const ULONG_PTR AIM_SKIP = 0xACE;

// Pass 宏：放行并审计物理按键状态
#define Pass ([&](){\
    bool _isDown = false; DWORD _vk = 0; ULONG_PTR _ex = 0;\
    if (nCode == HC_ACTION) {\
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) { _isDown = true; _vk = ((KBDLLHOOKSTRUCT*)lParam)->vkCode; _ex = ((KBDLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) { _isDown = false; _vk = ((KBDLLHOOKSTRUCT*)lParam)->vkCode; _ex = ((KBDLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_LBUTTONDOWN) { _isDown = true; _vk = VK_LBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_LBUTTONUP) { _isDown = false; _vk = VK_LBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_RBUTTONDOWN) { _isDown = true; _vk = VK_RBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_RBUTTONUP) { _isDown = false; _vk = VK_RBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {\
            _isDown = (wParam == WM_XBUTTONDOWN); _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo;\
            _vk = (HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData) == 1) ? VK_XBUTTON1 : VK_XBUTTON2;\
        }\
    }\
    /* 1. g_Out 影子账本 (永远记录) */\
    if (_vk > 0 && _vk < 256) g_Out[_vk].store(_isDown, std::memory_order_relaxed);\
    \
    /* 2. Aim 业务账本 (在 g_Out 基础上加黑名单) */\
    if (_ex != AIM_SKIP) {\
        if (_vk == 'U') Aim.u.store(_isDown, std::memory_order_relaxed);\
        else if (_vk == VK_RBUTTON) Aim.r.store(_isDown, std::memory_order_relaxed);\
    }\
    return CallNextHookEx(NULL, nCode, wParam, lParam);\
}())

// Mode: 开镜模式 (Shoulder/Scope)
enum Mode : BYTE { Shoulder, Scope };

// 影子账本：记录实际发出的逻辑按下状态，不参与业务逻辑
std::atomic<bool> g_Out[256]{ false };

// Aim: 业务账本，记录脚本逻辑发出的按下状态
struct AimLedger {
	std::atomic<bool> u{ false };
	std::atomic<bool> r{ false };
};
AimLedger Aim;

// M: Mode (模式)
std::atomic<Mode> M{ Scope };
// S: Shield (屏蔽位)
std::atomic<bool> S{ false };
// Num: NuMWer (状态器数值)
std::atomic<int> Num{ 1 };
// XB1: XButton1 (鼠标侧键1)
std::atomic<bool> XB1{ false };
// XB2: XButton2 (鼠标侧键2)
std::atomic<bool> XB2{ false };
// KF: Key F (F键状态)
std::atomic<bool> KF{ false };
// KS: Key Space (空格状态)
std::atomic<bool> KS{ false };
// CT64: C-Key Timestamp 64-bit (C键计时戳)
std::atomic<ULONGLONG> CT64{ false };
// RB: Right Button (右键状态)
std::atomic<bool> RB{ false };
// MW: Mouse Wheel (滚轮信号)
std::atomic<int> MW{ 0 };
// 前台检测
std::atomic<bool> g_IsGameActive{ false };

//线程触发事件句柄
HANDLE XB2event, Fevent, SPACEevent, RBevent, MWevent;

HWND g_hNotifyWnd = NULL; // UI全局句柄

// 记录主线程 ID，用于跨线程通信
DWORD g_mainThreadId = 0;

// --- 基础动作封装函数 ---

// 封装：等待指定毫秒
static void Wait(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 修改 SendKey
static void SendKey(BYTE vk, bool down, ULONG_PTR extra = 0) {
	INPUT input = { 0 };
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = 0;
	input.ki.wScan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
	input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
	input.ki.dwExtraInfo = extra; // 将暗号塞进 Windows 输入流

	if (vk == VK_RMENU || vk == VK_RCONTROL || (vk >= 33 && vk <= 46)) {
		input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	}
	SendInput(1, &input, sizeof(INPUT));
}

// 修改 SendMouse
static void SendMouse(BYTE vk, bool down, ULONG_PTR extra = 0) {
	INPUT input = { 0 };
	input.type = INPUT_MOUSE;
	input.mi.dwExtraInfo = extra; // 将暗号塞进 Windows 输入流
	if (vk == VK_RBUTTON) {
		input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
	}
	else if (vk == VK_LBUTTON) {
		input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
	}
	SendInput(1, &input, sizeof(INPUT));
}

// 修改 Press / Release / Tap 顺延传递参数
static void Press(BYTE vk, ULONG_PTR extra = 0) {
	if (vk == VK_RBUTTON || vk == VK_LBUTTON) SendMouse(vk, true, extra);
	else SendKey(vk, true, extra);
}

static void Release(BYTE vk, ULONG_PTR extra = 0) {
	if (vk == VK_RBUTTON || vk == VK_LBUTTON) SendMouse(vk, false, extra);
	else SendKey(vk, false, extra);
}

static void Tap(BYTE vk, ULONG_PTR extra = 0) {
	Press(vk, extra);
	Wait(5);
	Release(vk, extra);
}

static void ResetAim() {
	// 检查 U 账本
	if (Aim.u.load()) {
		Release('U'); // 默认 extra=0，Release 信号会经过 Pass 宏并把 Aim.u 置为 false
	}
	// 检查 R 账本
	if (Aim.r.load()) {
		Release(VK_RBUTTON); // 同理，重置 R 状态
	}
}

// --- 进程检测(100ms) ---
static void ActiveWindowMonitor() {
	static DWORD lastPid = 0;
	static bool lastState = false; // 记录上一次的状态
	while (true) {
		bool currentResult = false;
		HWND hwnd = GetForegroundWindow();
		if (hwnd) {
			DWORD pid;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid != lastPid) {
				lastPid = pid;
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
				if (hProc) {
					WCHAR path[MAX_PATH];
					if (K32GetProcessImageFileNameW(hProc, path, MAX_PATH)) {
						currentResult = (wcsstr(path, L"DeltaForceClient-Win64-Shipping.exe") != nullptr);
					}
					CloseHandle(hProc);
				}
				g_IsGameActive.store(currentResult, std::memory_order_relaxed);
			}
			else {
				currentResult = g_IsGameActive.load(std::memory_order_relaxed);
			}
		}
		else {
			g_IsGameActive.store(false, std::memory_order_relaxed);
			currentResult = false;
			lastPid = 0;
		}

		// --- 联动点：状态从 激活 -> 非激活 的瞬间，立刻发消息隐藏 UI ---
		if (lastState && !currentResult) {
			if (g_hNotifyWnd) {
				PostMessage(g_hNotifyWnd, WM_TIMER, 1, 0); // 发送计时器消息强制隐藏
			}
		}
		lastState = currentResult;

		Wait(100); // 100ms 巡检频率
	}
}
// 2. 读接口：现在这个函数到处调用都不会卡了
static inline bool IsTargetActive() {
	return 1; // 目标程序检测开关
	return g_IsGameActive.load(std::memory_order_relaxed);
}

// --- 模式切换提示 UI ---

static LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_PAINT) {
		// --- 瞬间拦截：如果当前不在游戏前台，直接隐藏并不执行任何绘制 ---
		if (!IsTargetActive()) {
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		}

		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT rect;
		GetClientRect(hwnd, &rect);

		HBRUSH bg = CreateSolidBrush(RGB(1, 1, 1));
		FillRect(hdc, &rect, bg);
		DeleteObject(bg);

		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, RGB(200, 200, 200));

		HFONT hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei");
		SelectObject(hdc, hFont);

		std::wstring text = (M == Shoulder) ? L"肩射模式" : L"倍镜模式";
		DrawTextW(hdc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		DeleteObject(hFont);
		EndPaint(hwnd, &ps);
		return 0;
	}

	if (msg == WM_USER + 100) { // 唤醒信号
		if (IsTargetActive()) { // 只有在游戏内才允许唤醒
			InvalidateRect(hwnd, NULL, TRUE);
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			SetTimer(hwnd, 1, 400, NULL); // 0.4秒后消失
		}
		return 0;
	}

	if (msg == WM_TIMER) {
		ShowWindow(hwnd, SW_HIDE);
		KillTimer(hwnd, 1);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- 并行逻辑线程 (高速循环响应) ---

// 提示窗口线程
static void Thread_Notify_Manager() {
	WNDCLASSEXW nwc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, NotifyWndProc, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"NotifyClass", NULL };
	RegisterClassExW(&nwc);

	int sw = GetSystemMetrics(SM_CXSCREEN);
	int width = 300;
	int height = 65;

	g_hNotifyWnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		L"NotifyClass", L"Notify",
		WS_POPUP, // 初始不带 WS_VISIBLE
		(sw / 2) - (width / 2), 240,
		width, height,
		NULL, NULL, GetModuleHandle(NULL), NULL
	);

	SetLayeredWindowAttributes(g_hNotifyWnd, RGB(1, 1, 1), 0, LWA_COLORKEY);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

// 【功能 1】 侧键2循环 (保持高精度自旋)
static void Thread_XB2_Loop() {
	while (WaitForSingleObject(XB2event, INFINITE) == WAIT_OBJECT_0) {
		// 每次唤醒先看一眼黑板
		if (XB2 && IsTargetActive()) {
			BYTE k = (BYTE)('0' + Num.load());

			do {
				Tap('L');

				// 128ms 高精度自旋
				auto start_1 = std::chrono::steady_clock::now();
				while (true) {
					// 只要切屏，g_IsGameActive 会变假，这里立刻跳出
					if (!XB2 || !IsTargetActive()) goto BREAK_LOOP;

					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start_1).count() >= 128) break;
					std::this_thread::yield();
				}

				Tap(k);

				// 18ms 高精度自旋
				auto start_2 = std::chrono::steady_clock::now();
				while (true) {
					if (!XB2 || !IsTargetActive()) goto BREAK_LOOP;

					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start_2).count() >= 18) break;
					std::this_thread::yield();
				}

			} while (XB2 && IsTargetActive());

		BREAK_LOOP:
			// 只有在依然处于游戏内时才执行收尾动作（3号键等）
			if (IsTargetActive()) {
				Tap(k);
				Wait(15);
				Tap('3');
			}
		}
	}
}

// 【功能 3】 F键循环
static void Thread_F_Loop() {
	while (WaitForSingleObject(Fevent, INFINITE) == WAIT_OBJECT_0) {
		// 只要切屏，isActive 变假，这里的 while 循环会自动终止
		while (KF && !XB1 && IsTargetActive()) {
			Tap('F');
			Wait(20);
		}
	}
}

// 【功能 4】 空格循环
static void Thread_Space_Loop() {
	while (WaitForSingleObject(SPACEevent, INFINITE) == WAIT_OBJECT_0) {
		while (KS && IsTargetActive()) {
			Tap(VK_SPACE);
			Wait(20);
		}
	}
}

// 【功能 6】 鼠标滚轮逻辑
static void Thread_MW_Loop() {
	while (WaitForSingleObject(MWevent, INFINITE) == WAIT_OBJECT_0) {
		int sDelta = MW.exchange(0);

		// 只有在 isActive 且 sDelta 有效时处理
		// 注意：能走到这里的信号，一定是 Hook 拦截下来的（XB1没按下时）
		if (sDelta != 0 && IsTargetActive()) {

			if (sDelta < 0) { // 下滚切换模式
				ResetAim();
				M = (M == Shoulder) ? Scope : Shoulder;
				if (g_hNotifyWnd && IsWindow(g_hNotifyWnd)) {
					PostMessage(g_hNotifyWnd, WM_USER + 100, 0, 0);
				}
			}
			else if (sDelta > 0) { // 上滚执行逻辑
				if (!RB.load()) {
					ResetAim();
					BYTE target = (M == Shoulder) ? 'U' : VK_RBUTTON;
					Press(target);
				}
				else {
					S = true;
				}
			}
		}
	}
}

// 【功能 2/6/7】 鼠标右键复杂逻辑
static void Thread_RB_Loop() {
	while (WaitForSingleObject(RBevent, INFINITE) == WAIT_OBJECT_0) {
		if (RB.load()) {
			Press('H');
			// ... CT64 处理逻辑 ...
			ResetAim();
			if (M == Shoulder) {
				Press('U');
				// 核心修改：利用 Tap 实现瞬间点击，Hook 会自动处理 g_Out
				std::thread([]() {
					Wait(10);
					Tap(VK_RBUTTON, AIM_SKIP);//传入 AIM_SKIP。Hook 看到这个右键会放行，但不会修改 Aim 账本
					}).detach();
			}
			else {
				Press(VK_RBUTTON);
			}
		}
		else {
			Release('H');
			if (S) S = false;
			else ResetAim();
		}
	}
}

// --- Hook 回调逻辑 ---

static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
		// --- 核心修改：在 return Pass 前记录影子账本 ---
		if (k->flags & LLKHF_INJECTED) return Pass;

		bool state = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		DWORD vk = k->vkCode;

		bool isActive = IsTargetActive();

		// --- 全局同步逻辑 (无论是否在游戏内，确保状态正确) ---
		// 这里必须保持不受 isActive 影响，物理键松开，变量必须同步，后台循环才能自然熄火
		switch (vk) {
		case 'F':
			KF = state;
			SetEvent(Fevent);
			break;
		case VK_SPACE:
			KS = state;
			SetEvent(SPACEevent);
			break;
		}

		// --- 业务逻辑处理 ---
		switch (vk) {
			// 【功能 5/8/9】 Q/E 叠加 A/D
		case 'Q':
			if (state) {
				if (isActive) Press('A'); // 按下仅在游戏内生效
			}
			else {
				Release('A'); // 松开不受 isActive 影响，确保复位
			}
			break;
		case 'E':
			if (state) {
				if (isActive) Press('D'); // 按下仅在游戏内生效
			}
			else {
				Release('D'); // 松开不受 isActive 影响，确保复位
			}
			break;

			// 【功能 1】 武器槽位
		case '1': if (state && isActive) Num = 1; break;
		case '2': if (state && isActive) Num = 2; break;
		case '4': if (state && isActive) Num = 4; break;

			// 【功能 3/4】 F 键与空格拦截
		case 'F':
			if (isActive) return XB1 ? Pass : 1; // 仅在前台时拦截
			break;
		case VK_SPACE:
			if (isActive) return 1; // 仅在前台时拦截
			break;


			// --- 功能 5/8/9：侧键 1 (XB1) 组合映射 (带身份互换逻辑) ---

		case VK_CAPITAL: { // 物理 Caps 键
			static bool pressedN = false;
			static bool pressedCtrl = false; // 对应原本的 isMappingCtrl，记录本次 Caps 物理按下是否转换成了模拟 Ctrl
			if (state) {
				if (isActive) {
					if (XB1) {
						Press('N');
						pressedN = true;
						return 1;
					}
					// 【新增】放行逻辑：Caps 变 Ctrl
					else {
						Press(VK_LCONTROL);
						pressedCtrl = true;
						return 1;
					}// 必须拦截原始 Caps，否则会触发大写锁定
				}
				else return Pass; // 非前台直接放行原始 Caps
			}
			else {
				// 抬起逻辑：只要标志位为真，说明之前在游戏内触发了映射，必须强制释放
				if (pressedN) {
					Release('N');
					pressedN = false;
					return 1;
				}
				if (pressedCtrl) {
					Release(VK_LCONTROL);
					pressedCtrl = false;
					return 1;
				}
				else return Pass;
			}
		}

		case VK_LCONTROL: { // 物理 Ctrl 键
			static bool pressedM = false;
			static bool pressedCaps = false; // 记录本次 Ctrl 物理按下是否转换成了模拟 Caps
			if (state) {
				if (isActive) {
					if (XB1) {
						Press('M');
						pressedM = true;
						return 1;
					}
					// 【新增】放行逻辑：Ctrl 变 Caps
					else {
						Press(VK_CAPITAL);
						pressedCaps = true;
						return 1;
					}
				}
				else return Pass; // 非前台直接放行原始 Ctrl
			}
			else {
				if (pressedM) {
					Release('M');
					pressedM = false;
					return 1;
				}
				if (pressedCaps) {
					Release(VK_CAPITAL);
					pressedCaps = false;
					return 1;
				}
				else return Pass;
			}
		}

		case 'X': { // X -> . (逻辑保持不变，不满足条件则放行原键)
			static bool pressedDot = false;
			if (state) {
				if (XB1 && isActive) {
					Press(VK_OEM_PERIOD);
					pressedDot = true;
					return 1;
				}
				else return Pass;
			}
			else {
				if (pressedDot) {
					Release(VK_OEM_PERIOD);
					pressedDot = false;
					return 1;
				}
				else return Pass;
			}
		}

		case VK_TAB: { // Tab -> ; (逻辑保持不变)
			static bool pressedSemicolon = false;
			if (state) {
				if (XB1 && isActive) {
					Press(VK_OEM_1);
					pressedSemicolon = true;
					return 1;
				}
				else return Pass;
			}
			else {
				if (pressedSemicolon) {
					Release(VK_OEM_1);
					pressedSemicolon = false;
					return 1;
				}
				else return Pass;
			}
		}

				   // 【功能 7】 战术 C 键
		case 'C':
			if (isActive && !XB1) {
				if (state) CT64 = GetTickCount64();
				return 1;
			}
			break;
		}
	}
	return Pass;
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lParam;

		// --- 核心修改：记录鼠标逻辑输出状态 ---
		if (m->flags & LLMHF_INJECTED) return Pass;

		bool isActive = IsTargetActive();

		// --- 鼠标按键处理 ---
		switch (wParam) {

			// --- 侧键 1 & 2 处理 (XB1 组合键开关 / XB2 连招触发) ---
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		{
			bool state = (wParam == WM_XBUTTONDOWN);
			int xNum = HIWORD(m->mouseData);

			if (xNum == 1) {
				// 侧键 1 处理
				if (state) {
					if (isActive) {
						XB1 = true;
						return 1; // 在游戏内按下：拦截物理信号，仅触发内部开关
					}
				}
				else {
					XB1 = false; // 松开：同步状态
					if (isActive) return 1; // 在游戏内松开：拦截物理信号
				}
				return Pass; // 非游戏内，放行原键位逻辑
			}

			if (xNum == 2) {
				// 侧键 2 处理
				if (state) {
					if (isActive) {
						XB2 = true;
						SetEvent(XB2event); // 唤醒连招线程
						return 1; // 在游戏内按下：拦截
					}
				}
				else {
					XB2 = false; // 松开：让线程通过 while 判断熄火
					SetEvent(XB2event); // 再次触发事件确保线程从 Wait 中醒来检查 XB2 状态
					if (isActive) return 1; // 在游戏内松开：拦截
				}
				return Pass;
			}
			break;
		}

		// --- 右键处理：同步状态并通知 Thread_RB_Loop ---
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		{
			bool state = (wParam == WM_RBUTTONDOWN);

			// 只有在 isActive 时才允许“按下”逻辑生效
			// 但“松开”逻辑必须始终同步，以确保 Thread_RB_Loop 能够执行 Release('H') 等收尾
			if (state) {
				if (isActive) {
					RB = true;
					SetEvent(RBevent);
					return 1; // 拦截物理按下
				}
			}
			else {
				RB = false;
				SetEvent(RBevent);
				// 如果之前按下被拦截了（RB为真时切走的），松开时也需要拦截或处理
				// 为了优雅，如果是在游戏内松开，我们返回 1 保持对称
				if (isActive) return 1;
			}
			break;
		}

		// --- 滚轮处理 ---
		case WM_MOUSEWHEEL:
		{
			if (isActive) {
				if (XB1.load()) {
					return Pass;
				}
				else {
					short rDelta = (short)HIWORD(m->mouseData);
					MW = (int)rDelta;
					SetEvent(MWevent);
					return 1;
				}
			}
			break;
		}
		}
	}
	return Pass;
}

// --- 【功能 10】 准星 UI 绘制 (对游戏帧率影响极小) ---

static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		// 使用第二段的绘图设置
		HBRUSH bgBrush = CreateSolidBrush(RGB(255, 0, 254)); // 透明色背景
		HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
		HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));

		RECT clientRect;
		GetClientRect(hwnd, &clientRect);
		FillRect(hdc, &clientRect, bgBrush);

		// 统一绘制函数：绘制带黑边的绿色条
		auto DrawPart = [&](int x, int y, int w, int h) {
			RECT r = { x, y, x + w, y + h };
			FillRect(hdc, &r, blackBrush);
			if (w >= 3 && h >= 3) {
				RECT core = { x + 1, y + 1, x + w - 1, y + h - 1 };
				FillRect(hdc, &core, greenBrush);
			}
			};

		// 绘制纯黑标记线函数
		auto DrawMark = [&](int x, int y, int w, int h) {
			RECT r = { x, y, x + w, y + h };
			FillRect(hdc, &r, blackBrush);
			};

		int midX = clientRect.right / 2;
		int midY = clientRect.bottom / 2;

		// 参数设置
		int thick = 3;
		int gap = 5;
		int len = 15;
		int markDist = 15; // 距离中心点的距离

		// 1. 中心点 (3x3)
		// 仅绘制准星核心，减少 GDI 刷新压力
		DrawPart(midX - 1, midY - 1, 3, 3);

		// 2. 四方向准星条
		DrawPart(midX - 1, midY - 1 - gap - len, thick, len); // 上
		DrawPart(midX - 1, midY + 2 + gap, thick, len); // 下
		DrawPart(midX - 1 - gap - len, midY - 1, len, thick); // 左
		DrawPart(midX + 2 + gap, midY - 1, len, thick); // 右

		// --- 新增：距离中心 15px 的 3x1 像素黑线 ---

		// 上方标记 (横线: 宽3高1)
		DrawMark(midX - 1, midY - 1 - markDist, 3, 1);

		// 下方标记 (横线: 宽3高1)
		DrawMark(midX - 1, midY + 1 + markDist, 3, 1);

		// 左侧标记 (竖线: 宽1高3)
		DrawMark(midX - 1 - markDist, midY - 1, 1, 3);

		// 右侧标记 (竖线: 宽1高3)
		DrawMark(midX + 1 + markDist, midY - 1, 1, 3);

		DeleteObject(bgBrush);
		DeleteObject(blackBrush);
		DeleteObject(greenBrush);
		EndPaint(hwnd, &ps);
		return 0;
	}

	// 自动显隐判定
	if (msg == WM_TIMER) {
		// 注意：需确保工程中已定义 IsTargetActive() 函数
		bool active = IsTargetActive();
		if (active && !IsWindowVisible(hwnd)) {
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		}
		else if (!active && IsWindowVisible(hwnd)) {
			ShowWindow(hwnd, SW_HIDE);
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void CreateCrosshair() {
	WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, CrosshairWndProc, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"CrosshairClass", NULL };
	RegisterClassExW(&wc);

	int sw = GetSystemMetrics(SM_CXSCREEN);
	int sh = GetSystemMetrics(SM_CYSCREEN);

	// --- 修改点：将 40 开为 100 ---
	// 线条 34px + 间距 6px = 40px，两侧总计 80px+，所以窗口必须大于 80。
	int windowSize = 100;

	// --- 修改点：添加 WS_EX_NOACTIVATE 防止窗口创建时抢夺焦点导致鼠标位置重置 ---
	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		L"CrosshairClass", L"Overlay",
		WS_POPUP, // 初始不带 WS_VISIBLE，由 Timer 控制显隐
		(sw / 2) - (windowSize / 2), (sh / 2) - (windowSize / 2),
		windowSize, windowSize,
		NULL, NULL, GetModuleHandle(NULL), NULL
	);

	SetLayeredWindowAttributes(hwnd, RGB(255, 0, 254), 0, LWA_COLORKEY);

	// 设置定时器：用于自动显隐判定
	SetTimer(hwnd, 1, 100, NULL);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

//检测三角洲是否退出，若退出则自动关闭脚本
static void MonitorAndExit(std::wstring processName) {
	HANDLE hProcess = NULL;
	DWORD pid = 0;

	// --- 阶段 1：低频率探测游戏是否启动 ---
	// 游戏没开时，没必要每秒扫描进程列表，2-3秒一次足以
	while (true) {
		// 性能优化点 1：优先使用 FindWindow 快速定位，这比遍历进程列表快得多
		HWND hwnd = FindWindowW(L"UnrealWindow", NULL); // 三角洲是虚幻引擎，通常类名为此
		if (hwnd) {
			GetWindowThreadProcessId(hwnd, &pid);
			// 验证 PID 是否属于目标 EXE
			hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
			if (hProcess) {
				WCHAR path[MAX_PATH];
				DWORD size = MAX_PATH;
				if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
					if (std::wstring(path).find(processName) != std::wstring::npos) {
						break; // 确认匹配，进入监控阶段
					}
				}
				CloseHandle(hProcess);
				hProcess = NULL;
			}
		}
		Wait(2000); // 没找到游戏时，每 2 秒检测一次
	}

	// --- 阶段 2：零占用监控 ---
	// 性能优化点 2：使用内核对象同步机制
	// WaitForSingleObject 会让本线程进入“等待挂起”状态，完全不占用 CPU 周期
	if (hProcess) {
		WaitForSingleObject(hProcess, INFINITE);
		CloseHandle(hProcess);
	}

	// --- 阶段 3：精确通知主线程退出 ---
	// 性能优化点 3：通过消息队列通知，而非暴力 ExitProcess，确保主线程能回收 Hook
	if (g_mainThreadId != 0) {
		PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
	}
}

// --- 程序退出冻结子程序 ---
static void FreezeAllSubThreads() {
	DWORD dwPID = GetCurrentProcessId();
	DWORD dwMainTID = GetCurrentThreadId();

	// 创建线程快照
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return;

	THREADENTRY32 te = { sizeof(THREADENTRY32) };

	if (Thread32First(hSnap, &te)) {
		do {
			// 只要是本进程的线程，且不是当前主线程，全部定身
			if (te.th32OwnerProcessID == dwPID && te.th32ThreadID != dwMainTID) {
				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (hThread) {
					SuspendThread(hThread); // 关键：物理断电
					CloseHandle(hThread);
				}
			}
		} while (Thread32Next(hSnap, &te));
	}
	CloseHandle(hSnap);
}

static void Alignkeys() {
	const BYTE keys[] = {
		'U', 'H', 'A', 'D', 'N', 'M', VK_OEM_PERIOD, VK_OEM_1,
		'1', '2', '3', '4', 'L', 'F', VK_SPACE,
		VK_LCONTROL, VK_CAPITAL, VK_RBUTTON
	};

	for (BYTE vk : keys) {
		bool phy = (GetAsyncKeyState(vk) & 0x8000) != 0;

		// 如果逻辑与物理不一致
		if (g_Out[vk] != phy) {
			// 以物理状态(phy)为准进行补发
			if (vk >= 0x01 && vk <= 0x06) SendMouse(vk, phy);
			else SendKey(vk, phy);

			// 状态对齐
			g_Out[vk] = phy;
		}
	}
}

// --- 程序入口 ---

int WINAPI WinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPSTR lpCmdLine,
	_In_ int nCmdShow
) {
	// 初始化门铃：自动重置模式
	MWevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	Fevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	SPACEevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	RBevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	XB2event = CreateEvent(NULL, FALSE, FALSE, NULL);

	timeBeginPeriod(1);// 提升定时器精度至1ms

	// 设置 DPI 意识，确保在不同分辨率缩放下的准星位置准确
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	g_mainThreadId = GetCurrentThreadId();

	// 启动线程
	std::thread(Thread_Notify_Manager).detach();
	std::thread(Thread_XB2_Loop).detach();
	std::thread(Thread_F_Loop).detach();
	std::thread(Thread_Space_Loop).detach();
	std::thread(Thread_RB_Loop).detach();
	std::thread(Thread_MW_Loop).detach();
	std::thread(ActiveWindowMonitor).detach();
	std::thread(CreateCrosshair).detach();
	std::thread(MonitorAndExit, L"DeltaForceClient-Win64-Shipping.exe").detach();

	// 安装 Hook：确保鼠标和键盘全局监控
	HHOOK kHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
	HHOOK mHook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, NULL, 0);

	// 消息循环：这是程序不退出的关键，同时也负责处理 Hook 消息
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// 程序退出前的清理工作
	FreezeAllSubThreads();
	Alignkeys();
	UnhookWindowsHookEx(kHook); UnhookWindowsHookEx(mHook);

	timeEndPeriod(1); // 记得恢复系统定时器精度

	ExitProcess(0);
}