// Requires SD card, external PNGdec and the GENERATED data/tiles directory.
// Missing SD/files remain visible errors; there is no synthetic fallback.
#include <demo/ExampleSupport.h>
#include <adapters/ArduinoFs.h>
#include <codecs/PngDecoder.h>
#include <SD.h>
#include <SPI.h>
#ifndef NICE_MAP_SD_CS
#define NICE_MAP_SD_CS 13
#endif
#ifndef NICE_MAP_SD_SCK
#define NICE_MAP_SD_SCK 14
#endif
#ifndef NICE_MAP_SD_MISO
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define NICE_MAP_SD_MISO 12
#else
#define NICE_MAP_SD_MISO 32
#endif
#endif
#ifndef NICE_MAP_SD_MOSI
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define NICE_MAP_SD_MOSI 11
#else
#define NICE_MAP_SD_MOSI 27
#endif
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
SPIClass sdSpi(HSPI); // S3 HSPI is SPI3, separate from NICE_GFX's SPI2.
#elif defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32
SPIClass sdSpi(VSPI); // Classic ESP32 VSPI is SPI3; HSPI would conflict.
#else
#error "This dual-bus SD example supports ESP32-S3/classic ESP32; select a separate SPI3 host explicitly for other targets."
#endif
using namespace nice_gfx_map;
demo::Example app;
Map &mapView=app.mapView;
FsTileTransport transport(SD,"/tiles","demo-v1",512*1024,esp32PsramAllocator());
PngDecoder decoder(esp32PsramAllocator());
TileLoader loader(transport,decoder,[]()->uint32_t{return millis();});
bool sdReady=false;
Status lastLoad=Status::NotFound;
void mountCard() {
  SD.end();sdSpi.begin(NICE_MAP_SD_SCK,NICE_MAP_SD_MISO,NICE_MAP_SD_MOSI,NICE_MAP_SD_CS);
  sdReady=SD.begin(NICE_MAP_SD_CS,sdSpi,10000000);mapView.clearCache();
  Serial.println(sdReady?"SD mounted: /tiles/demo-v1/0/14/x/y.png":"SD mount failed (check separate SPI3 pins)");
}
void setup() {
  if(!app.begin("10 SD PNG map: fixture / no HTTP",demo::home(),14))return;
  app.automaticTiles=false;
  // This synthetic fixture is a FILE source. Its URL is an identity template,
  // never fetched: FsTileTransport derives the layer/z/x/y PNG pathname.
  auto source=providers::xyz("https://offline.invalid/{z}/{x}/{y}.png",
                            "Generated synthetic SD fixture, not real roads",14,14,128);
  mapView.setSource(source);mountCard();
  Serial.println("Copy data/tiles to SD root. r: remount; c: clear RAM cache. Zoom fixed at fixture z14.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key=='r')mountCard();
    if(key=='c')mapView.clearCache();
  }
  if(sdReady) {
    const Status status=loader.loadNext(mapView,millis());
    if(status!=Status::Busy&&status!=Status::NotFound)lastLoad=status;
    // Missing files are NotFound, not an instruction to generate replacements.
    if(status==Status::NotFound&&mapView.cacheStats().failures)lastLoad=Status::NotFound;
  }
  char status[100];std::snprintf(status,sizeof(status),"%s %s tiles%u fail%u",sdReady?"SD":"NO CARD",sdReady?statusName(lastLoad):"check CS13/data",unsigned(mapView.cacheStats().readyTiles),unsigned(mapView.cacheStats().failures));
  app.tick(status);delay(5);
}
