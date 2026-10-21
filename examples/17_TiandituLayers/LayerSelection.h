#pragma once
#include <Providers.h>
namespace layer_app {
using namespace nice_gfx_map;
using providers::TiandituLayer;
struct Selection { TiandituLayer layer=TiandituLayer::Vector; Projection projection=Projection::WebMercator; bool labels=true,borders=false,strict=false; };
inline providers::TiandituOptions layerOptions(TiandituLayer layer,const Selection &selection) {
  providers::TiandituOptions o;o.layer=layer;o.projection=selection.projection;
  if(layer==TiandituLayer::VectorLabels&&selection.projection==Projection::WebMercator)o.grid.maxZoom=18;
  o.grid.useTiandituCtaWMercatorOverride=!selection.strict&&layer==TiandituLayer::TerrainLabels&&selection.projection==Projection::WebMercator;
  return o;
}
// Worker-side operation. No Map/UI calls; transactional output and the same
// bounded Transport/WMTS parser used by hand-written products. No URL logging.
inline Status buildProfile(Transport &http,const Selection &selection,const std::string &key,providers::TileProfile &output) {
  TiandituLayer layers[3]={selection.layer};size_t count=1;
  if(selection.labels) {
    if(selection.layer==TiandituLayer::Vector)layers[count++]=TiandituLayer::VectorLabels;
    if(selection.layer==TiandituLayer::Imagery)layers[count++]=TiandituLayer::ImageryLabels;
    if(selection.layer==TiandituLayer::Terrain)layers[count++]=TiandituLayer::TerrainLabels;
  }
  if(selection.borders&&selection.layer!=TiandituLayer::Boundaries)layers[count++]=TiandituLayer::Boundaries;
  providers::TileProfile profiles[3];
  for(size_t i=0;i<count;++i) {
    const auto o=layerOptions(layers[i],selection);const auto request=providers::tiandituCapabilities(key,o);
    if(request.url.empty())return Status::InvalidArgument;
    ByteBuffer bytes;auto status=http.fetch(request,bytes);if(status!=Status::Ok)return status;
    wmts::Capabilities capabilities;status=wmts::parseCapabilities(bytes.data(),bytes.size(),capabilities);if(status!=Status::Ok)return status;
    status=providers::tiandituProfile(key,capabilities,o,profiles[i]);if(status!=Status::Ok)return status;
  }
  return providers::combineProfiles(profiles,count,output);
}
}
