/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Control tool for the Censr virtual camera. Run ELEVATED.

  censr-vcam install [path\to\censr-mfcam.dll]
      Register the media-source COM server and create the "Censr Virtual Camera"
      (system lifetime, so it persists for apps to select).
  censr-vcam uninstall [path\to\censr-mfcam.dll]
      Remove the virtual camera and unregister the COM server.

The camera shows the redacted frames published by a running censr-host; without
the host it shows a dark placeholder.
*/

#include "censr_vcam_guid.h"

#include <windows.h>

#include <mfapi.h>
#include <mfvirtualcamera.h>

#include <cstdio>
#include <string>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static std::wstring dll_path(int argc, wchar_t **argv, int idx)
{
	if (argc > idx)
		return argv[idx];
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring p = exe;
	auto slash = p.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		p.resize(slash + 1);
	return p + L"censr-mfcam.dll";
}

typedef HRESULT(STDAPICALLTYPE *RegFn)();

static HRESULT call_dll_reg(const std::wstring &dll, const char *entry)
{
	HMODULE m = LoadLibraryW(dll.c_str());
	if (!m) {
		std::printf("could not load %ls (%lu)\n", dll.c_str(),
			    GetLastError());
		return E_FAIL;
	}
	auto fn = (RegFn)GetProcAddress(m, entry);
	HRESULT hr = fn ? fn() : E_FAIL;
	FreeLibrary(m);
	return hr;
}

static HRESULT create_camera(ComPtr<IMFVirtualCamera> &cam)
{
	return MFCreateVirtualCamera(
		MFVirtualCameraType_SoftwareCameraSource,
		MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser,
		CENSR_VCAM_FRIENDLY_NAME, CENSR_VCAM_CLSID_STRING, nullptr, 0,
		&cam);
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2) {
		std::printf("usage: censr-vcam <install|uninstall> "
			    "[path\\censr-mfcam.dll]\n");
		return 2;
	}
	std::wstring cmd = argv[1];
	std::wstring dll = dll_path(argc, argv, 2);

	HRESULT hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) {
		std::printf("MFStartup failed 0x%08x\n", (unsigned)hr);
		return 1;
	}
	int rc = 0;

	if (cmd == L"install") {
		hr = call_dll_reg(dll, "DllRegisterServer");
		if (FAILED(hr)) {
			std::printf("COM registration failed 0x%08x "
				    "(run elevated?)\n", (unsigned)hr);
			rc = 1;
		} else {
			ComPtr<IMFVirtualCamera> cam;
			hr = create_camera(cam);
			if (SUCCEEDED(hr))
				hr = cam->Start(nullptr);
			if (SUCCEEDED(hr))
				std::printf("Censr Virtual Camera installed.\n");
			else {
				std::printf("MFCreateVirtualCamera/Start failed "
					    "0x%08x\n", (unsigned)hr);
				rc = 1;
			}
		}
	} else if (cmd == L"uninstall") {
		ComPtr<IMFVirtualCamera> cam;
		hr = create_camera(cam);
		if (SUCCEEDED(hr))
			cam->Remove();
		call_dll_reg(dll, "DllUnregisterServer");
		std::printf("Censr Virtual Camera removed.\n");
	} else {
		std::printf("unknown command '%ls'\n", cmd.c_str());
		rc = 2;
	}

	MFShutdown();
	return rc;
}
