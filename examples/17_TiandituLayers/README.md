# ESP32 天地图全图层与双投影

2026-10-20 已在 COM10 的 ESP32-S31-Korvo-1 验证 7 图层×2 投影、三层组合、8.5 级连续缩放、平移、严格拒绝和显式兼容路径，共 42 项检查通过；每种组合均实际下载瓦片，瓦片失败与显示错误为 0。示例二进制 1,764,048 B，来源快照与 Flash 校验已绑定。[设备报告](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/device17/layers-device-report.json)只记录串口与程序观测，透明瓦片不证明该区域有地理内容，也不代替面板目视。工作区根可运行 `nice_gfx_editor/scripts/test-tianditu-layers-device.ps1` 复测。

本例在 ESP32 上按需访问真实 WMTS，展示 `vec/cva/img/cia/ter/cta/ibo` 七类图层的 `_w` 与 `_c`，支持底图、注记、境界组合。默认 S31 Korvo-1 SUB3 RGB 800×480，其他 ESP32 使用 ILI9341 320×240。硬件接线见 `LayerDisplay.h`；S31 也可定义 `NICE_TDT_LAYERS_KORVO=0` 切换 SPI 屏。本例使用串口菜单，不接入触摸驱动；库的 TouchPan/PinchGesture 可在产品的 owner task 接入。

安装 NICE_GFX、NICE_GFX_MAP 和 PNGdec，启用匹配的 PSRAM。800×400 地图区三层最坏需约 8 MiB 原始瓦片预算，另有 1 MiB 合成预算、RGB framebuffer、TLS 与解码峰值，建议 S31 16 MiB PSRAM；320×200 使用 4 MiB 原始预算。预算按需消耗，不预分配全部瓦片。缺内存返回明确状态，不伪造地图。

复制 `LayerSecrets.example.h` 为本地 `LayerSecrets.h`，填 2.4 GHz Wi-Fi、天地图服务端/原生应用 key 和从实际官方证书链核验的当前根 CA。该本地文件已忽略，不提交凭据或含密钥二进制。HTTPS 保持验证并先校时，不作 insecure 回退。示例不打印 URL、原始 XML 或密钥。

115200 波特率，逐行发送：

| 命令 | 效果 |
| --- | --- |
| `layer vec` / `cva` / `img` / `cia` / `ter` / `cta` / `ibo` | 选七种官方图层之一，独立注记/境界在背景上显示 alpha |
| `projection w` / `projection c` | 正确切换 Mercator / Geographic 网格，保留地图中心与演示路线、标记 |
| `labels 1` / `labels 0` | 给 vec/img/ter 自动组合对应注记；独立叠加层不重复添加 |
| `borders 1` / `borders 0` | 追加 ibo 境界；已选 ibo 时不重复添加 |
| `strict 1` / `strict 0` | 严格拒绝 / 显式启用已知 cta_w 元数据修正 |
| `center 30.6586 104.0648` | 修改中心，经纬度来自应用，不是 GNSS 定位 |
| `zoom 8.5` / `pan 64 -32` | 连续缩放 / 按屏幕像素平移 |
| `w/a/s/d` / `+/-` | 平移 / 整数级缩放，仍需回车 |
| `reload` / `status` / `help` | 重取所需元数据 / JSON 状态 / 命令表 |

串口 `wmts-status` JSON 给出当前已应用 `projection`、`layer`、`layers`、`warnings`、矩阵缩放范围、中心、ready/inflight/failures、缓存占用、显示初始化/传输错误和 PSRAM 总空闲/最大连续块；`wmts-profile` 给出每次配置完成的版本和状态。`fixture_route_points`/`fixture_markers` 说明演示配置数量，不冒充运行观测；`fixture_marker_hit_visible` 是实际 Map 投影与命中结果，标记离开视口时为 false。失败时保留上一个可用配置与覆盖物。两队列只传指针/完成标志，单个 job/completion 的 C++ 对象在 worker 与 owner 之间有明确交接；Map 与显示只在 loop 所属 owner 调用。网络 worker 固定创建在 Core0，IDF 对 PIE 用户可调整亲和性。

当前能力响应的广告范围：vec/img/cia/cta/ibo 1–18，cva 1–19，ter 1–14。组合取公共范围。`cva_w` 第 19 级元数据矛盾，本例明确限定 1–18；不声称已验证第 19 级。`cta_w` 元数据错误，默认示例策略显式启用仅针对已知模式的 Mercator 行数修正，并在 JSON/HUD 显示 warning；`strict 1` 可观察 `Unsupported`。原始能力响应不被改写。详见[矩阵证据与 API 契约](../../docs/tianditu-wmts.md)。

初始成都中心和蓝色路线、红色 Pin 是明确的演示覆盖物，不是在线规划或真实定位。Pin 头部半径 10 像素、总高 30 像素，尖端锚定预置经纬度；切换投影时仍以同一坐标绘制并检查命中。元数据与瓦片为实际服务请求，不作过程瓦片回退。图片透明不一定是错误，HTTP 成功也不保证某地某级有内容。图层署名始终可见；发布产品时按服务当前条款补齐完整归属/链接/标识要求。

主机验证实际 `LayerSelection.h` 配置工厂，使用脱敏官方 XML，不使用真实 key 或联网：工作区根 `node tests/run_tianditu_layers_example.mjs --sanitize`，通过 257 项 UBSan 检查；[主机报告](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/example17-host.json)记录公开源码哈希，排除私有配置。目标编译、真机联网/颜色和显示验收需另行记录，不能由主机 shim 推断。

工作区可运行 `powershell -NoProfile -ExecutionPolicy Bypass -File nice_gfx_editor/scripts/build-tianditu-layers.ps1 -Config <本机CLI配置路径> -Jobs 3` 重现 compile-only 构建。脚本不接串口，比较公开源码前后哈希，不读取/哈希/输出私有头；完整编译诊断仅保留在本地工具目录。2026-10-20 的实际 S31 目标完整链接通过：固件 1,764,048 字节，静态 RAM 70,488 字节，见[构建报告](../../../nice_gfx_editor/results/2026-10-20-tianditu-services/example17-build.json)。设备联网与显示状态另以真机记录为准。
