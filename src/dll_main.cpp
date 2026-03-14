/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dll_log.hpp"
#include "hook_manager.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <delayimp.h>
#include <filesystem>

extern "C" __declspec(dllexport) const char *ReShadeVersion = "MinimalDX12Hook-1.0.0";

HANDLE g_exit_event = nullptr;
HMODULE g_module_handle = nullptr;
std::filesystem::path g_reshade_dll_path;
std::filesystem::path g_reshade_base_path;
std::filesystem::path g_target_executable_path;

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory);
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory);
extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);
extern "C" HRESULT WINAPI D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice);

bool is_uwp_app()
{
	const auto GetCurrentPackageFullName = reinterpret_cast<LONG(WINAPI *)(UINT32 *, PWSTR)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName"));
	if (GetCurrentPackageFullName == nullptr)
		return false;

	UINT32 length = 0;
	return GetCurrentPackageFullName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

bool is_windows7()
{
	ULONGLONG condition = 0;
	VER_SET_CONDITION(condition, VER_MAJORVERSION, VER_LESS_EQUAL);
	VER_SET_CONDITION(condition, VER_MINORVERSION, VER_LESS_EQUAL);

	OSVERSIONINFOEX verinfo_windows7 = { sizeof(verinfo_windows7), 6, 1 };
	return VerifyVersionInfo(&verinfo_windows7, VER_MAJORVERSION | VER_MINORVERSION, condition) != FALSE;
}

std::filesystem::path get_base_path(bool)
{
	return g_reshade_dll_path.parent_path();
}
std::filesystem::path get_system_path()
{
	WCHAR buf[4096];
	return std::filesystem::path(buf, buf + GetSystemDirectoryW(buf, ARRAYSIZE(buf)));
}
std::filesystem::path get_module_path(HMODULE module)
{
	WCHAR buf[4096];
	return std::filesystem::path(buf, buf + GetModuleFileNameW(module, buf, ARRAYSIZE(buf)));
}

template <typename T>
static bool install_named_hook(HMODULE module, const char *name, T replacement)
{
	if (module == nullptr)
		return false;

	const FARPROC target = GetProcAddress(module, name);
	if (target == nullptr)
		return false;

	return reshade::hooks::install(
		name,
		reinterpret_cast<reshade::hook::address>(target),
		reinterpret_cast<reshade::hook::address>(replacement),
		true);
}

static bool install_dx12_and_dxgi_hooks()
{
	const HMODULE dxgi_module = LoadLibraryW(L"dxgi.dll");
	const HMODULE d3d12_module = LoadLibraryW(L"d3d12.dll");

	bool result = true;
	result = install_named_hook(dxgi_module, "CreateDXGIFactory", &CreateDXGIFactory) && result;
	result = install_named_hook(dxgi_module, "CreateDXGIFactory1", &CreateDXGIFactory1) && result;
	result = install_named_hook(dxgi_module, "CreateDXGIFactory2", &CreateDXGIFactory2) && result;
	result = install_named_hook(d3d12_module, "D3D12CreateDevice", &D3D12CreateDevice) && result;

	return reshade::hook::apply_queued_actions() && result;
}

static DWORD WINAPI initialize_hook_thread(LPVOID)
{
	g_reshade_dll_path = get_module_path(g_module_handle);
	g_reshade_base_path = g_reshade_dll_path.parent_path();
	g_target_executable_path = get_module_path(nullptr);

	std::filesystem::path log_path = g_target_executable_path.parent_path() / L"reshade_asi.log";
	std::error_code ec;
	reshade::log::open_log_file(log_path, ec);

	reshade::log::message(
		reshade::log::level::info,
		"Initializing minimal DX12 ImGui hook from '%s' into '%s' ...",
		g_reshade_dll_path.u8string().c_str(),
		g_target_executable_path.u8string().c_str());

	if (!install_dx12_and_dxgi_hooks())
		reshade::log::message(reshade::log::level::error, "Failed to install one or more DX12/DXGI hooks.");
	else
		reshade::log::message(reshade::log::level::info, "Installed DX12/DXGI hooks.");

	return 0;
}

#ifndef RESHADE_TEST_APPLICATION

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
			g_module_handle = hModule;
			DisableThreadLibraryCalls(hModule);

			const HANDLE init_thread = CreateThread(nullptr, 0, &initialize_hook_thread, nullptr, 0, nullptr);
			if (init_thread != nullptr)
				CloseHandle(init_thread);
		}
		break;
	case DLL_PROCESS_DETACH:
		reshade::hooks::uninstall();
		break;
	}

	return TRUE;
}

static FARPROC WINAPI DliNotifyHook2(unsigned dliNotify, PDelayLoadInfo pdli)
{
	if (dliNotify == dliNotePreLoadLibrary && _stricmp(pdli->szDll, "D3DCompiler_47.dll") == 0)
	{
		if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") == nullptr)
		{
			const HMODULE d3dcompiler_47_module = LoadLibraryW((get_system_path() / L"D3DCompiler_47.dll").c_str());
			return reinterpret_cast<FARPROC>(d3dcompiler_47_module);
		}

		if (const HMODULE d3dcompiler_47_module = LoadLibraryW(L"D3DCompiler_47.dll"))
			return reinterpret_cast<FARPROC>(d3dcompiler_47_module);

		if (const HMODULE d3dcompiler_43_module = LoadLibraryW(L"D3DCompiler_43.dll"))
			return reinterpret_cast<FARPROC>(d3dcompiler_43_module);
	}

	return nullptr;
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = DliNotifyHook2;

#endif
