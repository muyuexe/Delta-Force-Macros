#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <ShellScalingApi.h>

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Winmm.lib")