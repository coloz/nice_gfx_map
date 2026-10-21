// ESP32-only live service console and LCD/map demonstration. See README.md.
// Configure a local ignored ServiceSecrets.h; the checked-in template compiles
// but intentionally makes no network requests. Never print Request/url/raw.
#include "ServiceApp.h"
void setup(){service_app::begin();}
void loop(){service_app::poll();}
