# ESP32 天地图服务总览

2026-10-20 已在 COM10 的 ESP32-S31-Korvo-1 完成真实 HTTPS 验证：26 项调用中 25 项 HTTP 200 并解析成功，旧版搜索按预期返回 404；包含所有搜索模式、公交规划/线路/站点/返程闭环、静态图和只读 WFS。最终示例二进制 1,823,328 B，来源快照与 Flash 校验已绑定。[设备报告](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/device16/services-device-report.json)记录串口与程序观测，不代替面板目视。复测前按本文配置本地私有头；工作区根运行 `nice_gfx_editor/scripts/test-tianditu-services-device.ps1`。

本例把天地图公开 HTTP 服务与只读 WFS 接到 ESP32，使用真正的 HTTPS 和库内解析器；没有演示 POI、模拟路线或失败时替换结果的逻辑。串口逐项发起服务，LCD 显示状态及最近一次成功结果，POI、道路路线和行政区边界叠到真实底图。搜索地点、编码位置和 WFS 点使用尖端对准真实经纬度的红色 Pin 图标：头部半径 10 像素、总高 30 像素，自动取景保留 32 像素边距以容纳图标。请求失败显示 HTTP / 服务 / 解析类别并保留最近成功结果，画面明确标为 `Last OK`。

ESP32-S31 默认采用 Korvo-1 SUB3 的 800×480 ST7262 RGB 引脚与 18 MHz 时序；其他 ESP32 默认使用示例 02 的 ILI9341 横屏 320×240 接线。可在 `ServiceDisplay.h` 前定义 `NICE_TDT_USE_KORVO=0` 改用 SPI 屏。此库示例采用串口输入，不声明触摸菜单、GNSS 接入或实际道路跟随；Korvo 原生触摸界面见编辑器项目 `44-korvo-navigator`。

先安装 NICE_GFX、NICE_GFX_MAP、PNGdec。ESP32-S31 需要支持该芯片的 Arduino ESP32 / ESP-IDF 工具链，启用 PSRAM。将 `ServiceSecrets.example.h` 复制为本地 `ServiceSecrets.h`，配置 Wi-Fi、天地图服务端 key 和当前服务证书链的可信根 CA。公共只读 WFS 不需要 key；仅配置 Wi-Fi/CA 时仍可查询真实元数据和要素，LCD 显示摘要，受 key 保护的底图不会启动。私有头已被忽略，不把凭据写入草图、报告或压缩包。模板可以编译，尚未配置网络时设备会明确提示并且不会联网。ESP32-S31 使用 2.4 GHz Wi-Fi。

Wi-Fi 和 SNTP 在主循环中异步启动；系统时间未就绪时不会发起 HTTPS。请求始终校验证书，不存在不安全 TLS 回退。首个位置是成都天府广场附近的**预置地图中心**，不是卫星定位。菜单操作产生计费或计额请求，应遵守所用 key 的开放范围、配额及天地图使用条款；本例不会自动批量扫服务。

串口为 **115200**，每条命令以回车结束，文字参数使用 UTF-8。无参数时使用代码中明确列出的成都位置、目的地或关键词。`help` 随时列出命令，`status` 仅输出连接和任务状态，不输出 Wi-Fi 名称、地址、请求 URL 或凭据。

| 服务族 | 命令 | 库 API 与结果呈现 |
| --- | --- | --- |
| V2 搜索 | `search 公园`、`viewport 公园`、`nearby 公共厕所` | 普通、视野、周边搜索；POI 名称/地址/直线距离和地图标记 |
| V2 搜索扩展 | `suggest 天府`、`names 天府广场`、`polygon 公园` | 提示词、地名、多边形检索；建议或 POI；多边形以当前中心附近的闭合范围查询 |
| V2 搜索扩展 | `admin-search 公园`、`category 公园`、`statistics 公园` | 行政区、分类、统计；默认行政代码 156510100，分类接受官方名称或编码；默认“公园”已通过真实服务核验 |
| 旧版搜索 | `legacy 1 公园` 等 | `legacySearch`；允许 1/2/3/4/5/6/7/10，8/9 被本地拒绝。旧接口在当前服务可能返回 404，按实际错误显示，不改成其他服务冒充成功 |
| 地理编码 | `geocode 成都市天府广场` | 地址转坐标，显示真实位置及格式化地址 |
| 逆地理编码 | `reverse` | 地图中心转地址 |
| 行政区划 | `district 成都市` | 一级子区划和边界；显示返回的名称、代码及边界环描边 |
| 驾车 / 步行 | `drive`、`shortest`、`avoid`、`walk` | 最快、最短、避高速、步行；起点为当前地图中心，终点为明确预置的人民公园附近位置；显示第一条真实路线及距离/时间/步骤数 |
| 公交规划 | `transit` 或 `transit 3` | 公交策略位掩码 1/2/4/8 的组合；起点为当前地图中心，终点为已真实核验的预置坐标 30.6595 N / 104.0708 E；显示返回方案、几何和步骤 |
| 公交线详情 | `transit-detail UUID` | 查询官方返回的线路 UUID；不把显示名称当 UUID |
| 公交反向线 | `transit-reverse LINE_UUID STATION_UUID` | 查询指定线路与站点的反向线信息；无有效 UUID 不伪造结果 |
| 静态地图 | `static` | 256×256 真地图，包含预置标记和路径；HTTP 字节交给 PNG/JPEG 解码器后显示。ILI9341 视口不足时仅显示可容纳区域 |
| 静态图像素坐标 | `pixels` | 将当前中心和预置目的地点转换为静态图像素坐标，在串口/LCD 报告 |
| 真实 WMTS 底图 | `map` | `providers::tianditu` 与 PNG / JPEG 解码，地图平移缩放后自动请求缺少瓦片 |
| 公开 WFS 能力 | `wfs-capabilities` | 官方只读服务的实际图层、CRS、操作、输出格式；不需 key |
| 公开 WFS 模式 | `wfs-describe` 或 `wfs-describe TDTService:RESP` | 实际字段与类型；默认使用已核验的公开图层 |
| 公开 WFS 要素 | `wfs-features` 或 `wfs-features TDTService:RESP` | WFS 1.0.0 / EPSG:4326 / 经度纬度 BBOX / GeoJSON；默认只取成都固定范围内 2 个真实点 |

`w/a/s/d` 各平移 80 像素，`+/-` 以视口中心缩放一级。这些命令也需要回车。`help` 返回静态说明画面并停发新瓦片，`map` 恢复地图。`home` 无网络请求地恢复预置中心和 zoom 15、清除结果叠图，保留 `tiles off/on` 状态，便于下一次以确定起点查询；正在请求时等待完成后重试。`status` 报告屏幕初始化/最近传输错误、实际中心、缩放、PSRAM 剩余及最大连续块，不报告私有连接信息。服务请求与瓦片共用一个 worker；任务执行时串口继续响应，但另一项服务会报告 `BUSY`，不会排成长队。逐项核验服务时可先 `tiles off` 暂停新增瓦片请求，保留已有地图及结果绘制；`tiles on` 或 `map` 恢复。已在途的请求仍按原截止时间完成。

公交相关成功结果会输出经过脱敏的 `POI id`、`POI_LINE id`、`ROUTE id`、`TRANSIT_LINE id` 和前 3 个 `TRANSIT_STATION id/name`，可据官方返回的线路/站点类型用于后续详情或反向线命令。只展示有限条公开 ID 摘要，不从名称猜测标识，不输出原始 XML/JSON。

服务 API 是同步接口，但只在 Core 0 worker 调用。通过请求/完成队列交还结果；`Map::submit/fail`、标记、路线、边界与绘图均在主循环单一 owner 执行。默认每响应 128 KiB、最多 64 项/8 路线/4096 几何点，下载和解析字节限制相同；64 项可容纳常见的多站公交线，8 路线可容纳默认场景实际返回的 5 个公交方案。超限会明确失败，并以脱敏 `TDT_DETAIL message=...` 说明限制类别，不悄悄截断为完整路线。地图选取前 24 个有坐标的 POI 标记和第一条路线用于展示，完整解析结果仍由应用持有。临时 JSON/XML、解码缓冲和结果有界；仍应按目标内存容量调整，不代表所有行政区完整边界都能装入该示例。

WFS 使用实测的官方公开 HTTPS 地址，版本固定为 1.0.0；示例 BBOX 为经度 104.03–104.09、纬度 30.63–30.69，非自动全国下载。图层可通过能力文档选择，查询仍受公开范围、响应大小和几何点数限制。点/多点画为标记，线/多线仅选第一条连续线段作展示，面/多面按独立环描边，不连接洞或岛。Collection 只报告实际部件与属性数量，不猜测扁平几何的拓扑。完整要素和属性保留于 `Result.features`；只有用户显式查询才请求，不包含 WFS 写入或事务接口。

服务成功与显示是两个检查：静态图解码失败会额外输出 `STATIC decode=...`，不会把原始 body 当图片。ESP32-S31 的合格基线 JPEG 由硬件直接解码 RGB565；天地图 PNG 图层仍走 PNGdec。WMTS 图片和服务数据都来自实际网络，没有过程底图回退。

日志只输出白名单摘要与错误码，不输出 `Request.url`、`Result.raw` 或原始返回文本。驾车 XML 可能回显 key，因此即使开启自己的调试也不要直接打印响应。主画面保留 Tianditu 来源说明；实际分发时按官方要求补齐标识、署名、链接及适用许可。

该示例覆盖 ESP32 可直接使用的 HTTP 服务与已有 Mercator 栅格显示管线。[官方 V2 搜索说明](https://lbs.tianditu.gov.cn/server/search2.html) 允许以分类名称或编码查询，本例采用真实核验的中文分类。浏览器 DOM/JavaScript API、WebGL 三维引擎、官方 Android/iOS SDK 不在 ESP32 上运行，不能把串口演示描述为这些平台 SDK 的移植。库的通用请求构建接口可传递官方新增参数，但不会承诺未经验证的服务权限或运行结果。

本工作区的实际验证：共享于真实 worker 的 `ServiceRequests.h` 和生产库请求构建器通过 **160 项 UBSan 检查**，包括全部命令、转义、非法参数、WFS 无 key 与轴序。[主机证据](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/example16-host.json) 明确未执行网络；[ESP32-S31 目标编译证据](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/example16-build.json) 记录同次二进制与 358 份公开编译输入，私有头仅记录 present，不记录值或哈希。编译证据不能代替实际服务、LCD 和硬件测试。

从本工作区根运行 `node tests/run_tianditu_example_requests.mjs` 可复现请求工厂检查。Windows 下从 `nice_gfx_editor` 运行 `scripts/build-tianditu-services.ps1 -Config <Arduino-CLI配置文件>` 可重新编译 S31，默认仅构建、不烧录，并拒绝构建期间发生公开源码变化的证据。供独立 Arduino 使用时直接打开本目录草图，按前述依赖和私有配置要求编译即可。
