# 场景示例索引

新增 [16_ESP32 天地图服务总览](16_TiandituServices/README.md)与 [17_天地图 WMTS 图层/投影浏览](17_TiandituLayers/README.md)：前者以 worker 和串口菜单调用搜索、编码、行政区划、路线、公交、静态图与只读 WFS；后者按需读取真实矩阵，切换七类图层的 w/c、透明注记和境界组合。ESP32-S31 默认适配 Korvo RGB 屏，其他 ESP32 可使用 ILI9341。私有凭据独立配置，日志不输出 URL 或原始响应。当前共 17 个示例。

[15_Wio Terminal 天地图](15_WioTerminalTianditu/README.md) 直接使用板载屏幕、rpcWiFi 和按键，无需 SD 卡或 PSRAM，以受限 RAM 下载并逐块显示真实底图与注记。用户连接信息放在 Git 忽略的本地配置头中。

[14_天地图自行车记录仪与导航](14_TiandituBikeRecorder/README.md) 使用真实天地图、UART GNSS 和 SD，显示骑行仪表、保存轨迹 CSV，并导航导入的骑行路线；支持 GPX 转 CSV。14 与 15 的硬件和依赖各自独立。

04—13 是十个独立业务场景，默认使用明确标为 **SIM / SYNTH** 的可重复模拟数据；10 使用生成的 SD PNG 文件，不会在缺卡或缺图时悄悄切换过程底图。它们演示真实库 API 和状态交互，不表示已接入生产定位、车队、气象、测绘或农业平台。

04–13 采用相同屏幕布局：ILI9341 横屏 320×240；顶部标题 20px，地图 320×200 位于 y=20，底部状态 20px。共用 `<demo/ExampleSupport.h>`，包含 128 像素连续世界坐标测试底图和条带绘制。它们无需服务 key；实际地图需另外配置获准的瓦片源。14 使用独立的真实地图管线与骑行仪表布局，配置见其 README。

| 目录 | 场景与业务状态 | 主要额外命令 |
| --- | --- | --- |
| 04_GnssTracker | 模拟定位 / 真实 UART NMEA，轨迹、失去定位、跟随 | `m` SIM/UART；`c` 清轨迹 |
| 05_CycleNavigation | 给定路线引导，下一点、剩余距离、偏航、到达 | `o` 模拟偏航；`r` 重启 |
| 06_HikingWaypoints | 航点选择、距离/方位、步行与总览 | `n/b` 航点；`t` 走/停；`v` 总览 |
| 07_FleetMonitor | 五辆车移动、选车跟随、标记命中 | `1..5` 选车；`v` 总览；`h` 中心命中 |
| 08_GeofenceAlarm | 圆/多边形围栏、迟滞、ENTER/EXIT | `p` 形状；`h` 迟滞 8/25m |
| 09_DeliveryStops | 顺序停靠、到点才能确认、完成进度 | `t` 行驶；`c` 确认；`r` 重置 |
| 10_OfflineSdMap | SD 文件源、PNG、缺图/缺卡错误 | `r` 重挂载；`c` 清 RAM 缓存 |
| 11_WeatherOverlay | 合成透明天气层、透明度、帧切换 | `[/]` 透明度；`o` 显隐；`n` 下一帧 |
| 12_AgricultureZones | 多边形地块、选地、模拟湿度/灌溉 | `1..3` 选地；`i` 灌溉模拟；`v` 总览 |
| 13_TrackReplay | 时间戳轨迹、暂停、速度、seek、循环 | 空格、`</>`、`[/]`、`r`、`l`、`v` |
| 14_TiandituBikeRecorder | 真实天地图、骑行仪表、SD CSV、导入路线导航 | `r` 开始/停止；`p` 暂停；`l` 重载路线；`v` 总览 |
| 15_WioTerminalTianditu | Wio Terminal 无 SD 天地图、PNG 逐行绘制、透明注记、按键平移缩放 | 五向键平移/复位；A 放大、B 缩小、C 重载；串口 `l` 注记开关 |
| 16_TiandituServices | ESP32 天地图公开 HTTP 服务、真实结果叠图、单 worker；S31 Korvo / ILI9341 | 逐行 `nearby`、`geocode`、`district`、`drive`、`transit`、`static` 等；`help` 看完整菜单 |
| 17_TiandituLayers | 七类 WMTS 栅格图层、Mercator/Geographic 双投影、透明注记/境界、能力响应验证 | 逐行 `layer vec`、`projection c`、`labels 1`、`borders 1`、`status`；`help` 查看菜单 |

04—15 通用串口命令：115200 波特率，`w/a/s/d` 按屏幕方向平移，`+/-` 缩放，`f` 跟随最后定位。串口监视器可发送单字符，不需要换行。16/17 使用带参数的逐行命令，必须发送回车；其他业务命令详见各目录 README。

示例引脚：CS=5、DC=2、RST=4、SCK=18、MOSI=23、MISO=19。ESP32/S3 板型、可用引脚和屏幕必须自行核对；在首次包含 helper 前按需定义 `NICE_MAP_DEMO_CS/DC/RST/SCK/MOSI/MISO` 中任意宏可换接线，其余保留默认。推荐启用 PSRAM。地图绘制按 revision / HUD 内容变化触发，最多每秒 10 帧；这是节流策略，不是硬件帧率保证。

04 的真实 GNSS 模式需 3.3V UART 外设并核对 RX/TX；10 需要独立 SPI3 接线的 SD、PNGdec 和生成的 fixture，且其默认 SD 引脚面向 S3，经典 ESP32 须换可用引脚。04–13 中其他例子无需网络、GNSS 或服务凭据。14 需要真实天地图凭据、Wi-Fi、GNSS 与 SD。所有例子的完整编译与数据要求记录于 `scenarios.json`。

## 换成真实地图底图

04—09、11—13 共九个新例默认使用过程生成的合成地理底图；10 从 SD 读取本项目原创合成 PNG fixture。**04 即使切到真实 GNSS UART，定位仍叠在合成底图上**，不会因此自动联网获取道路。这些默认模式都不是在线真实地图。

不能只调用 `mapView.setSource(providers::...)`：helper 默认 `automaticTiles=true`，使用的是不做 I/O 的 ProceduralTransport / ProceduralDecoder；若不替换加载管线，会继续生成测试像素。

正确替换顺序：

1. 参考 [02_OnlineTiles](02_OnlineTiles/02_OnlineTiles.ino)，先完成 Wi-Fi、有效系统时间、服务 key 与真实根 CA 配置，并安装 PNGdec。保持 TLS 校验开启。
2. 在 `app.begin(...)` 后设置 `app.automaticTiles=false`，停用 helper 的过程瓦片加载。业务覆盖物、轨迹与 HUD 可继续使用。
3. 按 02 的写法创建并**持续保存**实际 `Esp32HttpTransport`、`PngDecoder` 和 `TileLoader`。它们应在整个 loop 生命周期存在，不能是 setup 返回后就销毁的局部对象。
4. 再调用 `mapView.setSource(providers::geoapify(key,...))` 或其他获准的真实源，检查 Status，并确认视口 zoom 与 provider 范围匹配。
5. 在 loop 调用实际 loader，或采用 root README 说明的 worker 管线。`app.tick(status)` 现在只负责绘图/HUD，不再自己制造瓦片。

加载层的核心关系如下；HttpOptions / 凭据初始化请直接复用完整 02 示例，保留各场景自己的业务代码：

```cpp
// 足够生命周期的对象：与 app 一样保存在全局或长期存活的应用对象中。
Esp32HttpTransport onlineHttp(makeHttpOptions()); // 02 中的真实根 CA / timeout 配置
PngDecoder onlineDecoder(esp32PsramAllocator());
TileLoader onlineLoader(onlineHttp, onlineDecoder,
                       []() -> uint32_t { return millis(); });

// setup 中，Wi-Fi / 时间 / app.begin 成功以后：
app.automaticTiles = false;
Status configured = mapView.setSource(providers::geoapify(kProviderKey, "osm-bright"));

// loop 中，保留原场景的定位、marker、围栏等业务更新：
Status loaded = onlineLoader.loadNext(mapView, millis());
app.tick(statusText); // automaticTiles=false 时不调用过程加载器
```

这段是集成片段，不是另一个可独立编译的草图；需与 02 的 include、凭据、联网和错误处理合并。同步 HTTP 仍会阻塞，产品应把下载/解码放入 worker，在 owner task 调用 submit/fail。

11 的第二层默认也是**生成的假天气**。若换真实天气，需要真正的第二层 URL、PNG alpha、匹配的投影/tileSize、时间版本和图例，不能把更换底图误认为已接入气象服务。真实底图和数据的归属、Logo、配额与离线许可按各服务官方条款落实；helper 不会替应用完成这些要求。

已有 01/02 是基础过程地图和在线地图示例。03 是需要设计器导出资源的 UI 集成模板，**不能当作独立完整硬件草图**；manifest 使用 `requires-export` 标明。

库的几何是球面近似，农业面积显示是演示估算，围栏和示例到达半径不是安全边界。替换成真实业务前应结合实际定位精度、来源坐标系与服务条款校验。
