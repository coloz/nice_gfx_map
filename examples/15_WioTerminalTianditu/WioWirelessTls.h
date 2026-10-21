#pragma once
#include <erpc/rpc_system.h>
#include <erpc/erpc_port.h>
#include "WioTlsBootstrap.h"
#include "WioSamd51Entropy.h"
#include "WioNetworkTime.h"

namespace wio_map {
inline bool &wirelessTlsProvisioned() { static bool value = false; return value; }
inline void invalidateWirelessTls() { wirelessTlsProvisioned() = false; }
inline bool ensureWirelessTls() {
    bool &provisioned = wirelessTlsProvisioned();
    static uint32_t provisionedAt = 0;
    if (provisioned && uint32_t(millis() - provisionedAt) < 3u * 60 * 60 * 1000)
        return true;
    if (!ensureNetworkTime()) return false;
    char *version = rpc_system_version();
    Serial.print("WIO_MAP_RADIO version=");
    Serial.println(version ? version : "unavailable");
    if (!wio_tls_bootstrap::supportsVersion(version)) {
        Serial.println("WIO_MAP_TLS_BOOTSTRAP unsupported_firmware");
        if (version) erpc_free(version);
        return false;
    }
    uint8_t seed[64] = {};
    if (!wio_tls_bootstrap::samd51Entropy(seed)) {
        erpc_free(version);
        Serial.println("WIO_MAP_TLS_BOOTSTRAP trng_unavailable");
        return false;
    }
    const int32_t result = wio_tls_bootstrap::provision(version, seed, tlsEpoch());
    wio_tls_bootstrap::clearEntropy(seed, sizeof(seed));
    erpc_free(version);
    Serial.print("WIO_MAP_TLS_BOOTSTRAP result="); Serial.println(result);
    provisioned = result == 0;
    if (provisioned) provisionedAt = millis();
    return provisioned;
}
} // namespace wio_map
