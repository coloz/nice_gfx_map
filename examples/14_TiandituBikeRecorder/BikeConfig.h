#pragma once
// ESP32-S3 + PSRAM + ILI9341. These are example wires, not a board pinout.
// Edit the strings locally; never commit a real key/password to a public repo.
namespace bike_config {
inline constexpr char WifiSsid[] = "YOUR_WIFI_SSID";
inline constexpr char WifiPassword[] = "YOUR_WIFI_PASSWORD";
inline constexpr char TiandituKey[] = "YOUR_TIANDITU_KEY";
// Paste the current trusted root CA of t0.tianditu.gov.cn from a trusted source.
// Empty means network loading stays disabled; insecure TLS is never enabled.
inline constexpr char RootCa[] = R"PEM(
REPLACE_WITH_TRUSTED_ROOT_CA_PEM
)PEM";
inline constexpr char RoutePath[] = "/routes/bike.csv";
inline constexpr uint32_t GnssBaud = 9600;
inline constexpr uint32_t FixTimeoutMs = 3000;
inline constexpr bool AutoStartRecording = true;
}
#ifndef NICE_BIKE_TFT_CS
#define NICE_BIKE_TFT_CS 10
#endif
#ifndef NICE_BIKE_TFT_DC
#define NICE_BIKE_TFT_DC 9
#endif
#ifndef NICE_BIKE_TFT_RST
#define NICE_BIKE_TFT_RST 8
#endif
#ifndef NICE_BIKE_TFT_SCK
#define NICE_BIKE_TFT_SCK 12
#endif
#ifndef NICE_BIKE_TFT_MOSI
#define NICE_BIKE_TFT_MOSI 11
#endif
#ifndef NICE_BIKE_TFT_MISO
#define NICE_BIKE_TFT_MISO 13
#endif
// NICE_GFX uses SPI2; give SD its own SPI3 bus.
#ifndef NICE_BIKE_SD_SCK
#define NICE_BIKE_SD_SCK 14
#endif
#ifndef NICE_BIKE_SD_MOSI
#define NICE_BIKE_SD_MOSI 15
#endif
#ifndef NICE_BIKE_SD_MISO
#define NICE_BIKE_SD_MISO 16
#endif
#ifndef NICE_BIKE_SD_CS
#define NICE_BIKE_SD_CS 17
#endif
#ifndef NICE_BIKE_GNSS_RX
#define NICE_BIKE_GNSS_RX 18
#endif
#ifndef NICE_BIKE_GNSS_TX
#define NICE_BIKE_GNSS_TX -1
#endif
