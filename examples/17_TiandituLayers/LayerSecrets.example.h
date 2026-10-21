#pragma once
// Copy to LayerSecrets.h locally; do not commit credentials or flashed binaries.
namespace layer_secrets {
constexpr const char *WifiSsid = "YOUR_2G4_WIFI_SSID";
constexpr const char *WifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char *TiandituKey = "YOUR_TIANDITU_SERVER_KEY";
constexpr const char RootCa[] = R"PEM(
REPLACE_WITH_THE_TRUSTED_ROOT_CA_CERTIFICATE_IN_PEM_FORMAT
)PEM";
}
