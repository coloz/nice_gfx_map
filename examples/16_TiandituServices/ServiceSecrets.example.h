#pragma once
// Copy to ServiceSecrets.h locally. Never commit ServiceSecrets.h or a binary
// containing credentials. Use a Tianditu server/native application key.
namespace service_secrets {
constexpr const char *WifiSsid="YOUR_2G4_WIFI_SSID";
constexpr const char *WifiPassword="YOUR_WIFI_PASSWORD";
constexpr const char *TiandituKey="YOUR_TIANDITU_SERVER_KEY";
// Public read-only WFS commands do not require this key. Without it, their
// actual metadata/features still display as text; protected basemaps stay off.
// Use the current trusted root certificate from the official service chain.
// HTTPS and SNTP are required; this example has no insecure TLS fallback.
constexpr const char RootCa[]=R"PEM(
REPLACE_WITH_THE_TRUSTED_ROOT_CA_CERTIFICATE_IN_PEM_FORMAT
)PEM";
}
