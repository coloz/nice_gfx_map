# 10：SD 离线 PNG 文件地图

此例使用真正的 `FsTileTransport` → `PngDecoder` → Map 文件加载链路，不连接 HTTP。显示的是项目生成的**合成测试地图 PNG**，不是随库授权分发的真实地理底图。

准备：

1. 安装外部 PNGdec，ESP32 启用 PSRAM，连接可用 SD 模块。S3 默认 SD 独立 SPI3：SCK=14、MISO=12、MOSI=11、CS=13；经典 ESP32 改用 SCK=14、MISO=32、MOSI=27、CS=13。频率 10MHz。屏幕保持 SPI2 与默认 18/19/23 接线。
2. 将本目录 `data/tiles` 完整复制为 SD 卡根目录 `/tiles`，不要多套一层 `data`。
3. 文件结构为 `/tiles/demo-v1/0/14/{x}/{y}.png`。fixture 为 128×128 PNG、z14，覆盖 `(30.6586,104.0648)` 附近 9×9 瓦片。

命令：`r` 重新挂载；`c` 清 RAM 瓦片缓存后从 SD 重读；通用平移有效。缩放固定在 z14；超出 fixture 区域会显示缺图状态，不生成假图补缺。

期望结果：有卡有图时显示连续地图；无卡时 HUD 显示 NO CARD，缺文件/损坏 PNG 时显示 NotFound/DecodeError 与失败数。缺卡后重新插入可按 r。运行中的错误不会静默回退到过程底图。

SPI 注意：ESP32-S3 的 Arduino `HSPI` 对应独立 SPI3；经典 ESP32 使用 `VSPI` 选择 SPI3，避免与 NICE_GFX 默认 SPI2 冲突。其他目标会明确编译报错，需要自行核对第二条可用总线。代码使用 `SD.begin(CS, sdSpi, 10000000)`，不使用 S3 默认 SD 总线去争用屏幕 host。

可在 include 前按需定义 `NICE_MAP_SD_CS/SCK/MISO/MOSI` 中任意宏换接线，其余保留默认。**经典 ESP32 的 GPIO11 常连接板载 Flash，不能照搬 S3 的 11/12 引脚**；代码对经典芯片选择 27/32，但仍需核对你板上的外设占用。本例没有实板总线验证结论。

真实数据接点：用有权离线保存的 PNG 瓦片替换 fixture，保持 sourceId、layer、zoom、tileSize 和 XYZ 的 Y 方向一致。更新数据包时改变 sourceId，避免混用样式版本；按服务条款获取离线授权。

依赖：ESP32/S3、NICE_GFX、ILI9341、SD、PNGdec、fixture，建议 PSRAM。不需要 Wi-Fi/key。
