# 天地图官方能力与 ESP32 对照

检索、只读实测日期：**2026-10-20**。范围为[官方首页](https://lbs.tianditu.gov.cn/)、[实际导航配置](https://lbs.tianditu.gov.cn/js/config.js)、[Web目录](https://lbs.tianditu.gov.cn/server/guide.html)，以及仍可访问的旧文档和地图目录链接的三维说明。文档存在、端点可用、库已实现、设备已验证是不同证据。本文不把 URL 构建成功当作完成接入。

## 许可、配额与安全

[授权说明](https://lbs.tianditu.gov.cn/authorization/authorization.html)要求许可key；[服务概述](https://lbs.tianditu.gov.cn/server/guide.html)说明账号调用配额。公开页没有足以承诺的统一现行单价/每秒/每日额度，按控制台账号及应用权限核对。免费注册/JS开源库免费不代表所有请求无限。旧静态图页的“频率无限制”不能覆盖账号配额政策。

库不分发key。ESP32需HTTPS、真实CA及有效时间；浏览器应用的key类型/域名限制单独配置，不能公开设备私有key。禁止记录Request.url、完整请求或原始drive XML：实测XML的`parameters/key`会回显凭据。测试fixture删除该完整块，且未保存带凭据URL。

## 地图服务

依据：[官方地图目录](https://lbs.tianditu.gov.cn/server/MapService.html)。每个二维图层都有经纬度`_c`和球面墨卡托`_w`，支持HTTPS、t0—t7、OGC WMTS GetCapabilities/GetTile。

| 能力 | HTTPS endpoint模板（不含key） | 返回/关键参数 | ESP32覆盖与边界 |
| --- | --- | --- | --- |
| 矢量底图 | `https://t0.tianditu.gov.cn/vec_{c或w}/wmts` | raster；LAYER=vec | 原有tianditu使用vec_w |
| 矢量注记 | 同域`cva_{c或w}/wmts` | 透明注记；cva | 原有vec_w+cva_w已实机验证 |
| 影像底图 | 同域`img_{c或w}/wmts` | raster；img | 复用PNG/JPEG解码 |
| 影像注记 | 同域`cia_{c或w}/wmts` | 注记；cia | 需与影像网格匹配 |
| 地形晕渲 | 同域`ter_{c或w}/wmts` | raster；ter | 不是数值高程查询 |
| 地形注记 | 同域`cta_{c或w}/wmts` | 注记；cta | 需与地形网格匹配 |
| 全球境界 | 同域`ibo_{c或w}/wmts` | 境界层；ibo | 不是行政边界矢量查询 |
| 三维地名 | `https://t0.tianditu.gov.cn/mapservice/GetTiles` | 官方Cesium扩展协议 | 当前ESP32不执行三维地球 |
| 三维地形 | `https://t0.tianditu.gov.cn/mapservice/swdx` | 官方Cesium地形协议 | 不能当成RGB565 raster层 |

GetTile参数：SERVICE=WMTS、REQUEST=GetTile、VERSION=1.0.0、STYLE=default、FORMAT=tiles、LAYER、TILEMATRIXSET、TILEMATRIX、TILEROW、TILECOL、tk。矩阵标识/级别/尺寸/限制以实际capabilities为准，不能只改URL的_w/_c。新增网格/七层实现见`TiandituWmts.h`、`TileGrid.h`和17号示例；最终测试以其记录为准。

[三维调用说明](https://lbs.tianditu.gov.cn/docs/#/sanwei/)明确Cesium1.108、天地图插件、浏览器window.Cesium依赖；本次只给平台边界，不伪造ESP32三维执行。

## V2搜索：七类与建议词扩展

endpoint：`https://api.tianditu.gov.cn/v2/search`，query为postStr=JSON、type=query、tk。依据：[官方V2文档](https://lbs.tianditu.gov.cn/server/search2.html)。

| 能力/queryType | 关键JSON参数 | 返回 | 公共库 |
| --- | --- | --- | --- |
| 普通1、纯地名7 | keyWord,mapBound,level；可specify/dataTypes/show | POI/统计/区域/线路 | search Normal/PlaceNames |
| 视野2 | keyWord,mapBound,level | 矩形内结果 | Viewport |
| 周边3 | keyWord,pointLonlat,queryRadius | POI及距离 | Nearby；冻结原点 |
| 多边形10 | keyWord,polygon，首尾闭合 | 多边形内POI | Polygon；有界点数 |
| 行政区内12 | keyWord,specify | 区内结果 | Administrative；不是边界服务 |
| 分类13 | dataTypes及specify/mapBound | 分类POI | Category |
| 统计14 | keyWord,specify | 行政区计数 | Statistics |
| 建议词4 | keyWord,mapBound,level | suggests/resultType4 | Suggestions；见差异说明 |

分页start0..300、count1..300；库限制窗口不超500。周边文档限10km；多边形受20点错误约束。当前附近厕所使用1/3/5km、3条/页。resultType1..5分开解析，坐标按经度,纬度转GeoPoint{纬度,经度}。

V2正文没有单列type4，但[官方LocalSearch](https://lbs.tianditu.gov.cn/api/js4.0/pages-class/service/LocalSearch.html)公开建议词功能。本次V2 type4实测200/resultType4/3条，**没有status**；属于JS文档有依据、V2正文未单列的实测扩展。V2 type5/6实测400，不假设所有旧类型可用。

## 其他设备HTTP服务

下表endpoint均以`https://api.tianditu.gov.cn`开头，省略私有tk。各项许可/配额按账号。当前主入口已真实只读验证，但不保证任意区域必有数据。

| 能力/endpoint | 参数 | 返回与公共解析 | 限制/官方依据 |
| --- | --- | --- | --- |
| 正编码 /geocoder | ds={keyWord:地址} | status0 JSON→location | 国内地址；[文档](https://lbs.tianditu.gov.cn/server/geocodinginterface.html) |
| 逆编码 /geocoder | postStr={lon,lat,ver:1},type=geocode | status0 JSON→formattedAddress/location | [文档](https://lbs.tianditu.gov.cn/server/geocoding.html) |
| 行政V2 /v2/administrative | keyword,childLevel0..3,extensions | status200 JSON→层级/中心/边界/建议 | [文档](https://lbs.tianditu.gov.cn/server/administrative2.html) |
| 驾车 /drive | postStr={orig,dest,mid?,style},type=search | **XML**→routes/steps/points/距离时间 | style0最快/1最短/2避高速；[文档](https://lbs.tianditu.gov.cn/server/drive.html) |
| 步行 /drive | 同上style3 | 同一XML契约 | 官方称步行，不称骑行 |
| 公交地铁 /transit | type=busline,postStr={startposition,endposition,linetype} | resultCode0/5 JSON→路线/分段/站点/几何 | 策略位1快/2少换乘/4少步行/8不坐地铁；[文档](https://lbs.tianditu.gov.cn/server/bus.html) |
| 公交线路详情 /transit | type=busline,postStr={uuid:线路ID} | JSON→lineInfo/route/stations/points | transitDetail；兼容实际小写字段 |
| 公交站详情 /transit | 同上uuid为站ID | JSON→station/关联线路ID | 同一transitDetail |
| 站点返程 /transit | postStr={lineUuid,stationUuid},type=busline | 实测为返程线路JSON | transitReverse；未知schema明确失败 |
| 静态图 /staticimage | center,width,height,zoom,layers；markers/styles/paths | **PNG**→owned bounded raw | [文档](https://lbs.tianditu.gov.cn/staticapi/static.html) |
| 静态图坐标转换 | 同一endpoint加pixLocation | **文本**x,y双竖线分隔→pixels | 不是PNG；覆盖物参数此时不生效 |

行政boundary实测为**WKT MULTIPOLYGON**，保留分开的闭合环，不把洞与岛强行连接填充。公交segmentLine实测为数组；库取首项作可视路径，Step.alternativeCount及id明确备选信息，完整决策可显式保留bounded raw。

单位依据：[RoutePlan](https://lbs.tianditu.gov.cn/api/js4.0/pages-class/service/RoutePlan.html)规定drive为公里/秒；[官方公交示例](https://lbs.tianditu.gov.cn/api/js4.0/newExample/BusLine/BusLine5.html)展示分/米。公共结果统一为米/秒。静态图文档限定宽高1..1024、zoom3..18、URL2048、标记≤26；parser检查PNG签名/尺寸，完整图像解码由decoder负责。

## 历史搜索与不可用端点

[旧文档](https://lbs.tianditu.gov.cn/server/search.html)仍可访问，导航已注释该入口。`https://api.tianditu.gov.cn/search`历史类型1/2/3/4/5/6/7/10，包括普通/公交建议词、公交起终站；旧type10是拉框，V2 type10是polygon。

本次旧/search的4/5/6全部HTTPS404。库保留legacySearch显式构建，Client返回真实HTTP失败，不自动降级、不制造结果；不能把历史文档当当前在线接通。V2 type4的成功单独列明。

## 数据API / WFS

[官方数据目录](https://lbs.tianditu.gov.cn/data/dataapi.html)当前可见2017版1:100万数据：交通LRRL/LRDL、水系HYDA/HYDL/HYDP、居民地设施RESA/RESP。行政BOUA/BOUL/BOUP已被HTML注释；重复水系条目只计一次。

官方列HTTP的gisserver.tianditu.gov.cn/TDTService/wfs；本次同路径**HTTPS**、现有固件CA、无key均验证通过。[实际capabilities](https://gisserver.tianditu.gov.cn/TDTService/wfs?service=WFS&request=GetCapabilities)默认2.0.0、98147字节；还广告AANP/AGNP/BOUA/BOUL/HYDA/HYDL/LRDL/LRRL/RESA/RESP，实际元数据与可见网页清单不同。广告包含Transaction等写操作，不等于获得写权限；本库仅实现授权范围的只读三项。依据同时参考[OGC规范](https://www.ogc.org/standards/wfs/)。

公共API：`wfsCapabilities()`、`wfsDescribe(typeName)`、`wfsFeatures(WfsOptions)`，固定已验证的WFS1.0.0，避免默认2.0.0的坐标轴歧义。GetCapabilities1.0实测21522B，DescribeFeatureType(RESP)1102B；成都小BBOX、maxFeatures2、outputFormat=application/json、srsName=EPSG:4326返回582B、2个真实Point，坐标落在请求范围内。BBOX参数明确经度/纬度，库要求非空BBOX、每轴≤5°、count1..100；后两项是设备内存策略，不是宣称服务官方上限。Transport仍须设置响应上限，单条线/面可能超限，明确返回TooLarge。

解析capabilities→layers/formats/operations/version，XSD→schemaFields，OGC异常→Provider及字符串code，GeoJSON→Feature的id/properties/points/分开的paths。支持Point/MultiPoint/LineString/MultiLineString/Polygon/MultiPolygon/GeometryCollection；2D绘图忽略有效第三维。多边形环单独描边，不臆测孔洞/岛屿填充；非经纬CRS、非标准几何、嵌套属性明确Unsupported。可配置字段、集合、节点/深度、总几何上限。

## 网页类与本地绘图边界

[JS4入门](https://lbs.tianditu.gov.cn/api/js4.0/guide.html)、[类参考](https://lbs.tianditu.gov.cn/api/js4.0/class.html)、[示例](https://lbs.tianditu.gov.cn/api/js4.0/examples.html)、[实际类目录](https://lbs.tianditu.gov.cn/js/initTree.js)核对如下。JS不是ESP32 C++ SDK；不在设备执行DOM/HTML5/SVG。

| 能力族 | 官方范围 | 当前设备库覆盖/缺口 |
| --- | --- | --- |
| 地图图层 | Map/TileLayer/TDT/WMS/MapType/LayerGroup、两投影 | raster/图层alpha/pan/pinch/fit已有；不直接执行JS/WMS类 |
| 控件 | Zoom/Scale/Copyright/OverviewMap/MapType/自定义 | UI可组合，不等于JS控件移植 |
| 覆盖物 | Label/Marker/Icon/InfoWindow、Polyline/Polygon/Rectangle/Circle/SVG、编辑拖动 | marker命中/route/track/circle/polygon已有；HTML窗/SVG/孔洞编辑等未完整实现 |
| 工具 | Mark/Polyline/Polygon/Rectangle/CircleTool、测距测面 | 球面距离/方位/围栏已有；交互编辑工具需应用实现 |
| 组件 | MarkerClusterer/CoordinatePickup/CloudMarkerCollection/GridlineLayer/PaintBrushTool | 坐标变换已有；聚合/海量点/格网/画笔无完整对应 |
| 交互 | 地图与覆盖物事件、键盘/鼠标/右键菜单 | View触摸/Map手势已有；浏览器事件机制不直接移植 |
| 服务包装 | LocalSearch/TransitRoute/BusLineSearch/DrivingRoute/Geocoder/AdministrativeDivision | 用前述HTTP构建/解析实现 |
| H5定位 | [Geolocation](https://lbs.tianditu.gov.cn/api/js4.0/pages-class/service/Geolocation.html)使用浏览器geolocation | 设备用独立GNSS，不能假装浏览器定位 |
| IP城市定位 | [LocalCity](https://lbs.tianditu.gov.cn/api/js4.0/pages-class/service/LocalCity.html) | 未见独立公开REST契约，不猜endpoint |
| 地图数据来源 | [DataSources](https://lbs.tianditu.gov.cn/api/js4.0/pages-class/service/DataSources.html) | 不把固定attribution当查询成功 |

符号标绘类目录还包含22种符号：CurveFlag、RectFlag、TriangleFlag、BezierCurve2/3/N、CardinalCurve、Arc、GatheringPlace、CloseCurve、RoundRect、Sector、DiagonalArrow、StraightArrow、DoveTailDiagonalArrow、DoveTailStraightArrow、DoubleArrow、PolylineArrow、BezierCurveArrow、CardinalCurveArrow、SectorSearch、ParallelSearch，以及对应Tool、HandDrawingTool、Control.militarySymbols。当前库没有这些专用工具/算法；可以显示外部生成的有界几何，但不能冒称兼容官方标绘。

[官方开源扩展](https://lbs.tianditu.gov.cn/api/js4.0/opensource/source.html)另有D3 SVG（GeoJSON/柱/饼）、HeatmapOverlay、CarTrack、BufferTool（turf）、ImageOverlay。已有TrackReplay/跟随不等于直接兼容JS扩展；热力、缓冲区、任意地理图片/GeoJSON仍需对应实现。

## 云控制台与移动端边界

[服务中心说明](https://lbs.tianditu.gov.cn/docs/)限定单位用户：≤5MB CSV/Shapefile ZIP/GeoJSON上传、地址/坐标/行政区空间化、编辑、行政设色/钻取/多时相、WMS/WMTS/矢量瓦片叠加、发布分享。说明没有公开CRUD契约；本次ESP32库不猜接口、不登录控制台上传/发布。首页遗留Android/iOS guide实测404，不把其他平台SDK当ESP32能力。

## 公共实现与证据

`TiandituServices.h/.cpp`构建Request，Client真实调用现有Transport；`adapters/TiandituParser.h/.cpp`处理各schema，未把JSON塞进瓦片Decoder。当前ESP32 SDK有cJSON；无cJSON的平台明确返回JSON Unsupported。纯IDF的CMake默认显式依赖IDF5内置json或IDF6常见managed espressif__cjson；自定义名称用NICE_GFX_MAP_CJSON_COMPONENT，缺少组件会报配置错误，不默默丢失JSON。明确不需要JSON时可设NICE_GFX_MAP_ENABLE_TDT_JSON=OFF。Arduino独立库沿用其SDK的include/link。

调用者需同时配置Transport下载上限≤Limits.responseBytes；列表/路线/文字/几何/节点/深度有界。同步fetch在worker，Map/UI owner仅采用成功结果。Network/Http/Provider/Parse/TooLarge/Unsupported/NoMemory分开；失败保留旧Result，界面要显示本次Report。cJSON分配失败和非法JSON均可能表现为Parse，底层Transport的NoMemory可独立识别。

| 本次真实请求 | 状态 | 格式/重点 |
| --- | --- | --- |
| 周边、统计 | 200/infocode1000 | JSON；厕所126总数3条 |
| 正逆编码 | 200/status0 | JSON |
| 行政/边界 | 200/status200 | JSON/WKT |
| drive最快/步行 | 200 | XML；样本0.72km55s/0.76km266s |
| 公交规划 | 200/resultCode0 | JSON；segmentLine数组 |
| 线路/站/返程 | 均200 | JSON；线路样本35站 |
| 静态图/像素 | 均200 | PNG/文本 |
| V2建议词4 | 200/resultType4 | 无status |
| 旧/search 4/5/6；V2 5/6 | 404；400 | 明确失败，不fallback |
| WFS capabilities1.0/2.0、schema | 均200无key | XML；现有CA有效 |
| WFS BBOX/count2要素 | 200无key | GeoJSON，2个真实Point |

`node tests/run_tianditu_services.mjs`默认UBSan，目前10707断言、19个真实schema；`--no-exceptions`同样10707项通过，`--no-json`的XML/像素可用与JSON Unsupported共7项通过。使用官方脱敏fixtures与真实Client/mock HTTP边界，测试时不访问网络或读取凭据。两新增CPP已通过实际S31/Arduino4.0RC1语法检查。`node tests/run_tianditu_cmake.mjs`使用CMake3.31.6验证IDF5/IDF6/自定义组件/显式禁用/缺失报错5种注册分支，10项通过；这不等于完整纯IDF固件构建。16号设备示例的最终编译/实机记录单列，host不能代替设备验证。证据见[service-validation](../tests/tianditu-service-validation.json)。
