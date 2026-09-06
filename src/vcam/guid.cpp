/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Sole allocation of CLSID_CensrMediaSource (INITGUID makes DEFINE_GUID emit the
definition here; every other TU sees it as extern).
*/
#include <windows.h>
#include <initguid.h>

#include "censr_vcam_guid.h"
