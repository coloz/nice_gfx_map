# NICE_GFX_MAP 0.2.0

优先面向 ESP32 的轻量栅格地图扩展库，同时提供资源受限的 Wio Terminal 天地图示例。它把真实经纬度转换成地图坐标，管理瓦片和有界内存缓存，在底图上绘制位置、方向、标记、路线与历史轨迹，再交给 NICE_GFX 或 NICE_GFX 原生 UI 显示。天地图公开 HTTP 服务另有独立的请求构建、传输和有界解析接口。

库与 `nice_gfx`、`nice_gfx_editor` 分开维护，不修改 UI 资源 ABI。配套编辑器已通过 `gfx-map` 组件接入本库；现有工程也可用设备动态更新的 `gfx-media`，由应用发布地图图像。编辑器自动固件集成与手写 Wio 示例的硬件范围不同，详见各自说明。

## 本版范围

- C++17 通用核心：Mercator/Geographic 投影、平移、连续缩放、缩放锚点、视口瓦片调度、容量受限的缓存。
- 单层底图与透明叠加层；透明层保留 PNG alpha 后与下层混合，适合天地图底图与注记的组合。
- 当前定位与方向、跟随、标记、路线折线和有界历史轨迹；经纬度由应用输入。
- RGB565 输出；可按条带渲染，无须始终保留完整屏幕缓冲。
- ESP32 HTTPS 传输与 PSRAM 分配适配；可选 `PNGdec` 图片解码适配。
- NICE_GFX 直接绘图适配和 `View` 动态图片适配。
- 视口范围适配、圆点/Pin 标记及 ID/命中检测、米制圆和透明多边形。Pin 尖端对应经纬度，默认圆点保持兼容。
- RMC/GGA 定位解析、顺序路线引导、带迟滞的电子围栏和时间戳轨迹回放。
- SD / SD_MMC / LittleFS 只读瓦片适配，以及[应用场景示例](examples/README.md)。
- 天地图七种 WMTS 栅格图层、实际 GetCapabilities 解析、独立矩阵配置；墨卡托与经纬度网格不能混用。
- 天地图 V2 检索、建议词、正逆地理编码、行政区与边界、驾车/步行、公交、静态图及像素坐标服务，见 [16 服务示例](examples/16_TiandituServices/README.md)。
- 天地图自行车记录仪示例：异步瓦片加载、真实 GNSS、骑行仪表、SD CSV、GPX 路线导入，以及可选 ESP32 PNG/JPEG 混合解码适配。
- [Wio Terminal 天地图示例](examples/15_WioTerminalTianditu/README.md)：无需 SD 卡或 PSRAM，原始 256 像素 PNG 瓦片逐张下载、逐行显示，透明注记从屏幕回读合成；固定复用 48 KiB 压缩缓冲，支持五向键平移和按键缩放。SAMD 支持范围目前仅此 Wio Terminal 示例，不表示 ESP32 的网络、PSRAM 或录制适配可直接用于其他 SAMD 板。

库不运行 JavaScript、DOM、MapLibre GL；不提供向量 PBF 渲染、三维引擎、旋转地图、离线路径搜索或实时路况服务。地址与 POI 检索、在线道路规划由天地图 HTTP 服务提供，网络调用应放到 worker。方向箭头不会让地图道路和文字一起旋转。完整官方能力、当前接口与历史失效端点的区别见[天地图能力矩阵](docs/tianditu-capabilities.md)。

`PngDecoder` 仅支持 PNG；可选的 `codecs/Esp32RasterDecoder.h` 在 ESP32 上按内容签名区分 PNG/JPEG。PNG 仍由 PNGdec 解码并保留透明注记。ESP32-S31 / ESP32-P4 的 SDK 提供 `SOC_JPEG_DECODE_SUPPORTED` 时，JPEG 使用 `esp_driver_jpeg` 硬件引擎，直接生成驱动对齐的 PSRAM 原生 RGB565 瓦片，不分配 RGB888 中间图；其他 ESP32 使用 SDK `esp_jpeg` 软件实现。硬件路径接受 16–512 像素、边长为 16 倍数的方形 baseline RGB/YUV JPEG，避免 MCU 补齐尺寸影响缓存布局；不支持 JPEG 透明叠加层、灰度转 RGB 和渐进式 JPEG。`lastBackendName()` 与 `hardwareJpegTiles()` 可核对实际采用的解码器。单独使用硬件 JPEG 可包含 `codecs/Esp32HardwareJpegDecoder.h`，无需 PNGdec；原生 IDF 应用在其自身组件声明 `esp_driver_jpeg` 依赖。官方 API 与内存约束见 [ESP32-S31 JPEG 文档](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32s31/api-reference/peripherals/jpeg.html)。

天地图骑行示例使用混合解码器，兼容 JPEG 底图与透明 PNG 注记。其他示例若仍用 `PngDecoder`，不会自动获得 JPEG 支持。当前 Korvo 项目天地图 vec/cva 响应为 PNG，硬件 JPEG 不会加速这些 PNG，也不会通过 PNG 转 JPEG 增加处理步骤。主机边界/所有权测试及原创真机颜色夹具见 [JPEG 测试说明](tests/data/README.md)；主机 shim 不代表硬件性能结果。

## 天地图服务接入

[16 服务示例](examples/16_TiandituServices/README.md)提供完整可编译的 ESP32 worker。以下代码放在调用者自己的网络任务；`applicationKey`、`applicationRootCa` 和联网校时由应用提供：

```cpp
#include <NICE_GFX_MAP.h>
#include <adapters/Esp32.h>
#include <adapters/TiandituParser.h>
using namespace nice_gfx_map;
namespace tdt = nice_gfx_map::tianditu;

tdt::Limits limits;
limits.responseBytes = 128 * 1024;
HttpOptions options;
options.caCertificate = applicationRootCa;
options.maxResponseBytes = limits.responseBytes;
options.allocator = esp32PsramAllocator();
Esp32HttpTransport transport(options);
tdt::ResponseParser parser;
tdt::Client client(transport, parser, limits);
tdt::SearchOptions query;
query.mode = tdt::SearchMode::Nearby;
query.keyword = "公共厕所";
query.center = {30.6587, 104.0646};
query.radiusMeters = 3000;
query.count = 3;
tdt::Result result;
const auto report = client.fetch(tdt::search(applicationKey, query), result);
// report.ok() 后，通过队列交给 UI owner；在那里读取 result.places 并更新地图。
```

下载与解析使用相同响应上限；`Limits` 另限制条目、路线、几何、文字、JSON 节点和深度。`Report` 区分 HTTP、网络、服务错误、超限和解析失败；失败保留旧 `Result`，应用应明确展示本次错误。禁止打印 `Request.url` 或原始响应，驾车 XML 会回显 key。默认不保留 raw；静态图片的有界原始数据由图像解码器处理。

`geocode` / `reverseGeocode`、`administrative`、`drive`、`transit` / `transitDetail` / `transitReverse` 和 `staticMap` 复用同一 `Client`。公共数据 API 的 `wfsCapabilities` / `wfsDescribe` / `wfsFeatures` 为只读 WFS 1.0.0；要素查询必须提供小范围 BBOX 和条数上限，返回 GeoJSON 几何与属性。端点与格式差异、旧版接口实测失效情况见[能力矩阵](docs/tianditu-capabilities.md)。

七种 WMTS 栅格图层及 `_w` / `_c` 实际矩阵解析见 [17 图层示例](examples/17_TiandituLayers/README.md)。`TileProfile` 同时包含 `source` 与 `grid`，必须一起交给 `Map::setSource(source, grid)`；不能只把旧 URL 中的 w 改为 c。旧的 `providers::tianditu(key)` 和 `setSource(source)` 保留墨卡托行为。

## 安装与构建

### Arduino / PlatformIO

1. 将本目录作为 Arduino 库安装，或放进 PlatformIO 项目的 `lib/nice_gfx_map`。
2. 同时安装本项目配套的 `NICE_GFX` 库。当前没有填写可供自动下载的仓库地址，需本地安装。
3. 在线 PNG 示例另需安装 Larry Bank 的 `PNGdec`，适配器以 1.1.2 版本核对。`codecs/PngDecoder.h` 是显式启用的适配头；只用通用核心或过程生成地图时不需要它。
4. 选择实际 ESP32 开发板和正确的 PSRAM 类型，在板级设置中启用 PSRAM，并核对显示屏引脚。

推荐 ESP32-S3 等带 **8 MiB PSRAM** 的配置。小视口、较少瓦片也能采用更小预算，但能否运行取决于实际空闲内存、图层、HTTPS/TLS 与 UI 缓冲，不能仅凭标称 PSRAM 容量保证。

`library.properties` 的发布 URL 留空；元数据没有声明尚未核实的许可证或远程仓库。项目发布前应由维护者确认这些信息。地图服务、数据和可选第三方依赖各自保留其条款与归属要求。

### 原生 ESP-IDF

把 `nice_gfx_map` 与 `nice_gfx` 放进应用的 `components` 目录。组件 CMake 注册通用核心、ESP32 传输和天地图服务解析，依赖 `nice_gfx`、`esp_http_client`、`esp-tls`、`esp_psram`、`esp_timer`、`heap`，不要求 Arduino。

```text
your_app/
  components/
    nice_gfx/
    nice_gfx_map/
  main/
```

天地图 JSON 服务默认启用 cJSON 显式组件依赖：IDF 5 通常使用内置 `json`，IDF 6 的当前验证环境使用 managed `espressif__cjson`。CMake 自动识别常见目录；自定义组件名通过 `-DNICE_GFX_MAP_CJSON_COMPONENT=组件名` 指定。IDF 6 应先在应用中声明/准备 `espressif/cjson` 组件。未找到依赖会明确报配置错误；只需瓦片、XML或像素文本解析时，可用 `-DNICE_GFX_MAP_ENABLE_TDT_JSON=OFF`，JSON 服务会返回 `Unsupported`。Arduino 独立库使用其 SDK 已有的 cJSON include/link，不受此 CMake 选项影响。

应用负责 Wi-Fi、联网校时、根证书和任务。PNGdec 的 Arduino 发行包默认包含 `Arduino.h`，不能直接把它作为纯 IDF 组件使用；纯 IDF 应用需要配置其非 Arduino 移植和一致的行缓冲宏，或者实现自己的 `Decoder`。本库的核心、HTTP 传输及显示/UI 适配本身不要求 Arduino。渲染与 UI 适配都是头文件，使用方按需包含。

### 主机核心

主机构建包含 ESP32 适配器的无平台回退实现，不依赖 ESP32 SDK、NICE_GFX 或 PNGdec；分配器使用主机内存，HTTPS 传输返回 `Unsupported`。可用于投影、调度和像素合成验证。

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

源码附带的主机检查也可通过下列命令运行，脚本会查找 C++17 编译器，或接受显式路径：

```sh
node scripts/test.mjs
node scripts/test.mjs --compiler=/path/to/compiler
node scripts/test.mjs --pngdec=/path/to/PNGdec
node scripts/test.mjs --sanitize --nice-gfx=/path/to/nice_gfx
node scripts/check-esp32.mjs --help
node scripts/verify-examples.mjs
node scripts/generate-offline-demo.mjs
node scripts/test-raster.mjs
python -m unittest discover -s examples/14_TiandituBikeRecorder/tools -v
```

脚本分别运行几何/管线、地图、导航/NMEA/围栏/回放、覆盖物、离线 FS 和骑行数据测试，并检查关闭异常时的核心编译；若找到外部 PNGdec，还会验证真实解码器和附带离线 PNG 的完整读取/解码/合成流程；若找到配套 `nice_gfx`，会编译真实 View 与显示实现，核对条带输出、动态图片发布及资源释放。测试图输出到 `.build/map-preview.png`。CMake/CTest 注册通用测试，不自动安装或构建 PNGdec。

2026-10-19 验证：主机地图测试通过 **1328** 项检查，覆盖物通过 **10795** 项检查，离线 FS 通过 **20** 项检查，骑行数据通过 **77** 项检查，SD 记录器通过 **50** 项检查，真实 PNGdec 1.1.2 通过 **64** 项检查，附带离线 PNG 的读取/解码/合成通过 **72** 项检查，真实 NICE_GFX View/显示适配通过 **2432** 项检查，并通过 UndefinedBehaviorSanitizer 与禁异常核心编译。GPX 工具的 **10** 项 Python 单元测试通过。几何、图片所有权、provider、加载管线及导航/NMEA/围栏/回放另有独立测试。示例校验确认应用场景共 11 个、总计 14 个示例，以及 81 张离线演示 PNG（合计 33244 字节）。

本轮硬件 JPEG 使用[原创四色夹具](tests/data/README.md)单独验证分派、输出与颜色；主机 shim 不代替芯片实际解码或性能测量，天地图 PNG 仍使用 PNGdec。

使用本机 ESP32-S3 GCC 14.2.0、Arduino-ESP32 3.3.12 / 配套 ESP-IDF 5.5.5 头文件，对全部 8 个库实现文件、显示/UI/文件系统/混合解码适配、示例 01/02 与 04–14、单独覆盖引脚宏，以及禁异常 Map/Navigation/Nmea 与骑行示例做了 **27/27** 项编译检查。可运行 `node scripts/check-esp32.mjs` 重现；异机使用 `--help` 查看工具链路径选项。输出报告为 `.build/esp32/compile-results.json`。

上述 2026-10-19 ESP32 检查仅生成目标文件，当时没有执行固件链接、烧录、服务 Key 联网请求或实屏验证，也未运行完整 CMake/CTest 构建；实际通过的是上述 Node 驱动的编译与运行流程。后续真机和新增接口证据见对应示例与报告。示例 03 是依赖用户导出工程的集成模板。

2026-10-20 新增 WMTS **754**、Geographic 投影 **829651**、Pin 图标 **1001674**、实际示例配置工厂 **257** 项主机检查，均通过 UBSan 与禁异常编译运行；原 Mercator **55515** 项检查的指纹 `86693f7916090a0d` 保持一致，完整 Native/Wire **14855182** 项、实际 View 手势 **358** 项和缓存压力 **1151** 项回归通过。工作区根的 `tests/run_tianditu_wmts.mjs`、`run_map_projection.mjs`、`run_map_pin.mjs`、`run_tianditu_layers_example.mjs` 可重现新增检查；真实 CMake configure/build/CTest **5/5** 通过，含这四项与旧渲染回归，见[报告](../nice_gfx_editor/results/2026-10-20-tianditu-services/wmts-cmake-host.json)，可用 `node tests/run_map_cmake.mjs` 重现。实际 CMake 的 IDF 5/6 JSON 组件依赖选择另通过 5 种分支/10 项检查。17 示例已完成 ESP32-S31 完整目标链接，具体二进制与公开源码哈希见其 README；设备内容和显示结果需以真机记录为准。

## 先运行哪个示例

应用场景为 **04 GNSS 定位、05 骑行路线引导、06 徒步航点、07 车队监控、08 电子围栏、09 配送停靠、10 SD 离线地图、11 天气叠加、12 农业地块、13 轨迹回放、14 天地图自行车记录仪与导航**。各例具有独立状态、串口操作、预期结果和真实数据接入说明，见 [examples/README.md](examples/README.md)。原来的三个入门/集成示例保留。

04–13 中九个示例默认使用原创过程测试底图，SD 例读取附带的 81 张原创 PNG；它们不是默认联网的真实地图。GNSS 例可以接真实 UART 定位，业务数据与底图来源各自配置。接在线地图时应关闭示例自动过程瓦片加载，并使用真实 Transport/Decoder；不能只换 provider URL 后继续调用过程解码器。**14 示例独立采用真实天地图加载管线与 GNSS，需填写配置，无模拟回退。**

| 示例 | 额外条件 | 用途 |
| --- | --- | --- |
| `01_ProceduralMap` | NICE_GFX、ESP32 PSRAM、320×240 ILI9341 | 无网络、无密钥。通过 Transport → Decoder → TileLoader 生成 256 像素街区测试图，展示模拟 GNSS、路线、轨迹、平移和缩放。 |
| `02_OnlineTiles` | Wi-Fi、服务账号、真实根 CA、有效系统时间、PNGdec | 在 Geoapify、MapTiler、天地图配置间切换，演示一次处理一张瓦片的同步 HTTPS 加载。 |
| `03_UiMap` | 自己从设计器导出的 `ui.h`、`Hardware.h`、`TouchAdapter.h` | 动态图片集成模板，使用过程生成底图。不附完整硬件工程，缺少这些导出文件时会明确报编译错误。 |
| [`14_TiandituBikeRecorder`](examples/14_TiandituBikeRecorder/README.md) | ESP32-S3、8 MiB PSRAM、SD、GNSS、天地图 key/根 CA、Wi-Fi | 真实天地图与骑行仪表、CSV 记录、导入骑行路线的航点导航；独立网络任务。 |
| [`15_WioTerminalTianditu`](examples/15_WioTerminalTianditu/README.md) | Wio Terminal、定制 TLS 无线固件、rpcWiFi、PNGdec、JPEGDEC、Wi-Fi、天地图 key；无需 SD/PSRAM | 真实天地图与透明注记、可见瓦片逐行绘制、按键平移缩放；已通过 Wio 真机联网、显示及证书校验测试。 |
| [`16_TiandituServices`](examples/16_TiandituServices/README.md) | ESP32、NICE_GFX、PNGdec、Wi-Fi、服务端 key、根 CA；默认 S31 Korvo / 可选 ILI9341 | 搜索、编码、行政边界、驾车/步行、公交与静态图；实际 HTTPS、单 worker、白名单摘要及真实结果叠图。 |
| [`17_TiandituLayers`](examples/17_TiandituLayers/README.md) | ESP32、NICE_GFX、PNGdec、Wi-Fi、服务端 key、根 CA | 七种官方 WMTS 图层、w/c 投影、按需元数据、底图/注记/境界组合。 |

前两个示例的 ILI9341 接线为 CS=5、DC=2、RST=4、SCK=18、MOSI=23、MISO=19，旋转后为 320×240。它只是示例接线，不是所有 ESP32 板的通用定义。请以实际板卡和屏幕为准。

示例 01 串口指令：`w/a/s/d` 平移并退出跟随，`+/-` 缩放，`f` 恢复跟随模拟位置。测试街区不是实际地理道路。

## 从请求到绘制

0.2.0 的复用能力：

| 接口 | 用途 |
| --- | --- |
| `fitBounds` / `fitPoints` | 适配路线、车队或航点的视口，支持跨日期线的最小经度包络。 |
| `markerAt` / `Marker::id` | 把屏幕命中结果连接到车辆、航点等业务对象。 |
| `setCircles` / `setPolygons` | 绘制围栏、地块、服务范围及透明状态区域。 |
| `setLayerOpacity` / `setLayer` | 调整叠加透明度，或只刷新变化图层，保留底图缓存及其他图层请求。 |
| `NmeaParser` | 有界流式解析校验和正确的 RMC/GGA，向应用提供定位状态。 |
| `RouteNavigator` | 沿应用给定的路线顺序到点、计算剩余距离和偏航状态。 |
| `GeoFence` | 圆形或单环多边形围栏，迟滞控制 ENTER/EXIT 事件。 |
| `TrackReplay` | 时间戳位置插值、方向、终点及循环采样，控制器可实现暂停、倍速和 seek。 |
| `FsTileTransport` | 从已挂载的 Arduino FS 读取 `root/sourceId/layer/z/x/y.png`，不做网络或生成替代瓦片。 |

球面距离/方位采用平均地球半径近似；地图圆的视觉半径采用当前投影的局部米制比例（Geographic 的东西/南北比例不同）。多边形围栏限单环、简单且可用局部半球投影表达的区域；复杂/退化输入会被拒绝。RMC/GGA 中缺失的速度、方向、海拔保留上次值，当前解析结果不带逐字段年龄标记。以上接口的无效输入及容量上限以公开头文件为准。

```text
定位/GNSS、按键、触摸
        │ setLocation / panBy / zoomAt
        ▼
      Map ── nextRequest ──► Transport ──► Decoder
       ▲                        HTTPS       PNG → RGB565 + alpha
       └──────── submit / fail ◄─────────────┘
        │
        ├── renderRegion → 小条带 → NICE_GFX 屏幕
        └── publishToUi → 完整视口图 → View 的动态媒体组件
```

核心 `Map` 不进行 I/O。服务 URL、下载、压缩数据、解码和屏幕传输都在独立接口中；因此相同调度器既可用于线上地图，也可用于应用自行实现的文件源。

### WMTS 图层与投影

`providers::TiandituLayer` 覆盖 vec、cva、img、cia、ter、cta、ibo 七类官方栅格层。`tiandituCapabilities()` 构造按需元数据请求，worker 通过现有 Transport 获取后交给有界 `wmts::parseCapabilities()`；`tiandituProfile()` 输出独立 `TileProfile{source,grid}`，`combineProfiles()` 求同投影各层的共同矩阵范围并保留 alpha。owner 调用 `mapView.setSource(profile.source,profile.grid)`，`sourceGrid()` 可读当前配置。原单参数 `setSource(source)`、旧投影函数与 `TileSource::url(key)` 继续使用 Mercator；原 MapConfig、TileSource、SourceLayer 字段布局和 Map pimpl 大小保持不变。

`_c` 使用真正的 Geographic 经度/纬度等角网格，第 1 级宽 2、高 1；`_w` 为 Mercator 方形网格。经纬度、视口、平移/缩放锚点、fit、覆盖物和瓦片行列使用相同投影，不以换 URL 代替投影。原始能力中的矩阵标识、行列限界、CRS、原点顺序和比例尺可检查；冲突默认拒绝。`cva_w` 第 19 级尚未验证且元数据矛盾，示例限 1–18；`cta_w` 需要明确启用有告警的已知元数据修正，原响应仍保留。完整表、证据、限制与调用片段见 [WMTS 契约](docs/tianditu-wmts.md)，设备演示见 [17 全图层示例](examples/17_TiandituLayers/README.md)。

### 初始化

```cpp
#include <NICE_GFX_MAP.h>
using namespace nice_gfx_map;

MapConfig config;
config.width = 320;
config.height = 240;
config.cacheBytes = 4 * 1024 * 1024;
config.maxCachedTiles = 32;
config.maxInFlight = 2;
Map mapView(config);

// key/token 由应用安全配置，不要提交进源码或可分享的设计器工程。
TileSource source = providers::geoapify(myKey, "osm-bright");
Status status = mapView.setSource(source);
if (status == Status::Ok) status = mapView.setCenter({30.6586, 104.0648});
if (status == Status::Ok) status = mapView.setZoom(14);
```

示例采用 `mapView` 变量名，避免与 Arduino 的全局 `map(...)` 数值映射函数重名。

检查每次操作的返回值；不要假定设置成功。切换数据源会清理旧缓存并取消旧请求，不能混用不同 provider/style 的同名 `{z}/{x}/{y}`。

### 交互与业务状态

地点可使用水滴 Pin 图标：`Marker place{point, 0xf800, 10, placeId, MarkerShape::Pin};` 后在 owner 调用 `setMarkers(&place,1)`。GeoPoint 对应尖端，`radius` 是圆头的像素半径，图标宽 `2r`、高 `3r`；中心圆孔透明，保留底图/路线。图标随投影、平移与缩放定位，像素尺寸保持固定。`markerAt` 按水滴外形及像素 tolerance 命中，中心圆孔也可选中；后加入的标记优先。原四字段 aggregate 默认仍为 `MarkerShape::Circle`，圆点像素保持一致。新增末尾 shape 字段后，使用方应随库头文件重编译。工作区根 `node tests/run_map_pin.mjs --sanitize` 验证两投影、命中、透明孔、条带/ROI 与 Native/Wire 一致性。

```cpp
mapView.setLocation({latitude, longitude}, headingDegrees);
mapView.setFollow(true);
mapView.appendTrack({latitude, longitude});

mapView.setFollow(false);        // 用户主动浏览时，由应用明确退出跟随。
mapView.panBy(deltaX, deltaY);   // 地图内容跟随此屏幕像素位移。
mapView.zoomAt(15, touchX, touchY);
```

- `GeoPoint` 的字段顺序为 **latitude、longitude**；部分 HTTP API 的中心参数是 longitude、latitude，不要混淆。
- Web Mercator 显示纬度限制约为 ±85.05112878°；经度跨日界线需保持核心的归一化和世界环绕规则。
- 本库不隐式执行 WGS84、GCJ-02、BD-09 坐标转换。GNSS、服务数据和自定义覆盖物必须使用相同的坐标约定；`_w`/Web Mercator 图层也不等于替你转换所有来源坐标。
- `setRoute()` 显示应用传入的路线；在线算路使用独立的 `tianditu::drive()` / `transit()` 服务，在 owner task 将成功结果交给地图。
- 标记、路线和轨迹受 `MapConfig` 的数量上限约束。应用需决定采样间隔、轨迹抽稀和状态保存方式。

### 简单同步加载

```cpp
TileLoader loader(transport, decoder, []() -> uint32_t { return millis(); });
Status status = loader.loadNext(mapView, millis());
```

每次最多处理一张瓦片。`NotFound` 表示当前没有符合条件的任务，`Busy` 表示在途名额已满；它们通常不是业务故障。`NetworkError`、`DecodeError`、`TooLarge`、`NoMemory` 等必须处理，可用 `statusName()` 记录状态。不要输出包含密钥的请求 URL。

`fail(request, Status::NoMemory, now)` 会在原有延迟重试前有界回收屏外已就绪瓦片及其派生合成图，优先回收无需作为缩放回退的旧图。当前可见瓦片和在途请求不受影响，配置预算不变。这用于共享分配器的连续块不足：仅重复同一解码而保留全部旧图会持续失败；若没有可回收的屏外瓦片，应用仍需释放其他资源或处理分配失败。

可选第三个参数是当前时钟函数；同步网络加载建议传入 `millis()`，让失败重试退避从下载/解码结束时计时。无时钟函数时使用 `loadNext()` 传入的时间，适合快速同步源和确定性测试。

**一次一张不等于非阻塞。** 一次 DNS、TLS、下载或解码仍可能耗时数秒；示例 02 不保证触摸实时响应。`timeoutMs` 是尽量覆盖总传输的期限，并会应用到阻塞 I/O；它不是严格实时上界，也不能当作稳定帧预算。

### 响应式加载

实际产品应让 UI/地图归一个 owner task 管理：

1. owner task 调用 `nextRequest()`，把返回的独立 `TileRequest` 送到工作队列。
2. worker 使用自己的 `Transport`、`Decoder` 与缓冲，执行下载和解码。
3. worker 把请求、`Status`、可移动的 `TileImage` 送回结果队列。
4. owner task 调用 `submit()` 或 `fail()`，并继续处理输入和绘制。

`TileImage` 和 `ByteBuffer` 都不可复制，可移动传递。不要在 FreeRTOS 原始按字节复制队列里直接放含 `std::string`、所有权指针或 C++ 容器的对象；可用队列传递明确所有权的指针，或使用正确构造/移动的 C++ 队列封装。结果丢弃时也要销毁对象，释放图像。

不要从 ISR、网络回调或多个任务并发操作同一个 `Map`/`View`。旧视口或旧数据源结果应仍携带原请求，让 `submit()` 验证 request ID / generation；取消后不能拿新请求覆盖旧请求的身份。暂停接收新任务、处理已完成结果并清理 worker 后，才销毁任务上下文和分配器上下文。

## 两种显示方式

### 直接绘制 NICE_GFX

```cpp
#include <adapters/NiceGfx.h>
uint16_t scratch[320 * 8];
Status status = NiceGfxRenderer::draw(mapView, gfx, 0, 0,
                                    scratch, sizeof(scratch) / sizeof(scratch[0]));
```

适配器使用 RGB565 条带绘制，scratch 的容量单位是**像素**。直接绘制路线不需要额外保存完整地图视口；瓦片缓存、解码器和屏幕驱动仍会占内存。可依据 `revision()` 决定是否需要重画，避免静止地图持续刷屏。

`Map::renderRegion(x, y, width, height, pixels, capacityPixels, stride)` 保持原生 RGB565 `uint16_t` 颜色语义。需要高字节在前的显示/UI 存储时，可改用相同参数的 `renderRegionWire()`，在最终绘制写入时融合字节序转换，避免另做整幅图交换。两个入口的区域、裁剪、透明度、覆盖物和采样结果相同；解码瓦片与合成缓存始终保存 Native RGB565。它们都只渲染，不下载、不分配瓦片，也不修改 Map。

`capacityPixels` 与 `stride` 的单位均为像素；`stride=0` 表示紧凑的 `width`，否则至少为 `width`，容量至少覆盖 `(height-1)*stride+width`。输出区域必须完整位于地图视口内，像素指针满足 `uint16_t` 对齐。Wire 缓冲应按高字节在前的格式交给消费者，不能把其中的 `uint16_t` 数值直接当作 Native 颜色处理。

### 显示在现有原生 UI 中

配套 `nice_gfx_editor` 已提供 `gfx-map` 地图组件，可在组件库搜索“地图”添加，配置中心、缩放、定位、标记和路线，并导出带地图调度的 ESP32 Arduino 工程。编辑器使用离线示意预览；真实天地图 / XYZ 在设备端加载。完整配置与当前硬件范围见 [编辑器地图组件说明](../nice_gfx_editor/docs/map-component.md)。

`gfx-map` 在原生 UI 中编译为动态 Media，地图实例仍由本扩展库维护。完整导出生成 `MapRuntime.h`、`MapConfig.h` 与本地 `MapSecrets.h`，应用通过 `ui_maps::get("map")` 更新位置和覆盖物。

手动集成动态图片时，设计器页面先放置：

```html
<gfx-media id="map" data-media-dynamic="true"
           style="width:320px;height:200px"></gfx-media>
```

应用中设置相同地图视口尺寸，加载导出的 UI 后调用：

```cpp
#include <adapters/UiImage.h>
#include <adapters/Esp32.h>
Status status = publishToUi(mapView, ui, ui_config::id("map"), esp32PsramAllocator());
```

这个入口合成完整视口图像，然后通过现有动态图片能力发布；它不让固件解析 HTML，也不新增地图节点。原生 UI 可能保留上一幅图片和在途帧快照，所以发布瞬间的峰值内存高于单张视口图。

持续发布时可保留一个 `UiImagePublisher`，复用三个有界像素槽：

```cpp
nice_gfx_map::UiImagePublisher publisher; // 生命周期覆盖连续发布。
Status status = publisher.publish(mapView, ui, mapId, esp32PsramAllocator());
// 显式 Wire decorator：publisher.publishWire(mapView, ui, mapId, allocator, decorate, context);
```

`publish(map, ui, id, allocator, decorate, context)` 没有 decorator 时自动使用 `renderRegionWire()`；提供 decorator 时保留旧契约：回调同步读写 Native RGB565，回调后再转换成 Wire。显式 `publishWire(...)` 的回调则读写已经编码的 Wire 像素；例如 `TileCanvas::attach` 必须指定 `nice_gfx::PixelOrder::Wire`。两类回调类型均为 `void(TileImage&, void*)`，只允许在调用期间绘制，不能保留或移走图像，也不能在回调里修改 Map/View。原有一次性 `publishToUi()` 及其 Native decorator 契约保持不变。

发布后的 Image 是不可变 lease。只有 View 状态、页面缓存和所有在途快照均释放某槽的 lease 后，发布器才会复用该槽；三个槽都被借用时返回 `Busy`，不会覆盖旧画面。lease 保持银行存活，即使发布器已经销毁；最终释放可来自其他任务。`publish()`、`publishWire()`、`matchesUi()`、`trimUnused()` 以及发布器的创建/销毁仍应由同一 View owner task 管理，分配器 context 必须覆盖所有在途 lease 的生命周期。

`matchesUi(ui, id)` 通过 `View::hasImage` 检查当前有效媒体是否仍使用发布器最新的同一 Image 资源，不比较像素内容。应用只有在地图 revision 未变且该检查为真时才能跳过发布；清空、恢复默认图或替换其他图片后必须重新发布。最新发布图像本身也保留一个 lease。

后台地图可调用 `trimUnused()` 回收闲置槽的像素，返回释放的字节数；它只处理没有任何 lease 借用的槽，保留 latest 图像、所有活跃/在途引用。转场快照可能稍后才释放，因此可在后续 owner 轮询再次回收。活动地图通常保留缓冲池以复用连续拖动，切回后台地图后按需重新分配；此操作不清空 Map 瓦片缓存。

新增搜索前的 ESP32-S31 Korvo 优化固件已完成 COM10 烧录、真实天地图联网、Wire 对照与完整 150 ms 淡入验收，见[历史性能实测](../nice_gfx_editor/results/2026-10-20-korvo-navigator/device/PERFORMANCE.md)。真实导航回放推进 106.630 秒后返回地图并缩放，瓦片失败和显示错误均为 0；采样 PSRAM 总空闲最低 2,201,932 字节、最大连续块最低 1,638,388 字节，详见[历史内存复测](../nice_gfx_editor/results/2026-10-20-korvo-navigator/device/MEMORY-REGRESSION.md)。这些是特定配置下的采样结果，不代表屏幕 FPS 或无限运行保证。该历史版本已获用户实际操作确认；新增搜索、Pin 与完整天地图服务的设备验证见[本轮结果](../nice_gfx_editor/results/2026-10-20-tianditu-services/)。

地图对象和真实定位状态留在应用，不要随切页销毁。目标媒体不存在于当前页面时，保留状态，在 `RouteChange` 进入地图页后重新发布。更新地图图片应按 `revision()` 和目标帧率节流；示例 03 限制为每秒最多 5 次。这个数值是示例策略，不是实板性能保证。

`TouchPan` 提供无分配的单指拖拽，包含 4 像素起拖阈值、区域内按下捕获、区域外继续拖动，以及拖动后抑制点击。首次拖动会通过 `panBy()` 退出定位跟随；点击定位图标时调用 `setFollow(true)` 恢复。坐标须先转为地图视口内坐标，并由 UI 命中检测优先处理地图上方的按钮；所有调用仍在地图 owner task 中执行。

```cpp
nice_gfx_map::TouchPan drag;
// 地图命中后按下：drag.begin(mapView, {localX, localY});
// 已捕获的指针移动：drag.move(mapView, {localX, localY});
// 松开：if (drag.end()) suppressClick = true;
// 切换页面或触摸被取消：drag.cancel();
```

`PinchGesture` 提供双指连续缩放与中点平移：在地图视口内用 `begin(mapView, a, b)` 捕获两点，后续以 `move(mapView, a, b)` 更新，结束或取消时调用 `end()` / `cancel()`。默认起始间距至少 12 px；捕获后触点可以移出视口。起始双指中点对应的地理位置始终跟随当前中点，距离比例控制 `viewZoom()`，操作会退出定位跟随，并限制在来源支持的缩放范围内。两点交换顺序不改变计算结果；跟踪稳定触点 ID、UI 命中检测、消费剩余触点和抑制点击由调用方负责。

`Map::viewZoom()` 是连续缩放值，`setViewZoomAt(double zoom, double x, double y)` 围绕指定屏幕锚点更新它；`zoom()` 仍返回瓦片请求使用的整数层级，`setZoom()` / `zoomAt()` 恢复整数缩放。目标层级缺图时，渲染器按图层缩放复用缓存中的父级或邻近旧层级真实瓦片，保持透明注记、路线和定位覆盖物；可复用数据未覆盖的区域显示缺图背景，不会合成道路。这个回退不启动额外请求，也不在渲染时分配瓦片。

编辑器的 GT1151 固件导出已自动接入上述两种手势：同一地图内容区域内的两点才可启动双指操作，叠放按钮优先由原生 UI 命中；完整报告只读取一次，依据触点 ID 跟踪，`Frame.count` 定义有效触点，压力为 0 也有效。双指捕获取消普通单指输入，并持续消费触点直到全部释放，避免单指残留触发点击；错误或切页取消手势。只输出单点的其他触摸驱动仍保留原来的单指行为。手写集成也必须在地图 owner task 中处理完整触摸帧，并对下载请求与图像发布节流。

多个页面显示同一数据源时，可以用 `activeMap.takeCacheFrom(inactiveMap)` 移动解码好的瓦片缓存，避免页面切换后再次下载与解码。仅接受相同瓦片尺寸、URL 模板与 alpha 图层布局；不透明度、地图中心和业务覆盖物可以不同。该操作优先接收当前视口中的瓦片，遵守接收方缓存大小和数量限制，保留接收方正在加载的请求，然后清空旧地图缓存并取消旧请求；迟到的旧结果会返回 `Canceled`。不会复制像素或在多个地图之间共享可变所有权。

地图渲染针对不透明 RGB565 底图使用整行复制，跳过被底图完全覆盖的背景写入，并跳过全透明注记像素。路线和轨迹在输入时投影，线条的笔触跨度按线段复用，降低全屏与条带重绘成本。优化保留原始 RGB565 混合、透明度、跨日期线和条带裁剪结果。工作区的 `tests/run_map_performance_touch.mjs` 可运行像素一致性、触摸手势、缓存迁移和主机微基准；主机时间不代表具体硬件的帧率。

内存充足的设备可额外设置 `MapConfig::compositeCacheBytes`，例如 2 MiB，把已加载完成的底图与注记预先合成为 RGB565 瓦片。平移与导航重绘直接复制合成结果，避免每帧重复读取 PSRAM 中的 alpha 注记并混合。该预算默认是 0，独立于 `cacheBytes`，因此总图像预算为二者之和；原始底图与 alpha 注记仍保留。合成图沿用底图的分配器，ESP32 PSRAM 底图不会误用内部堆分配大块合成图。首次合成在提交瓦片时进行；改变图层或透明度会使合成缓存失效并按需重建；内存不足时继续采用原来的逐层绘制。渲染调用自身不分配或修改缓存。

`cacheStats()` 的 `compositeTiles`、`compositeBytes`、`compositeBudget` 和 `compositeBuilds` 分别给出合成缓存数量、占用、独立预算和累计构建次数。页面迁移仅在各层透明度与实际输入像素所有者均匹配时直接移动合成图，避免重复合成；否则依据迁入的原始瓦片重建。

原生 UI 的页面缓存和动态图像区域修复保留项目配置的转场与组件动画；Korvo 导航项目保留完整 150 ms 淡入淡出。ESP32-S31 在配套 SDK 支持 PPA、两页缓存及显示传输条件满足时，可用 PPA 合成整页淡入淡出，不能使用或操作失败时保留 CPU 路径。这是 UI 合成能力，与地图 JPEG 解码相互独立；不代表天地图 PNG 底图或透明注记会经过 JPEG 硬件，也不构成实板帧率保证。

## 内存预算

| 项目 | 占用，不含分配器和对象开销 |
| --- | ---: |
| 256×256 RGB565 底图 | 128 KiB / 张 |
| 256×256 RGB565 + 8-bit alpha 透明图层 | 192 KiB / 张 |
| 同一 XYZ 底图 + 透明注记 | 320 KiB / 组 |
| 320×240 RGB565 视口 | 150 KiB / 张 |
| 480×480 RGB565 视口 | 450 KiB / 张 |
| 800×480 RGB565 视口 | 750 KiB / 张 |
| 320×8 RGB565 绘制条带 | 5 KiB |

实际可见瓦片数随中心与瓦片边缘位置变化，不能只用 `ceil(width/256) × ceil(height/256)` 作最坏估算。例如 320×240 单层视口可能覆盖 3×2 张瓦片；双层便需要对应的两组可见资源。

`cacheBytes` 约束 Map 的已就绪解码图像与 pending 请求的预留容量，`maxCachedTiles` 约束对应条目数量。`cacheStats().bytes` 只报告已就绪图像的实际字节数，不包含 pending 预留，不能据此把剩余数值全部用作其他分配。**它们不是进程总内存上限**，预算还必须另加：

- 正在下载的压缩输入、网络/TLS 缓冲和 ByteBuffer 容量；
- 解码器内部工作空间，以及尚未提交给 Map 的新解码图像；
- 多 worker 的并行峰值；
- UI 发布的新视口图、旧图、在途渲染资源和 UI/显示驱动存储；
- 请求字符串、marker/route/track、标准容器容量及分配器元数据。

8 MiB PSRAM 的建议主要是给这些并存数据留余量，而不是鼓励把所有内存都设成瓦片缓存。先用 `maxInFlight=1` 和 256 像素瓦片测出真实峰值，再决定并发数。PSRAM 也不保证可以直接作为任意屏幕 DMA 缓冲；显示驱动的 DMA/内部内存要求仍需单独满足。

自定义 `Allocator` 的 allocate/release 必须配对，context 生命周期须覆盖其分配出的所有图像。`submit()` 成功后图像归 Map 管理，不得继续通过旧指针写入；UI 使用的图像也需遵守其自身所有权规则。

像素与字节缓冲的显式分配可以返回 `NoMemory`。请求字符串、向量等 STL 元数据仍受标准库行为约束；在 ESP32 常见的 `-fno-exceptions` 构建中，底层 STL 分配失败可能终止程序，不能保证所有 OOM 都变成 `Status`。请保留实际堆余量，限制视口、并发与数据容量。

## HTTPS 与图片数据

`HttpOptions` 提供 `caCertificate`、`timeoutMs`、`maxResponseBytes`、`userAgent` 和输入缓冲分配器。示例要求真实 PEM 根 CA 和有效系统时间，不使用 `setInsecure()`。CA、主机、重定向与时间错误应作为传输错误处理，不要在失败后静默关闭校验。

ESP32 传输对未知长度/chunked 响应从 `min(8 KiB, maxResponseBytes)` 起始，按实际数据有界倍增，不再一次申请整个响应上限；已知 Content-Length 仍按声明长度预留并校验。增长、接收和错误路径始终受 `maxResponseBytes` 约束，内存不足返回 `NoMemory`。增长时旧/新缓冲可能短暂并存；预算仍须保留 HTTP、解码图像和最大连续块余量，总空闲内存不等于可以成功分配同样大小的连续块。

PNG 解码验证尺寸与请求的 tileSize 一致，并保留透明图层的直色 RGB 和独立 alpha，合成前不预乘。当前支持 8-bit RGB、RGBA、灰度+alpha，以及 1/2/4/8-bit 调色板和灰度 PNG；16-bit 和隔行 PNG 返回 `Unsupported`。PNGdec 默认行缓冲可能无法容纳部分 512 像素 RGBA 输入，此时返回 `TooLarge`；默认 provider 使用 256 像素。若调整 PNGdec 行缓冲宏，必须让 PNGdec 和应用使用相同配置重新编译。

`Esp32RasterDecoder` 的 JPEG 路径先有界检查文件头与实际尺寸。ESP32-S31 / P4 在 SDK 支持时采用前述硬件路径，直接输出原生 RGB565。其他 ESP32 的软件路径调用 SDK `esp_jpeg`，支持单扫描 8-bit 基线灰度/三分量 JPEG，渐进/CMYK 不支持；软件解码为 RGB888 后转换成原生 RGB565，256 像素瓦片临时缓冲为 192 KiB。透明图层必须使用 PNG。此适配头仅在相应 SDK 组件可用时显式包含；纯 ESP-IDF 应用需自行声明实际使用的硬件或软件 JPEG 组件依赖。

HTTP 200 不保证内容是图片：错误页、登录页、无效 token 响应、损坏 PNG 和超限响应都应失败；不要把未验证的数据当像素使用。

本版缓存位于 RAM，没有自动持久化、磁盘 LRU 或离线地图包导出。SD/LittleFS 可用 `FsTileTransport` 读取预先准备的 PNG 包；自定义存储与服务器可实现 `Transport`，并保持响应上限、图层、tileSize、样式版本和缓存键的一致性。能读文件不等于拥有打包某地图服务内容的权限。

## 地图服务配置与边界

内置 factory 负责构造服务源，不赠送账号、密钥、额度、离线授权或路由服务。

| factory | 栅格路线 | 需要关注 |
| --- | --- | --- |
| `providers::tianditu(key, labels)` | 天地图 Web Mercator 底图，可附注记透明图层 | 服务 key、实际可用图层/编码、底图与注记分别请求、国内业务归属要求。 |
| `providers::geoapify(key, style)` | 256×256 PNG XYZ | API Key、样式有效性、归属与请求速率。 |
| `providers::mapTiler(key, mapId)` | 显式 256 像素 PNG Maps API | API Key、栅格权限、MapTiler/数据源归属、临时缓存与批量下载边界。 |
| `providers::mapbox(token, username, styleId)` | 256 像素 PNG Static Tiles API | access token、样式兼容性、Mapbox Logo/文字归属和适用授权。 |
| `providers::xyz(url, attribution, minZoom, maxZoom, tileSize)` | 自定义 HTTPS 模板 `{z}/{x}/{y}` | 准确的投影、XYZ/TMS方向、tileSize、归属和来源许可由调用方确认。 |

不要把服务 512 像素默认瓦片或 `@2x` 结果直接当作 256 像素请求提交。不同服务对 tileSize、zoom offset 与 HiDPI 的约定不同；本库内置源选择普通 256 像素配置，自定义源须自行对齐。当前渲染是 Web Mercator XYZ，不自动翻转 TMS 的 Y。

以下官方资料于 2026-10-19 核对，服务政策可能更新：

- **MapTiler**：直接提供 PNG/JPEG 栅格请求，默认 512、显式 `/256/` 请求 256，`@2x` 是另外的像素密度选项。[Maps API](https://docs.maptiler.com/cloud/api/maps/)、[尺寸说明](https://docs.maptiler.com/guides/maps-apis/maps-platform/difference-between-256x256-512x512-and-hidpiretina-rasterized-tiles/)。Cloud 允许单用户临时缓存，批量下载、内容导出及代理用途有单独约定要求；需显示服务及数据归属。[Cloud 条款](https://www.maptiler.com/terms/cloud/)、[归属](https://docs.maptiler.com/guides/map-design/attribution/add-attribution/)
- **Geoapify**：普通 PNG XYZ 为 256，`@2x` 为相同覆盖范围的 512 像素图。官方说明允许缓存、保存和再分发生成瓦片，但仍需遵守请求速率与对应计划/样式归属；常见归属涉及 OSM、OpenMapTiles，免费计划还包括 Geoapify。[API](https://apidocs.geoapify.com/docs/maps)、[缓存与再分发说明](https://www.geoapify.com/map-tiles/)
- **Mapbox**：样式道路底图使用 Static Tiles API；它支持 256/512 与 PNG/JPEG。普通 Raster Tiles API 主要面向卫星和上传的栅格内容。默认 HTTP 缓存不代表任意离线批量下载许可；其移动 SDK 离线说明也不能直接当作 ESP32 离线包授权。[Static Tiles API](https://docs.mapbox.com/api/maps/static-tiles/)、[归属](https://docs.mapbox.com/help/dive-deeper/attribution/)、[移动离线范围](https://docs.mapbox.com/help/dive-deeper/mobile-apps/)
- **OSM 公共瓦片**：不是离线包下载源；禁止批量预取/离线下载，要求可识别的 User-Agent、归属和遵循 HTTP 缓存头。不能处理缓存头时至少缓存 7 天。[OSMF Tile Usage Policy](https://operations.osmfoundation.org/policies/tiles/)。本库不会自动添加该服务为默认源。

`TileSource::attribution` 保存说明文本，核心不会自动绘制商标、超链接或满足所有服务的归属布局。应用必须按所选来源在实际地图界面显示要求的文字、Logo/链接；仅串口输出或在 README 写一遍不能代替产品界面归属。小屏能否折叠归属、如何展示 Logo，应按该服务的具体规则处理。

服务覆盖范围、道路准确度、中文标注、服务器连通性、配额和商业可用性需在目标地区实际验证。位置刷新与底图更新是两件事：移动定位点不需要每次重新下载底图，能复用已有可见瓦片；“实时地图”也不表示底图道路或路况秒级更新。

## 集成检查

先运行无网络示例，确认条带绘制、方向、颜色与坐标；再接单层 PNG 源验证证书、尺寸、配额和内存；最后打开透明注记、worker 与 UI 发布。

实际硬件还需覆盖：拖动跨瓦片边界、世界日界线、缩放锚点、来源切换时的迟到结果、失网/重连、错误响应、解码损坏、PSRAM 不足、UI 跨页和长时间轨迹上限。记录 HTTP/解码状态与内存统计时避免泄露服务密钥或用户精确轨迹。
