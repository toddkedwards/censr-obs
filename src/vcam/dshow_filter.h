/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <unknwn.h>

/* Factory used by the class object (DllGetClassObject) to instantiate the Censr
 * DirectShow capture source filter. */
HRESULT CreateCensrDShowFilter(REFIID riid, void **ppv);
