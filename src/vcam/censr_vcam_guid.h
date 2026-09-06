/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <guiddef.h>

/* CLSID of the Censr media source (the in-proc COM object the frame server
 * activates to back the virtual camera). Stable — the registration and the
 * MFCreateVirtualCamera call both reference it. */
// {6F9A3B2C-1D4E-4F8A-9C2B-7E5D3A1B0C9F}
DEFINE_GUID(CLSID_CensrMediaSource, 0x6f9a3b2c, 0x1d4e, 0x4f8a, 0x9c, 0x2b,
	    0x7e, 0x5d, 0x3a, 0x1b, 0x0c, 0x9f);

#define CENSR_VCAM_FRIENDLY_NAME L"Censr Virtual Camera"
#define CENSR_VCAM_CLSID_STRING L"{6F9A3B2C-1D4E-4F8A-9C2B-7E5D3A1B0C9F}"

/* Virtual-camera output format (RGB32 == BGRA in memory, matching the host's
 * shared frames — no color conversion needed). */
#define CENSR_VCAM_WIDTH 1920
#define CENSR_VCAM_HEIGHT 1080
#define CENSR_VCAM_FPS 30
