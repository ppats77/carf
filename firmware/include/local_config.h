#pragma once
// The ESP BLE Mesh SDK also ships a settings.h. Qualify the project path.
#if __has_include("../include/settings.h")
#include "../include/settings.h"
#else
#include "settings.example.h"
#endif

#ifdef CARF_VERIFY_UPLOAD
static_assert(sizeof(CARF_WIFI_SSID) > 1 && sizeof(CARF_UPLOAD_TOKEN) > 1 &&
              sizeof(CARF_RECEIVER_URL) > 1,
              "Set home Wi-Fi, receiver URL and upload token with configure_logger.py before flashing");
#endif
