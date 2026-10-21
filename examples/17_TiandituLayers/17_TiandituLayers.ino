// ESP32 live WMTS layer/projection browser. See README.md; configure only the
// ignored LayerSecrets.h locally. This console never logs credential URLs.
#include "LayerApp.h"
void setup() { layer_app::begin(); }
void loop() { layer_app::poll(); }
