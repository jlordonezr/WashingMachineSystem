#ifndef GREYWATER_NET_H
#define GREYWATER_NET_H

#include <Arduino.h>

// Wi-Fi features. Everything here is optional: if Wi-Fi is missing or down,
// the wash cycle runs exactly as it does without it.
//
//  - Wi-Fi setup page (WiFiManager): on first boot, or after holding STOP for
//    5 s while the screen shows READY, the board opens the network
//    WIFI_AP_NAME. Join it with a phone and pick the home Wi-Fi.
//  - Dashboard at http://reusewater.local (or the IP shown on the OLED),
//    user "admin", password from the settings (default "greywater").
//  - Wireless firmware updates (ArduinoOTA), host name "reusewater".
//  - Phone alerts through ntfy (https://ntfy.sh) when a topic is set.
//  - Clock from NTP for monthly statistics and tank water age.

#define NET_HOSTNAME            "reusewater"
#define WIFI_AP_NAME            "ReuseWater-Setup"
#define WIFI_AP_PASSWORD        "greywater"   // fallback only (no screen); normally "gw-" + MAC digits, shown on the screen
#define CONFIG_PORTAL_TIMEOUT_S 180           // setup page closes after 3 minutes
#define WIFI_RETRY_MS           60000         // try to reconnect every minute when offline
#define WEB_USER                "admin"
#define FIRMWARE_VERSION        "3.3.1"
#define AUTH_SLOTS              6             // devices tracked for the wrong-password lockout
#define ALERT_RETRY_WINDOW_S    1800          // an alert that cannot be sent is tried again for 30 min
#define ALERT_TEST_WINDOW_S     30            // ... the test alert only for 30 s (the app is waiting)
#define ALERT_RETRY_MS          5000          // first wait before trying again, doubled each time
#define ALERT_RETRY_MAX_MS      300000        // ... up to every 5 min
#define ALERT_TIMEOUT_MS        10000         // connect, TLS handshake and answer, each
#define ALERT_PENDING           6             // alerts that can wait for a retry at the same time

void netBegin();               // call once in setup(), after settingsBegin()
void netLoop();                // call on every loop() pass (non-blocking)
void netStartConfigPortal();   // open the Wi-Fi setup page now
bool netPortalActive();
bool netIsConnected();

// Queue a phone alert. Returns immediately; a background task sends it.
// priority: 1 (min) .. 5 (max), as defined by ntfy. tags: ntfy emoji tags.
void notify(const char* title, const char* body, uint8_t priority = 3, const char* tags = "");

#endif
