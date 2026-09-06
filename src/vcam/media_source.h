/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <mfidl.h>

/* Factory used by the class object (DllGetClassObject) to instantiate the Censr
 * media source that backs the virtual camera. */
HRESULT CreateCensrMediaSource(REFIID riid, void **ppv);
