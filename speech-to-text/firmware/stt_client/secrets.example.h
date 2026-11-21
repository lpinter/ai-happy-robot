// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is gitignored so your Wi-Fi password never ends up in git.
#pragma once
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"
#define STT_URL   "http://192.168.x.x:8000/talk"  // your PC's IPv4 ( from ipconfig), path /talk

// Optional: Happy's own Wi-Fi network, used when the router above can't be reached.
// Join it from the PC; Happy then uses the port and path of STT_URL with the PC's new address.
// The password needs at least 8 characters. Leave these out to use the defaults below.
#define AP_SSID   "Happy-Robot"
#define AP_PASS   "happy1234"
