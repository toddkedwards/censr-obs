/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Sole allocation of CLSID_CensrDShowSource. Includes ONLY the guid header after
initguid.h so DirectShow's own GUIDs still come from strmiids.lib.
*/
#include <windows.h>
#include <initguid.h>

#include "dshow_guid.h"
