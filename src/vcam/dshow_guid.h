/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <guiddef.h>

/* CLSID of the Censr DirectShow capture source filter (registered under
 * CLSID_VideoInputDeviceCategory so legacy apps enumerate it as a webcam). */
// {A2E3F1B0-4C5D-4E6F-8A9B-0C1D2E3F4A5B}
DEFINE_GUID(CLSID_CensrDShowSource, 0xa2e3f1b0, 0x4c5d, 0x4e6f, 0x8a, 0x9b,
	    0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b);

#define CENSR_DSHOW_FRIENDLY_NAME L"Censr Virtual Camera"
#define CENSR_DSHOW_CLSID_STRING L"{A2E3F1B0-4C5D-4E6F-8A9B-0C1D2E3F4A5B}"

#define CENSR_DSHOW_WIDTH 1920
#define CENSR_DSHOW_HEIGHT 1080
#define CENSR_DSHOW_FPS 30
