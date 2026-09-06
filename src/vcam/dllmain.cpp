/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

In-proc COM server entry points for the Censr media source: class factory,
DllGetClassObject, and (un)registration of the CLSID under HKCR\CLSID. Register
with an elevated `regsvr32 censr-mfcam.dll` (or via the censr-vcam tool).
*/

#include "media_source.h"
#include "censr_vcam_guid.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <new>

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
		return CreateCensrMediaSource(riid, ppv);
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

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (clsid == CLSID_CensrMediaSource) {
		auto cf = Make<ClassFactory>();
		if (!cf)
			return E_OUTOFMEMORY;
		return cf.CopyTo(riid, ppv);
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

/* Conservative: keep the DLL resident for the life of the host process. */
STDAPI DllCanUnloadNow()
{
	return S_FALSE;
}

STDAPI DllRegisterServer()
{
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(g_module, path, MAX_PATH))
		return HRESULT_FROM_WIN32(GetLastError());

	wchar_t clsidKey[128];
	swprintf_s(clsidKey, L"CLSID\\%s", CENSR_VCAM_CLSID_STRING);
	wchar_t inprocKey[160];
	swprintf_s(inprocKey, L"CLSID\\%s\\InprocServer32",
		   CENSR_VCAM_CLSID_STRING);

	if (set_key(HKEY_CLASSES_ROOT, clsidKey, nullptr,
		    L"Censr Virtual Camera Source") != ERROR_SUCCESS)
		return E_ACCESSDENIED;
	if (set_key(HKEY_CLASSES_ROOT, inprocKey, nullptr, path) != ERROR_SUCCESS)
		return E_ACCESSDENIED;
	if (set_key(HKEY_CLASSES_ROOT, inprocKey, L"ThreadingModel", L"Both") !=
	    ERROR_SUCCESS)
		return E_ACCESSDENIED;
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	wchar_t clsidKey[128];
	swprintf_s(clsidKey, L"CLSID\\%s", CENSR_VCAM_CLSID_STRING);
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
