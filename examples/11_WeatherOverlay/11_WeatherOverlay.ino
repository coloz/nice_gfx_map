// Synthetic weather pixels. This is not a forecast or a weather data service.
#include <demo/ExampleSupport.h>
using namespace nice_gfx_map;
demo::Example app;
Map &mapView=app.mapView;
int opacity=180;
bool visible=true;
unsigned forecastFrame=0;
void setup() {
  if(!app.begin("11 Weather alpha overlay / SYNTH"))return;
  app.useProceduralOverlay(true);
  Serial.println("[/]: opacity; o: overlay on/off; n: next synthetic frame. Blue/yellow/red are generated test pixels.");
}
void loop() {
  while(Serial.available()) {
    const int key=Serial.read();if(app.handleKey(key))continue;
    if(key=='[')opacity=std::max(0,opacity-25);
    if(key==']')opacity=std::min(255,opacity+25);
    if(key=='o')visible=!visible;
    if(key=='n'&&mapView.source().layers.size()>1) {
      app.decoder.phase=++forecastFrame;
      mapView.setLayer(1,mapView.source().layers[1]); // Refresh weather only, retain base tiles.
    }
    mapView.setLayerOpacity(1,uint8_t(visible?opacity:0));
  }
  char status[100];std::snprintf(status,sizeof(status),"SYNTH frame%u alpha%u %s cached%u",forecastFrame,unsigned(opacity),visible?"ON":"OFF",unsigned(mapView.cacheStats().readyTiles));
  app.tick(status);delay(2);
}
