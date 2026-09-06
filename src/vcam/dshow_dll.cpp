/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

In-proc COM server entry points for the Censr DirectShow source filter: class
factory, DllGetClassObject, and registration under CLSID_VideoInputDeviceCategory
(via IFilterMapper2) so legacy apps enumerate it as a webcam. Register with an
elevated `regsvr32 censr-dshowcam.dll`.
*/

#include "dshow_filter.h"
#include "dshow_guid.h"

#include <windows.h>

#include <dshow.h>

#include <cstdio>
#include <cwchar>

#include <wrl/client.h>
#include <wrl/implements.h>

using namespace Microsoft::WRL;

static HMODULE g_module = nullptr;

namespace {

class ClassFactory
	: public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
	IFACEMETHODIMP CreateInstance(IUnknown *outer, REFIID riid,
				      void **ppv) override
	{
		if (outer)
			return CLASS_E_NOAGGREGATION;
		return CreateCensrDShowFilter(riid, ppv);
	}
	IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

LONG set_key(HKEY root, const wchar_t *sub, const wchar_t *name,
	     const wchar_t *value)
{
	HKEY key;
	LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr,
				 &key, nullptr);
	if (r != ERROR_SUCCESS)
		return r;
	r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
			   (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(key);
	return r;
}

HRESULT register_device(bool add)
{
	ComPtr<IFilterMapper2> mapper;
	HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr,
				      CLSCTX_INPROC_SERVER,
				      IID_PPV_ARGS(&mapper));
	if (FAILED(hr))
		return hr;

	if (!add)
		return mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory,
						CENSR_DSHOW_CLSID_STRING,
						CLSID_CensrDShowSource);

	REGPINTYPES pinType{};
	pinType.clsMajorType = &MEDIATYPE_Video;
	pinType.clsMinorType = &MEDIASUBTYPE_NULL;

	REGFILTERPINS pin{};
	pin.strName = const_cast<wchar_t *>(L"Capture");
	pin.bRendered = FALSE;
	pin.bOutput = TRUE;
	pin.bZero = FALSE;
	pin.bMany = FALSE;
	pin.clsConnectsToFilter = &GUID_NULL;
	pin.nMediaTypes = 1;
	pin.lpMediaType = &pinType;

	REGFILTER2 rf2{};
	rf2.dwVersion = 1;
	rf2.dwMerit = MERIT_DO_NOT_USE;
	rf2.cPins = 1;
	rf2.rgPins = &pin;

	ComPtr<IMoniker> moniker;
	return mapper->RegisterFilter(CLSID_CensrDShowSource,
				      CENSR_DSHOW_FRIENDLY_NAME, &moniker,
				      &CLSID_VideoInputDeviceCategory,
				      CENSR_DSHOW_CLSID_STRING, &rf2);
}

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (clsid == CLSID_CensrDShowSource) {
		auto cf = Make<ClassFactory>();
		if (!cf)
			return E_OUTOFMEMORY;
		return cf.CopyTo(riid, ppv);
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
	return S_FALSE;
}

STDAPI DllRegisterServer()
{
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(g_module, path, MAX_PATH))
		return HRESULT_FROM_WIN32(GetLastError());

	wchar_t clsidKey[128], inprocKey[160];
	swprintf_s(clsidKey, L"CLSID\\%s", CENSR_DSHOW_CLSID_STRING);
	swprintf_s(inprocKey, L"CLSID\\%s\\InprocServer32",
		   CENSR_DSHOW_CLSID_STRING);
	if (set_key(HKEY_CLASSES_ROOT, clsidKey, nullptr,
		    L"Censr Virtual Camera (DirectShow)") != ERROR_SUCCESS ||
	    set_key(HKEY_CLASSES_ROOT, inprocKey, nullptr, path) !=
		    ERROR_SUCCESS ||
	    set_key(HKEY_CLASSES_ROOT, inprocKey, L"ThreadingModel", L"Both") !=
		    ERROR_SUCCESS)
		return E_ACCESSDENIED;

	HRESULT hr = CoInitialize(nullptr);
	HRESULT r = register_device(true);
	if (SUCCEEDED(hr))
		CoUninitialize();
	return r;
}

STDAPI DllUnregisterServer()
{
	HRESULT hr = CoInitialize(nullptr);
	register_device(false);
	if (SUCCEEDED(hr))
		CoUninitialize();
	wchar_t clsidKey[128];
	swprintf_s(clsidKey, L"CLSID\\%s", CENSR_DSHOW_CLSID_STRING);
	RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey);
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = inst;
		DisableThreadLibraryCalls(inst);
	}
	return TRUE;
}
