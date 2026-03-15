#include "pch.h"
#include "BaseFn.h"

// AIM_SKIP： 特殊操作码。当 dwExtraInfo 为此值时，Hook 不会更新 Aim 业务账本
const ULONG_PTR AIM_SKIP = 0xACE;

// Pass 宏：放行并审计物理按键状态
#define Pass ([&](){\
    bool _isDown = false; DWORD _vk = 0; ULONG_PTR _ex = 0;\
    if (nCode == HC_ACTION) {\
        /* 键盘消息审计 */\
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) { _isDown = true; _vk = ((KBDLLHOOKSTRUCT*)lParam)->vkCode; _ex = ((KBDLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) { _isDown = false; _vk = ((KBDLLHOOKSTRUCT*)lParam)->vkCode; _ex = ((KBDLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        /* 鼠标左键审计 */\
        else if (wParam == WM_LBUTTONDOWN) { _isDown = true; _vk = VK_LBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_LBUTTONUP) { _isDown = false; _vk = VK_LBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        /* 鼠标右键审计 */\
        else if (wParam == WM_RBUTTONDOWN) { _isDown = true; _vk = VK_RBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_RBUTTONUP) { _isDown = false; _vk = VK_RBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        /* 鼠标中键审计 (新增) */\
        else if (wParam == WM_MBUTTONDOWN) { _isDown = true; _vk = VK_MBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        else if (wParam == WM_MBUTTONUP) { _isDown = false; _vk = VK_MBUTTON; _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo; }\
        /* 鼠标侧键审计 */\
        else if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {\
            _isDown = (wParam == WM_XBUTTONDOWN); _ex = ((MSLLHOOKSTRUCT*)lParam)->dwExtraInfo;\
            _vk = (HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData) == 1) ? VK_XBUTTON1 : VK_XBUTTON2;\
        }\
    }\
    \
    /* 1. g_Out 影子账本：记录所有物理/逻辑发出的按键状态，用于程序退出时对齐 */\
    if (_vk > 0 && _vk < 256) g_Out[_vk].store(_isDown, std::memory_order_relaxed);\
    \
    /* 2. Aim 业务账本：只有当不带 AIM_SKIP 暗号时，才更新业务逻辑需要的状态 */\
    if (_ex != AIM_SKIP) {\
        if (_vk == 'U') Aim.u.store(_isDown, std::memory_order_relaxed);\
        else if (_vk == VK_RBUTTON) Aim.r.store(_isDown, std::memory_order_relaxed);\
    }\
    return CallNextHookEx(NULL, nCode, wParam, lParam);\
}())

// Mode： 开镜模式 (Shoulder/Scope)
enum Mode : BYTE { Shoulder, Scope };

// 影子账本：记录实际发出的逻辑按下状态，不参与业务逻辑
std::atomic<bool> g_Out[256]{ false };

AimLedger Aim;

// M： Mode (模式)
std::atomic<Mode> M{ Scope };
// S： Shield (屏蔽位)
std::atomic<bool> S{ false };
// Num： NuMWer (状态器数值)
std::atomic<int> Num{ 1 };
// XB1： XButton1 (鼠标侧键1)
std::atomic<bool> XB1{ false };
// XB2： XButton2 (鼠标侧键2)
std::atomic<bool> XB2{ false };
// KF： Key F (F键状态)
std::atomic<bool> KF{ false };
// KS： Key Space (空格状态)
std::atomic<bool> KS{ false };
// XB1_SPACE： XButton1 + Space (侧键1+空格组合状态)
std::atomic<bool> XB1_SPACE{ false };
// 记录C按下时刻的时间戳
std::atomic<ULONGLONG> CpressTime{ 0 };
// RB： Right Button (右键状态)
std::atomic<bool> RB{ false };
// MW： Mouse Wheel (滚轮信号)
std::atomic<int> MW{ 0 };
// 前台检测
std::atomic<bool> g_IsGameActive{ false };

//线程触发事件句柄
HANDLE XB2event, Fevent, SPACEevent, RBevent, MWevent;

HWND g_hNotifyWnd = NULL; // UI全局句柄

// 记录主线程 ID，用于跨线程通信
DWORD g_mainThreadId = 0;

//1.4.4更改
HANDLE QEevent, SpaceExitEvent;
std::atomic<bool> KQ{ false };
std::atomic<bool> KE{ false };
std::atomic<int> SpaceLock{ 0 }; // 0:空闲, 1:运行中, 2:待终止


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
	//return 1; // 目标程序检测开关
	return g_IsGameActive.load(std::memory_order_relaxed);
}


// --- 消息提示 UI ---

static LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// 关键：使用静态变量保存当前要显示的文字内容
	static std::wstring currentText = L"";

	if (msg == WM_PAINT) {
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

		// 逻辑：如果是爆闪终止，可以使用红色提示，否则使用灰色
		if (currentText == L"爆闪终止") {
			SetTextColor(hdc, RGB(255, 80, 80)); // 醒目的淡红色
		}
		else {
			SetTextColor(hdc, RGB(200, 200, 200)); // 默认灰色
		}

		HFONT hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei");
		SelectObject(hdc, hFont);

		DrawTextW(hdc, currentText.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		DeleteObject(hFont);
		EndPaint(hwnd, &ps);
		return 0;
	}

	// 处理“模式切换”消息
	if (msg == WM_USER + 100) {
		currentText = (M == Shoulder) ? L"肩射模式" : L"倍镜模式";
		goto REFRESH_UI;
	}

	// 新增：处理“爆闪终止”消息
	if (msg == WM_USER + 101) {
		currentText = L"爆闪终止";
		goto REFRESH_UI;
	}

	if (msg == WM_TIMER) {
		ShowWindow(hwnd, SW_HIDE);
		KillTimer(hwnd, 1);
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);

REFRESH_UI:
	if (IsTargetActive()) {
		// 根据文字内容决定透明度 (0-255)
		// 255 是不透明，180 约等于 70% 透明度
		BYTE alpha = (currentText == L"爆闪终止") ? 180 : 255;
		SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), alpha, LWA_COLORKEY | LWA_ALPHA);

		InvalidateRect(hwnd, NULL, TRUE);
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		SetTimer(hwnd, 1, 400, NULL);
	}
	return 0;
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

	SetLayeredWindowAttributes(g_hNotifyWnd, RGB(1, 1, 1), 255, LWA_COLORKEY | LWA_ALPHA);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

// 【功能 1】 侧键2循环 (保持高精度自旋)
static void Thread_XB2() {
	while (WaitForSingleObject(XB2event, INFINITE) == WAIT_OBJECT_0) {
		// 每次唤醒先看一眼黑板
		if (XB2 && IsTargetActive()) {
			BYTE k = (BYTE)('0' + Num.load());

			do { // 循环长度约为192ms
				Tap('L');

				// 140ms 高精度自旋
				auto start_1 = std::chrono::steady_clock::now();
				while (true) {
					// 只要切屏，g_IsGameActive 会变假，这里立刻跳出
					if (!XB2 || !IsTargetActive()) goto BREAK_LOOP;

					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start_1).count() >= 140) break;
					std::this_thread::yield();
				}

				Tap(k);

				// 40ms 高精度自旋
				auto start_2 = std::chrono::steady_clock::now();
				while (true) {
					if (!XB2 || !IsTargetActive()) goto BREAK_LOOP;

					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start_2).count() >= 40) break;
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
static void Thread_F() {
	while (WaitForSingleObject(Fevent, INFINITE) == WAIT_OBJECT_0) {
		bool isLongPress = true;

		// A. 模拟默认输出：由于物理信号被 Hook 拦截，此处手动补发 Press
		Press('F');
		auto start_timestamp = std::chrono::steady_clock::now();

		while (KF) {
			// 检查时间：是否已经达到 200ms
			auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start_timestamp).count() >= 200) {
				break; // 达到 200ms，保持 isLongPress 为 true 并退出计时循环
			}
			Wait(15);
		}

		// 如果是因为 KF 变为 false（用户松开）跳出的循环
		if (!KF) isLongPress = false;

		// B. 终止当前的 F 状态：无论是松开还是准备转入连击，都需先 Release
		Release('F');

		// C. 超出 200ms 则启动连击
		if (isLongPress) {
			while (KF && IsTargetActive()) {
				Tap('F');
				Wait(15);
			}
		}
	}
}

// 【功能 4】 空格循环
static void Thread_Space() {
	while (WaitForSingleObject(SPACEevent, INFINITE) == WAIT_OBJECT_0) {
		SpaceLock.store(1);

		while (KS && IsTargetActive()) {
			if (SpaceLock.load() == 2) break; // 收到打断请求

			Tap('G');
			// 此处按需保留 Wait(15);
		}

		SpaceLock.store(0);
		SetEvent(SpaceExitEvent); // 【关键】发送握手确认信号
	}
}

// 【功能 6/7】 鼠标滚轮逻辑
static void Thread_MW() {
	while (WaitForSingleObject(MWevent, INFINITE) == WAIT_OBJECT_0) {
		int sDelta = MW.exchange(0);
		if (sDelta != 0 && IsTargetActive()) {

			// --- 情况 A：按住侧键 1 时，直接放行滚动 ---
			if (XB1.load(std::memory_order_acquire)) {
				// 形式：Roll(参数, 暗号)
				Roll(sDelta);

				// 如果是下滚，额外触发一次 Tap B
				if (sDelta < 0) {
					Tap('B');
				}
			}
			// --- 情况 B：普通状态 (模式切换逻辑保持不变) ---
			else {
				if (sDelta < 0) { // 下滚
					ResetAim();
					M = (M == Shoulder) ? Scope : Shoulder;
					if (g_hNotifyWnd) PostMessage(g_hNotifyWnd, WM_USER + 100, 0, 0);
				}
				else if (sDelta > 0) { // 上滚
					if (!RB.load()) {
						BYTE target = (M == Shoulder) ? 'U' : VK_RBUTTON;
						g_Out[target] ? Release(target) : Press(target);
					}
					else { S = true; }
				}
			}
		}
	}
}

// 【功能 2/6/7/5】 右键逻辑
static void Thread_RB() {
	while (WaitForSingleObject(RBevent, INFINITE) == WAIT_OBJECT_0) {

		if (RB.load()) {
			// --- 按下逻辑 ---
			// 仅在目标窗口激活时才执行核心操作
			if (IsTargetActive()) {
				// --- 逻辑：检查通行证 ---
				ULONGLONG lastC = CpressTime.load(std::memory_order_relaxed);
				if (lastC > 0) {
					if (CpressTime.compare_exchange_strong(lastC, 0)) {
						Tap(VK_OEM_COMMA);
					}
				}

				// --- 核心右键动作 ---
				Press('H');
				ResetAim();
				if (M == Shoulder) {
					Press('U');
					std::thread([]() { Wait(10); Tap(VK_RBUTTON, AIM_SKIP); }).detach();
				}
				else {
					Press(VK_RBUTTON);
				}
			}
		}
		else {
			// --- 松开逻辑 ---
			// 无视窗口状态，只要 RB 为 false（物理按键松开），必须执行清理
			Release('H');
			if (S) S = false; else ResetAim();
		}
	}
}

// 【功能 ？】 Q/E逻辑
static void Thread_QE() {

	const int X_MS = 265; // A/D 连携的最大持续时间（毫秒）
	static long long QPushTime = 0;  // Q 按下的起始时间戳
	static long long EPushTime = 0;  // E 按下的起始时间戳
	static bool isALocked = false;    // 逻辑 A 是否处于模拟按下状态
	static bool isDLocked = false;    // 逻辑 D 是否处于模拟按下状态

	while (true) {
		// 动态计算等待时间：若有键按下，10ms 轮询一次；否则无限等待唤醒
		DWORD waitTime = (KQ || KE) ? 10 : INFINITE;
		WaitForSingleObject(QEevent, waitTime);

		long long now = (long long)GetTickCount64();

		// --- Q 及其连带 A 的独立逻辑 ---
		if (KQ.load()) {
			if (QPushTime == 0) {
				// 按下瞬间：记录时间并同步按下 A 和 Q
				QPushTime = now;
				isALocked = true;
				Press('A');
				Press('Q');
			}
			else if (isALocked && (now - QPushTime >= X_MS)) {
				// 超时限制：仅松开连带键 A
				Release('A');
				isALocked = false;
			}
			// 注意：只要物理键 KQ 为 true，此处不需要反复执行 Press('Q')
		}
		else {
			// 物理键已松开：清理所有相关状态
			if (isALocked) { Release('A'); isALocked = false; }
			if (QPushTime != 0) { Release('Q'); QPushTime = 0; }
		}

		// --- E 及其连带 D 的独立逻辑 ---
		if (KE.load()) {
			if (EPushTime == 0) {
				EPushTime = now;
				isDLocked = true;
				Press('D');
				Press('E');
			}
			else if (isDLocked && (now - EPushTime >= X_MS)) {
				Release('D');
				isDLocked = false;
			}
		}
		else {
			if (isDLocked) { Release('D'); isDLocked = false; }
			if (EPushTime != 0) { Release('E'); EPushTime = 0; }
		}

		// --- 原有空格打断逻辑 ---
		if ((KQ || KE) && SpaceLock.load() == 2) {
			WaitForSingleObject(SpaceExitEvent, 100);
		}
	}
}


// --- Hook 回调逻辑 ---

static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION) return Pass;

	KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
	// --- 核心修改：在 return Pass 前记录影子账本 ---
	if (k->flags & LLKHF_INJECTED) return Pass;

	bool state = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
	DWORD vk = k->vkCode;
	bool isActive = IsTargetActive();

	// --- 全局同步逻辑 (无论是否在游戏内，确保状态正确) ---

	// --- 业务逻辑处理 ---
	switch (vk) {
		// 【功能8： Q/E 叠加 A/D 且打断逻辑】
	case 'Q':
	case 'E': {
		bool isQ = (vk == 'Q');
		if (state) { // KeyDown (物理按下)
			if (isActive) {
				if (isQ) {
					if (KQ.load()) return 1; // 屏蔽系统连发
					KQ = true;
				}
				else {
					if (KE.load()) return 1;
					KE = true;
				}

				// --- 打断握手逻辑 ---
				int expected = 1;
				if (!SpaceLock.compare_exchange_strong(expected, 2)) {
					SetEvent(SpaceExitEvent);
				}

				SetEvent(QEevent); // 唤醒轮询线程
				return 1;
			}
		}
		else { // KeyUp (物理松开)
			if (isQ) KQ = false; else KE = false;

			SetEvent(QEevent); // 唤醒线程处理清理逻辑
			return 1;
		}
		break;
	}

			// 【功能1：记录武器槽位】
	case '1': case '2': case '4':
		if (state && isActive) Num = (vk == '1' ? 1 : (vk == '2' ? 2 : 4));
		break;

		// 【功能4：空格逻辑】
	case VK_SPACE:
		if (state) { // 按下状态
			if (isActive) {
				// --- 业务逻辑：XB1 映射 ---
				if (XB1) {
					Press('C');
					XB1_SPACE = true;
				}
				// --- 业务逻辑：唤醒线程 ---
				if (!KS) {
					KS = true;
					ResetEvent(SpaceExitEvent);
					SetEvent(SPACEevent);
				}
			}
		}
		else { // 抬起状态
			KS = false;
			if (XB1_SPACE) {
				XB1_SPACE = false;
				Release('C');
			}
		}
		break;

		// 【功能3】
	case 'F': {
		static bool fHooked = false; // 状态锁：用于屏蔽 Win 自动重复输入
		if (state) {
			// 瞬间判定：仅在按下的第一帧且游戏处于激活状态时进行逻辑分流
			if (!fHooked && isActive) {
				if (!XB1) {
					fHooked = true;
					KF = true;
					SetEvent(Fevent);
				}
			}
			// 如果处于 Hook 锁定状态，拦截后续所有物理按下消息（包括 Auto-Repeat）
			if (fHooked) return 1;
		}
		else {
			// 释放判定
			if (fHooked) {
				fHooked = false;
				KF = false; // 通知线程停止
				return 1;
			}
		}
		break; // XB1 模式或非激活状态，放行物理信号
	}

		// 【功能8：XB1切换映射&Lshift和Lctrl交换映射】

		// Lshift 映射 (Normal -> Ctrl)
	case VK_LSHIFT: {
		static bool shiftHooked = false;
		if (state) {
			// 游戏激活且不是 XB1 模式（假设 Normal 模式下交换）
			if (!shiftHooked && isActive) {
				Press(VK_LCONTROL);
				shiftHooked = true;
			}
			if (shiftHooked) return 1;
		}
		else {
			if (shiftHooked) {
				Release(VK_LCONTROL);
				shiftHooked = false;
				return 1;
			}
		}
		break;
	}

				  // LControl 映射 (XB1 -> M | Normal -> Lshift)
	case VK_LCONTROL: {
		static bool ctrlToM = false;
		static bool ctrlToShift = false;

		if (state) {
			if (!ctrlToM && !ctrlToShift && isActive) {
				if (XB1) {
					Press('M');
					ctrlToM = true;
				}
				else {
					Press(VK_LSHIFT);
					ctrlToShift = true;
				}
			}
			if (ctrlToM || ctrlToShift) return 1;
		}
		else {
			if (ctrlToM) {
				Release('M');
				ctrlToM = false;
				return 1;
			}
			if (ctrlToShift) {
				Release(VK_LSHIFT);
				ctrlToShift = false;
				return 1;
			}
		}
		break;
	}

					// CapsLock 映射 (XB1 -> N)
	case VK_CAPITAL: {
		static bool capsHooked = false;
		if (state) {
			if (!capsHooked && isActive && XB1) {
				Press('N');
				capsHooked = true;
			}
			if (capsHooked) return 1;
		}
		else {
			if (capsHooked) {
				Release('N');
				capsHooked = false;
				return 1;
			}
		}
		break;
	}

				   // X 键映射 (XB1 -> 句号 .)
	case 'X': {
		static bool xHooked = false; // 记录本次按下是否被拦截

		if (state) {
			// 只有在没被接管、在游戏内、且满足 XB1 模式时才开始拦截
			if (!xHooked && isActive && XB1) {
				Press(VK_OEM_PERIOD);
				xHooked = true;
			}
			if (xHooked) return 1; // 拦截所有重复的 Down 信号
		}
		else {
			if (xHooked) {
				Release(VK_OEM_PERIOD);
				xHooked = false; // 必须先重置状态
				return 1; // 拦截松开信号
			}
		}
		break;
	}

			// Tab 键映射 (XB1 -> 分号 ;)
	case VK_TAB: {
		static bool tabHooked = false;
		if (state) {
			if (!tabHooked && isActive && XB1) {
				Press(VK_OEM_1);
				tabHooked = true;
			}
			if (tabHooked) return 1;
		}
		else {
			if (tabHooked) {
				Release(VK_OEM_1);
				tabHooked = false;
				return 1;
			}
		}
		break;
	}


			// 【功能5：战术 C 键】
	// 【功能5：战术 C 键】
	case 'C': {
		// 静态变量：用于防止按键长按触发的系统自动重复（Key-Repeat）
		static bool C_input = false;

		if (state) { // KeyDown
			// 1. 状态锁：仅在 isActive 开启且该按键未被锁定（按下中）时触发逻辑
			// 移除了 !XB1 判断，确保任何模式下逻辑都能触发
			if (isActive && !C_input) {
				C_input = true;

				// --- 核心功能逻辑保持不变 ---
				if (RB.load(std::memory_order_relaxed)) {
					// --- 连携模式 ---
					CpressTime.store(0);
					Tap(VK_OEM_COMMA);
				}
				else {
					// --- 通行证/计时模式 ---
					ULONGLONG oldTime = CpressTime.exchange(0);

					if (oldTime > 0) {
						// 情况：重复按下 C，销毁当前计时
						if (g_hNotifyWnd && IsTargetActive()) {
							PostMessage(g_hNotifyWnd, WM_USER + 101, 0, 0);
						}
					}
					else {
						// 情况：开启新计时
						ULONGLONG startTime = GetTickCount64();
						CpressTime.store(startTime);

						std::thread([startTime]() {
							Wait(4000); // 4秒寿命
							ULONGLONG expected = startTime;
							if (CpressTime.compare_exchange_strong(expected, 0)) {
								if (g_hNotifyWnd && IsTargetActive()) {
									PostMessage(g_hNotifyWnd, WM_USER + 101, 0, 0);
								}
							}
							}).detach();
					}
				}
				// 【关键点】：此处不再 return 1。
				// 逻辑执行完毕后，程序会流向最后的 break，从而将 C 键消息放回给系统。
			}
		}
		else { // KeyUp
			C_input = false; // 释放状态锁
		}
		// 【功能实现】：所有的路径最终都会执行到这里，放行 'C' 键
		break;
	}

	}

	if (vk >= '1' && vk <= '5') {
		ResetAim();
	}

	return Pass;
}

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION) return Pass;

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

		static bool x1Hooked = false;
		static bool x2Hooked = false;

		if (xNum == 1) {
			// 侧键 1 处理
			if (state) {
				if (isActive) {
					XB1 = true;
					x1Hooked = true;
				}
			}
			else {
				XB1 = false; // 松开：同步状态
				if (x1Hooked) { x1Hooked = false; return 1; }
			}

			if (x1Hooked) return 1; // 【功能8：在游戏内：拦截物理信号（无论按下还是松开）】
			break; // 非游戏内，跳出 switch 执行最后的 Pass
		}

		if (xNum == 2) {
			// 侧键 2 处理
			if (state) {
				if (isActive) {
					XB2 = true;
					SetEvent(XB2event); // 唤醒连招线程
					x2Hooked = true;
				}
			}
			else {
				XB2 = false; // 松开：让线程通过 while 判断熄火
				SetEvent(XB2event); // 再次触发事件确保线程从 Wait 中醒来检查 XB2 状态
				if (x2Hooked) { x2Hooked = false; return 1; }
			}

			if (x2Hooked) return 1; // 【功能8：在游戏内：拦截物理信号】
			break;
		}
		break;
	}

	// --- 右键处理：同步状态并通知 Thread_RB_Loop ---
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	{
		bool state = (wParam == WM_RBUTTONDOWN);
		static bool rbHooked = false;

		// 只有在 isActive 时才允许“按下”逻辑生效
		// 但“松开”逻辑必须始终同步，以确保 Thread_RB_Loop 能够执行 Release('H') 等收尾
		if (state) {
			if (isActive) {
				RB = true;
				SetEvent(RBevent);
				rbHooked = true;
				return 1; // 拦截物理按下
			}
		}
		else {
			RB = false;
			SetEvent(RBevent);
			if (rbHooked) {
				rbHooked = false;
				return 1; // 在游戏内松开：拦截
			}
		}
		break;
	}

	// --- 滚轮处理 ---
	case WM_MOUSEWHEEL:
	{
		if (isActive) {
			// 关键逻辑：物理滚轮没有注入标志，会被拦截并通知线程
			// 逻辑 Roll 发出的信号带有 LLMHF_INJECTED，会在 MouseProc 开头被直接 Pass
			short rDelta = (short)HIWORD(m->mouseData);
			MW.store((int)rDelta, std::memory_order_relaxed);
			SetEvent(MWevent);

			return 1; // 拦截物理滚轮
		}
		break;
	}

	// --- 中键逻辑：普通状态放行，XB1按下时映射为 V ---
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	{
		bool state = (wParam == WM_MBUTTONDOWN);
		// 静态变量 mbMapped 用于记录“当前这一下”中键是否被转换成了 V
		static bool mbMapped = false;

		if (state) {
			// 如果激活且按住了侧键 1
			if (isActive && XB1.load(std::memory_order_acquire)) {
				Press('V');
				mbMapped = true;   // 标记已映射
				return 1;          // 拦截，不触发原中键功能
			}
			else {
				mbMapped = false;  // 标记未映射，走原功能
			}
		}
		else {
			// 松开时，如果刚才按下是被映射的，则释放 V
			if (mbMapped) {
				Release('V');
				mbMapped = false;
				return 1;          // 拦截
			}
		}
		// 只有 mbMapped 为 false 时才会执行到这里，即：正常放行中键
		break;
	}

	}
	return Pass;
}


// --- 【功能 9】 准星 UI 绘制 (对游戏帧率影响极小) ---

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

		// 上方标记 (横线： 宽3高1)
		DrawMark(midX - 1, midY - 1 - markDist, 3, 1);

		// 下方标记 (横线： 宽3高1)
		DrawMark(midX - 1, midY + 1 + markDist, 3, 1);

		// 左侧标记 (竖线： 宽1高3)
		DrawMark(midX - 1 - markDist, midY - 1, 1, 3);

		// 右侧标记 (竖线： 宽1高3)
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

	// --- 修改点：将 40 开为 101 ---
	// 线条 34px + 间距 6px = 40px，两侧总计 80px+，所以窗口必须大于 80。
	int windowSize = 101;

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
	for (int vk = 0; vk < 256; vk++) {
		bool phy = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (g_Out[vk] != phy) {
			// 0x01-0x06 包含 L, R, Middle, X1, X2
			if (vk >= 0x01 && vk <= 0x06) SendMouse(vk, phy);
			else SendKey(vk, phy);
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
	QEevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	SpaceExitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	timeBeginPeriod(1);// 提升定时器精度至1ms

	// 设置 DPI 意识，确保在不同分辨率缩放下的准星位置准确
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	g_mainThreadId = GetCurrentThreadId();

	// 启动线程
	std::thread(Thread_Notify_Manager).detach();
	std::thread(Thread_XB2).detach();
	std::thread(Thread_F).detach();
	std::thread(Thread_Space).detach();
	std::thread(Thread_RB).detach();
	std::thread(Thread_MW).detach();
	std::thread(ActiveWindowMonitor).detach();
	std::thread(CreateCrosshair).detach();
	std::thread(MonitorAndExit, L"DeltaForceClient-Win64-Shipping.exe").detach();
	std::thread(Thread_QE).detach();

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