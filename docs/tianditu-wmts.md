# 天地图 WMTS 图层与投影

本页面向 ESP32 的二维栅格地图。图层目录依据[天地图官方地图服务](https://lbs.tianditu.gov.cn/server/MapService.html)，矩阵依据 2026-10-20 实际返回的 14 份 GetCapabilities；已脱敏的原始响应见 [WMTS 测试夹具](../tests/data/wmts/README.md)。官方元数据存在矛盾，因此“文档列出”“矩阵可解释”和“已实测有内容”分别记录，不以 HTTP 200 推断每个瓦片都有地图。

| 图层 | 内容 | 绘制角色 | `_w` 广告矩阵标识 | `_c` 广告矩阵标识 |
| --- | --- | --- | --- | --- |
| `vec` | 矢量地图的栅格瓦片 | 底图 | 1–18 | 1–18 |
| `cva` | 矢量注记 | PNG alpha 叠加 | 1–19；19 元数据异常，示例取 1–18 | 1–19 |
| `img` | 影像 | 底图 | 1–18 | 1–18 |
| `cia` | 影像注记 | PNG alpha 叠加 | 1–18 | 1–18 |
| `ter` | 地形晕渲 | 底图 | 1–14 | 1–14 |
| `cta` | 地形注记 | PNG alpha 叠加 | 1–18；需显式已知错误修正 | 1–18 |
| `ibo` | 全球境界 | PNG alpha 叠加 | 1–18 | 1–18 |

使用 `https://t0` 至 `https://t7.tianditu.gov.cn/{layer}_{w|c}/wmts`，`SERVICE=WMTS`、`VERSION=1.0.0`；当前响应的 Style 为 `default`、Format 为 `tiles`。`tiles` 不说明图片编码，解码器应按 PNG/JPEG 签名识别，注记和境界必须保留 PNG alpha。不要将矢量底图的名称理解为库支持 PBF 渲染。底图、注记与境界组合取各层公共矩阵范围，例如 `ter+cta` 最高到 14。

## 网格契约

- `Projection::WebMercator`：全球正方形，显示纬度限制约 ±85.05112878°。正常 `_w` CRS 为 `urn:ogc:def:crs:EPSG::900913`，第 `z` 级宽、高均为 `2^z` 张。
- `Projection::Geographic`：经纬度等角网格，全球经度 360°、纬度 180°；当前 `_c` CRS 为 `urn:ogc:def:crs:EPSG::4490`。第 `z` 级宽 `2^z`、高 `2^(z-1)` 张，所以第 1 级为 2×1。经纬度投影不使用 Mercator 的非线性纬度公式。
- 当前官方瓦片均为 256×256；核心显式网格也接受 128/512 的等边瓦片。`TileMatrix::zoom` 是内部二次幂列数级别，`identifier` 是服务实际标识，可为字符串。URL 的 `{z}` 在显式网格入口中替换为编码后的 `identifier`。
- `TileMatrixSetLimits` 如存在，保留行列限界，未列出的级别不暴露；网格需为连续级别。当前 14 份响应没有此字段，不据此虚构地域限制。
- 全球网格按经度环绕，纬度不环绕。纬度极点、视口外区域和地域限制外像素使用背景色，不请求不存在的行。
- 投影与基准不同。库不自动转换 CGCS2000、WGS84、GCJ-02、BD-09；服务数据、定位和覆盖物需使用一致的坐标约定。

原 `MapConfig`、`TileSource`、`SourceLayer` 字段布局、`Map` pimpl 大小及原函数签名保持不变。`project(point,z,size)` / `unproject(...)`、`TileSource::url(key)` 和 `Map::setSource(source)` 仍使用原 Mercator 语义。后者也会从显式 Geographic 配置切回默认 Mercator。新增四参数投影函数、`url(key,grid)`、`setSource(source,grid)` 与只读 `sourceGrid()`。

显式网格的平移、连续缩放锚点、fit、坐标命中与 Native/Wire 输出使用同一投影。切换投影取消原请求并清瓦片缓存，已存路线、轨迹和多边形重新投影，标记与定位保留。地图米制圆采用当前投影的局部比例；Geographic 中东西、南北比例不同，显示为椭圆。这不是极区或长距离的测地线圆。Mercator 显示会裁剪超出其纬度范围的几何。

该裁剪会改变库当前保存的投影几何；再切回 `_c` 不会自动恢复原始极区顶点。需要无损往返时，由应用保留原始 GeoPoint，切回 Geographic 后重新 `setRoute`、`clearTrack`/`appendTrack`、`setPolygons`，并按需重新设置标记、圆、定位和中心。

`MarkerShape::Pin` 可用于地点，尖端锚定 GeoPoint、圆头中心透明；默认 `Circle` 和旧四字段 aggregate 保持原行为。图标像素大小不随缩放改变，两投影与 Native/Wire 的命中和裁剪契约一致。该形状是原生绘图能力，无需图片资源或字体图标。

## 必须显式处理的元数据错误

原始 XML 保留，`wmts::Capabilities` 不保存携带密钥的服务 URL。`wmts::tileGrid()` 返回可供渲染的独立配置和 `metadataWarnings`：

- 正常 `_w` 的 TopLeftCorner 实际以 north/east 顺序返回，`_c` 为 latitude/longitude；接受可识别的顺序并报告 `TiandituNorthEastAxes`。公开 `TileMatrix` 仍保留原始两数。
- 部分 WGS84BoundingBox 使用米制数值，不能当作经纬度包络。渲染使用明确的全球投影范围、矩阵维数与瓦片尺寸，不从错误包络推导投影。
- 当前 ScaleDenominator 与 WMTS 标准 0.28 mm 像素约定不符；保留原值并报告 `DeclaredScaleDiffers`。采样比例由网格范围和维数推导，不让原始比例尺改变像素定位。
- `cva_w` 第 19 级突然广告 Geographic 原点与半高矩阵，与 CRS 矛盾。默认返回 `Unsupported`。调用者可明确设置 `GridOptions.maxZoom=18`，或 `skipIncompatible=true` 保留连续有效子集。第 19 级的实际内容和定位仍未验证，不替它生成假定矩阵。
- `cta_w` 的全部级别也广告 Geographic 原点与半高矩阵。实际成都第 8 级在 Mercator 列 202 / 行 105 返回预期地名，Geographic 行 42 透明；`cta_c` 行 42 正常。默认严格转换仍拒绝这份矛盾元数据。设置 `useTiandituCtaWMercatorOverride=true` 才对精确的 `cta/w` 已知模式修正有效行数，并报告 `TiandituCtaWMercatorOverride | IncompatibleMatrix`；原始 Capabilities 不变。此项证明寻址修正，不等于逐级内容验收。

默认遇到不兼容矩阵时不会静默切换 CRS。HTTP 200 或透明 PNG 可能是合法空内容，不能单凭透明响应断言最高级别可用或不可用。

## worker 与 owner 的接法

```cpp
// worker: transport already has verified TLS, a valid clock and bounded size.
providers::TiandituOptions options;
options.layer = providers::TiandituLayer::Vector;
options.projection = Projection::Geographic;
TileRequest metadata = providers::tiandituCapabilities(key, options);
ByteBuffer bytes;
Status status = transport.fetch(metadata, bytes);
wmts::Capabilities caps;
providers::TileProfile profile;
if (status == Status::Ok)
  status = wmts::parseCapabilities(bytes.data(), bytes.size(), caps);
if (status == Status::Ok)
  status = providers::tiandituProfile(key, caps, options, profile);
// Marshal profile/status to the Map owner task. Never log metadata.url.
// owner, only after a successful completion:
if (status == Status::Ok)
  status = map.setSource(profile.source, profile.grid);
```

`parseCapabilities` 默认至多 256 KiB、16 图层、16 矩阵集、每集 32 矩阵、32 级 XML 深度、单文本 512 字节；不载入 DTD/外部实体，不依赖 DOM。失败保留输出。`tiandituProfile` 检查图层、投影、Style、Format 和网格；`combineProfiles` 验证同网格，求公共级别/限界并保留每层 alpha。配置对象跨任务通过队列转移所有权，不让 worker 操作 Map 或 UI。

[17_TiandituLayers](../examples/17_TiandituLayers/README.md) 展示完整接法。它按操作获取所需元数据，单网络 worker 下载/解码，owner 提交图片并条带绘图；不会默认扫全服务。三维地名、三维地形与 Cesium 扩展属于[官方三维客户端](https://lbs.tianditu.gov.cn/docs/#/sanwei/)能力，不在 ESP32 二维栅格库执行范围。

## 验证

工作区根运行 `node tests/run_tianditu_wmts.mjs --sanitize`、`node tests/run_map_projection.mjs --sanitize` 和 `node tests/run_tianditu_layers_example.mjs --sanitize`；均接受 `--no-exceptions`。分别核验官方 XML/失败事务/限界，Geographic 独立像素参考/极点/日期线/手势/透明层与两种字节序，以及实际示例配置工厂的 14 种选择和三层组合。原 Mercator 渲染指纹仍为 `86693f7916090a0d`，另有完整 Native/Wire、触摸和缓存压力回归；主机测试不代表所有服务内容和设备帧率。

真实 standalone CMake configure/build/CTest 5/5 通过，含 WMTS、Geographic、Pin、示例配置工厂和旧渲染；[报告](../../nice_gfx_editor/results/2026-10-20-tianditu-services/wmts-cmake-host.json)保留公开源码前后哈希。工作区 `node tests/run_map_cmake.mjs` 使用本地工具重现，不访问网络、凭据、SDK 或串口；`CXX`、`CMAKE`、`NINJA` 可指定安装路径。
