/* Copyright (C) 2005-2010, Thorvald Natvig <thorvald@natvig.com>
   Copyright (C) 2011, Kissaki <kissaki@gmx.de>
   Copyright (C) 2011, Nye Liu <nyet@nyet.org>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "lib.h"
#include <d3d11.h>
#include <d3dx10math.h>
#include <d3dx11.h>
#include <time.h>

DXGIData *dxgi = NULL;

static bool bHooked = false;
static HardHook hhPresent;
static HardHook hhResize;

typedef HRESULT(__stdcall *CreateDXGIFactory1Type)(REFIID, void **);

typedef HRESULT(__stdcall *PresentType)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(__stdcall *ResizeBuffersType)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

#define HMODREF(mod, func) func##Type p##func = (func##Type) GetProcAddress(mod, #func)

extern HRESULT presentD3D10(IDXGISwapChain *pSwapChain);

extern HRESULT presentD3D11(IDXGISwapChain *pSwapChain);

static HRESULT __stdcall myPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags) {
	// Present is called for each frame. Thus, we do not want to always log here.
	#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	ods("DXGI: Call to Present; Drawing and then chaining the call to the original logic");
	#endif

	presentD3D10(pSwapChain);
	presentD3D11(pSwapChain);

	//TODO: Move logic to HardHook.
	// Call base without active hook in case of no trampoline.
	PresentType oPresent = (PresentType) hhPresent.call;
	hhPresent.restore();
	HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);
	hhPresent.inject();

	return hr;
}

extern void resizeD3D10(IDXGISwapChain *pSwapChain);
extern void resizeD3D11(IDXGISwapChain *pSwapChain);

static HRESULT __stdcall myResize(IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
	#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	ods("DXGI: Call to Resize. Forwarding to D3D10 and D3D11 custom implementations before calling original logic ...");
	#endif

	resizeD3D10(pSwapChain);
	resizeD3D11(pSwapChain);

	//TODO: Move logic to HardHook.
	// Call base without active hook in case of no trampoline.
	ResizeBuffersType oResize = (ResizeBuffersType) hhResize.call;
	hhResize.restore();
	HRESULT hr = oResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	hhResize.inject();
	return hr;
}

static void HookPresentRaw(voidFunc vfPresent) {
	ods("DXGI: Injecting Present");
	hhPresent.setup(vfPresent, reinterpret_cast<voidFunc>(myPresent));
}

static void HookResizeRaw(voidFunc vfResize) {
	ods("DXGI: Injecting ResizeBuffers Raw");
	hhResize.setup(vfResize, reinterpret_cast<voidFunc>(myResize));
}

void hookDXGI(HMODULE hDXGI, bool preonly);

void checkDXGIHook(bool preonly) {
	static bool bCheckHookActive = false;
	if (bCheckHookActive) {
		ods("DXGI: Recursion in checkDXGIHook");
		return;
	}

	if (! dxgi->iOffsetPresent || ! dxgi->iOffsetResize)
		return;

	bCheckHookActive = true;

	HMODULE hDXGI = GetModuleHandleW(L"DXGI.DLL");

	if (hDXGI) {
		if (! bHooked) {
			hookDXGI(hDXGI, preonly);
		}
	#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	} else {
		ods("DXGI: No DXGI.DLL found as loaded. No hooking at this point.");
	#endif
	}

	bCheckHookActive = false;
}

/// @param hDXGI must be a valid module handle.
void hookDXGI(HMODULE hDXGI, bool preonly) {
	const int procnamesize = 2048;
	wchar_t procname[procnamesize];
	GetModuleFileNameW(NULL, procname, procnamesize);
	ods("DXGI: hookDXGI in App '%ls'", procname);

	// Add a ref to ourselves; we do NOT want to get unloaded directly from this process.
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<char *>(&hookDXGI), &hSelf);

	bHooked = true;

	// Can we use the prepatch data?
	GetModuleFileNameW(hDXGI, procname, procnamesize);
	if (_wcsicmp(dxgi->wcDXGIFileName, procname) == 0) {
		// The module seems to match the one we prepared d3dd for.

		unsigned char *raw = (unsigned char *) hDXGI;
		HookPresentRaw((voidFunc)(raw + dxgi->iOffsetPresent));
		HookResizeRaw((voidFunc)(raw + dxgi->iOffsetResize));

	} else if (! preonly) {
		ods("DXGI: Interface changed, can't rawpatch");
	} else {
		bHooked = false;
	}
}

extern void PrepareDXGI10();
extern void PrepareDXGI11();

extern "C" __declspec(dllexport) void __cdecl PrepareDXGI() {
	if (! dxgi)
		return;

	// This function is called by the Mumble client in Mumble's scope
	// mainly to extract the offsets of various functions in the IDXGISwapChain
	// and IDXGIObject interfaces that need to be hooked in target
	// applications. The data is stored in the dxgi shared memory structure.

	if (! dxgi)
		return;

	ods("DXGI: Preparing static data for DXGI Injection");

	dxgi->wcDXGIFileName[0] = 0;
	dxgi->iOffsetPresent = 0;
	dxgi->iOffsetResize = 0;

	OSVERSIONINFOEXW ovi;
	memset(&ovi, 0, sizeof(ovi));
	ovi.dwOSVersionInfoSize = sizeof(ovi);
	GetVersionExW(reinterpret_cast<OSVERSIONINFOW *>(&ovi));
	// Make sure this is (Win7?) or greater
	if ((ovi.dwMajorVersion >= 7) || ((ovi.dwMajorVersion == 6) && (ovi.dwBuildNumber >= 6001))) {
		HMODULE hDXGI = LoadLibrary("DXGI.DLL");

		if (hDXGI != NULL) {
			GetModuleFileNameW(hDXGI, dxgi->wcDXGIFileName, 2048);

			CreateDXGIFactory1Type pCreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactory1Type>(GetProcAddress(hDXGI, "CreateDXGIFactory1"));
			ods("DXGI: Got CreateDXGIFactory1 at %p", pCreateDXGIFactory1);
			if (pCreateDXGIFactory1) {
				IDXGIFactory1 * pFactory;
				HRESULT hr = pCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));
				if (FAILED(hr))
					ods("DXGI: Call to pCreateDXGIFactory1 failed!");
				if (pFactory) {
					HWND hwnd = CreateWindowW(L"STATIC", L"Mumble DXGI1 Window", WS_OVERLAPPEDWINDOW,
											  CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, 0,
											  NULL, NULL, 0);

					IDXGIAdapter1 *pAdapter = NULL;
					pFactory->EnumAdapters1(0, &pAdapter);

					PrepareDXGI10();
					PrepareDXGI11();

					DestroyWindow(hwnd);

					pFactory->Release();
				} else {
					FreeLibrary(hDXGI);
				}
			} else {
				FreeLibrary(hDXGI);
			}
		} else {
			FreeLibrary(hDXGI);
		}
	} else {
		ods("DXGI: No DXGI pre-Vista - skipping prepare");
	}
}