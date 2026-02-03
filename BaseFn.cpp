#include "pch.h"
#include "BaseFn.h"

// 封装：等待指定毫秒
void Wait(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 修改 SendKey
void SendKey(BYTE vk, bool down, ULONG_PTR extra) {
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
void SendMouse(BYTE vk, bool down, ULONG_PTR extra) {
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
void Press(BYTE vk, ULONG_PTR extra) {
    if (vk == VK_RBUTTON || vk == VK_LBUTTON) SendMouse(vk, true, extra);
    else SendKey(vk, true, extra);
}

void Release(BYTE vk, ULONG_PTR extra) {
    if (vk == VK_RBUTTON || vk == VK_LBUTTON) SendMouse(vk, false, extra);
    else SendKey(vk, false, extra);
}

void Tap(BYTE vk, ULONG_PTR extra) {
    Press(vk, extra);
    Wait(5);
    Release(vk, extra);
}

void ResetAim() {
    // 检查 U 账本
    if (Aim.u.load()) {
        Release('U'); // 默认 extra=0，Release 信号会经过 Pass 宏并把 Aim.u 置为 false
    }
    // 检查 R 账本
    if (Aim.r.load()) {
        Release(VK_RBUTTON); // 同理，重置 R 状态
    }
}