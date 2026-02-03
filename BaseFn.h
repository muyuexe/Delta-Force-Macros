#pragma once
#include <windows.h>
#include <atomic>

// --- 业务（瞄准状态）账本结构定义 ---
struct AimLedger {
    std::atomic<bool> u{ false };
    std::atomic<bool> r{ false };
};

// 外部变量声明，确保 ResetAim 能访问主程序中的 Aim 实例
extern AimLedger Aim;

// --- 基础动作函数声明 ---

// 封装：等待指定毫秒
void Wait(int ms);

// 修改 SendKey
void SendKey(BYTE vk, bool down, ULONG_PTR extra = 0);

// 修改 SendMouse
void SendMouse(BYTE vk, bool down, ULONG_PTR extra = 0);

// 修改 Press / Release / Tap 顺延传递参数
void Press(BYTE vk, ULONG_PTR extra = 0);

void Release(BYTE vk, ULONG_PTR extra = 0);

void Tap(BYTE vk, ULONG_PTR extra = 0);

void ResetAim();