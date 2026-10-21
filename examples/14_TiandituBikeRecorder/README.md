# 天地图自行车记录仪与导航

本例使用**真实天地图底图与注记、真实 UART GNSS 和 SD 卡**，记录骑行轨迹及仪表数据。没有模拟定位或假地图回退。按本次需求，记录内容是位置、速度、里程和时间。

屏幕为横向 320×240：顶部 40 像素显示 REC/PAUSE/STOP、速度、里程、时间、平均/最高速度与卫星数；中部 320×160 显示天地图、路线、骑行轨迹和位置方向；底部显示下一航点、距离、方位、剩余距离、偏航/到达、地图联网状态与天地图署名。

## 硬件与依赖

- ESP32-S3，**启用至少 8 MiB PSRAM**。双层 256 像素瓦片缓存预算 4 MiB，另需 TLS、解码和系统内存。
- ILI9341 SPI 屏幕，3.3V UART GNSS，SD 卡及 SPI 接口模块。
- 本地 `nice_gfx`、`nice_gfx_map`、PNGdec 1.1.2；以 Arduino-ESP32 3.3.12 检查。
- 手机热点或其他 Wi-Fi；天地图账号授权的有效 tk 和服务当前可信根 CA。

`BikeConfig.h` 独立配置接线与凭据。默认示例接线如下，各引脚均可分别通过对应 `NICE_BIKE_*` 宏覆盖。须核对实际板卡的可用引脚、电压、USB、Flash/PSRAM占用和上电要求。

| 外设 | 信号 | ESP32-S3 示例 GPIO |
| --- | --- | --- |
| ILI9341 / SPI2 | CS / DC / RST | 10 / 9 / 8 |
| ILI9341 / SPI2 | SCK / MOSI / MISO | 12 / 11 / 13 |
| SD / SPI3 | CS / SCK / MOSI / MISO | 17 / 14 / 15 / 16 |
| GNSS / UART1 | ESP RX ← GNSS TX | 18 |
| GNSS / UART1 | ESP TX → GNSS RX | 默认 -1，不连接 |
| 全部 | GND | 共地 |

NICE_GFX 占用 SPI2，SD 单独使用 SPI3，不共用总线。GNSS 默认 9600 波特率，需输出带校验和的 `$GPRMC`/`$GNRMC`，建议同时输出 GGA 获取海拔与卫星数。仅有 GGA 不会让速度和骑行记录保持有效。GNSS RX 环形缓冲为 8192 字节，处理速率和记录速率约为 1 Hz。

## 首次运行

1. 修改 `BikeConfig.h` 的 Wi-Fi、`TiandituKey` 和 `RootCa`。根 CA 必须来自实际服务证书链的可信来源，并随证书更新维护。本例保持 TLS 校验，等待有效系统时间后才发送地图请求。
2. 在天地图控制台按实际使用方式申请应用和密钥，核对服务授权、访问限制和配额；申请流程见[官方帮助](https://tianjin.tianditu.gov.cn/static/help/note.html)。地图服务采用 `vec_w` + `cva_w` 的 WMTS 墨卡托瓦片，参考[官方地图服务文档](https://lbs.tianditu.gov.cn/server/MapService.html)及[官方节点技术说明](https://heilongjiang.tianditu.gov.cn/iportal/iClient/forJavaScript/docs/openlayers/ol.source.Tianditu.html)。
3. 准备 FAT SD 卡，把自己的路线保存到卡根目录下的 `/routes/bike.csv`，格式见后文。`data/routes/bike.csv` 仅为**合成坐标格式样例**，没有核对真实道路，不应直接当作骑行线路。复制的是 `data` 下的内容，不是名为 data 的外层目录。
4. 按实际接线配置 ESP32-S3 与 PSRAM，编译上传本目录同名 `.ino`，打开 115200 串口。
5. 上电会尝试挂载 SD、载入路线、连接 Wi-Fi，并默认新建一次骑行记录。`AutoStartRecording=false` 可改为手动开始。到室外等 GNSS 定位；首个有效 RMC 后进入跟随。此处没有自动模拟模式。

缺凭据时屏幕显示 `SET WIFI/KEY/CA`，没有 Wi-Fi 时显示 `NO WIFI`，校时未完成显示 `WAIT CLOCK`；这些状态下 GNSS 和 SD 记录仍能工作。断网时保留 RAM 中已经缓存的瓦片，缺失区域明确留空，不承诺完整离线地图；本例不会批量下载或永久保存天地图瓦片。SD 缺失时仍可看在线地图和定位，但没有轨迹文件与文件路线。更换/重插 SD 后重启设备重新挂载。

## 串口操作

| 指令 | 操作 |
| --- | --- |
| `r` | 开始一次新记录 / 停止并刷新当前记录；重开会分配新目录，不覆盖旧记录。 |
| `p` | 暂停 / 恢复当前记录；定位与导航继续，暂停间不累积路程或移动时间。 |
| `l` | 从 SD 重新载入路线，并把导航进度重置到首点；无效文件会清除旧导航。 |
| `v` | 总览整条路线，并退出位置跟随。 |
| `f` | 恢复跟随当前位置。 |
| `w/a/s/d` | 平移地图，退出跟随。 |
| `+/-` | 缩放，天地图级别 1–18。 |

发送单字符即可，无需换行。当前示例用串口操作，便于移植为实体按钮或触摸事件。

## 骑行路线与导航

CSV 必须是十进制度数，**纬度在前，经度在后**，至少 2 点、最多 1024 点，整个文件最多 128 KiB，单行最多 127 字节。允许空行和以 `#` 开头的整行注释，不支持引号字段、BOM 或额外列。读完全部文件且校验成功后才使用路线，超限会报错，不会悄悄截断。

```csv
latitude,longitude
30.6586000,104.0648000
30.6588000,104.0648000
30.6590000,104.0648000
```

路线坐标应与 GNSS/天地图一致，不能把经过 GCJ-02/BD-09 偏移的经纬度直接混用。支持用本目录 `tools/gpx_to_route.py` 将自己已有的 GPX 轨迹或路线转换为 CSV，需要 Python 3.10+，没有第三方依赖：

```sh
python tools/gpx_to_route.py ride.gpx bike.csv
python tools/gpx_to_route.py ride.gpx bike.csv --track 1 --segment 2
python tools/gpx_to_route.py ride.gpx bike.csv --route 1
```

序号从 1 开始；多轨迹或多分段须明确选择，不合并不连续的 track segment，也不静默降采样。默认拒绝覆盖现有输出，显式 `--force` 才替换。

这是**天地图底图 + 预先导入的骑行路线航点导航**：从首点出发，依次进入 25m 到点半径，显示到下一点的距离和真实北方位；偏离当前路线段超过 60m 显示 `OFF ROUTE`，到终点显示 `ARRIVED`。路线必须由用户确认适合自行车。没有在线骑行路径搜索、道路吸附、转弯语音或自动重规划；本例没有把天地图驾车规划当成自行车规划。

请从起点开始按顺序骑行。中途加入或跳过未到达航点时，导航仍会指向该未到达点；定位丢失或采样跨过小半径也可能错过到点。平移地图和暂停记录后导航仍然工作；屏幕北向固定，只旋转位置方向标记。

## 文件与统计

每次记录新建 `/rides/ride0001/track.csv`，依次到 `ride9999`；目录已存在就跳过，包括断电后只留下目录的情况。开始新记录会清空屏幕历史轨迹和本次统计，但不删除 SD 旧文件。

CSV 列为：

```text
uptime_ms,utc_seconds,latitude,longitude,speed_kmh,altitude_m,satellites,distance_m,moving_ms,duration_ms
```

- `uptime_ms` 是本次开机的无符号 32 位毫秒时间，约 49.7 天回绕；`utc_seconds` 是 GNSS 当日 UTC 秒，**没有日历日期**，跨午夜需结合行顺序识别。离线记录不依赖 NTP，NTP 用于地图 HTTPS 校验证书。
- 仅记录有定位、具有当前 RMC 速度字段且通过异常检查的采样。超过 3 秒无有效 RMC 清除当前位置与速度；无定位、暂停及超过 5 秒的采样间隔不跨段累积距离。
- 里程是相邻有效位置的球面距离；移动判定阈值为 0.8m/s；高于 35m/s 或超过速度上限加 8m 容差的跳点会被拒绝。统计为 GNSS 估计，静止漂移仍可能增加里程，不能代替轮速传感器。
- 时间从本次记录的首个有效采样开始，包括其后的停顿、暂停和失锁时间。平均速度为里程/总时间，最高速度取有效 GNSS/位置差分速度；海拔和卫星数沿用最近一次 GGA，未携带字段年龄，未收到时为零。
- SD 每 5 秒尝试 flush，暂停/停止时也 flush；写入不足立即停止并显示错误。Arduino FS 的 flush 没有可用的完整介质提交结果，突然断电仍可能丢失最后几行或损坏文件系统。正常结束请先按 `r`。
- 屏幕只保留最近 2048 个轨迹点，SD CSV 持续追加。SD 和显示属于同一任务，HTTP/解码在独立任务，网络阻塞不会占住主循环；SD 与条带刷新仍是同步操作，不能保证硬实时采集。

地图署名常驻底部 `Map: Tianditu`，完整服务归属见[天地图](https://www.tianditu.gov.cn/)。实际发行的归属、图标与使用范围应按账号及服务要求配置；不应隐藏底图自带的审图/署名信息。日志不保存 tk、Wi-Fi 密码或地图 URL。

## 验证范围

`node scripts/test.mjs --sanitize` 中骑行数据检查 77 项、记录器检查 50 项；GPX 工具单元测试 10 项。`node scripts/test-raster.mjs` 使用外部官方软件 TJpgDec 和真实 PNGdec 通过 22 项检查，依赖说明见库内 `tests/data/raster/README.md`。主机 JPEG 软件实现与当前 ESP32-S3 SDK 的 ROM 实现不同。

主机测试覆盖有界 CSV 导入、异常点过滤、暂停/失锁断段、毫秒回绕和 CSV 输出；ESP32-S3 使用实际 SDK 编译默认配置及独立引脚覆盖配置。网络任务采用单个请求槽、指针队列交接所有权，所有 Map/SD/显示操作回到主循环。图片按签名识别 PNG/JPEG：PNGdec 处理透明注记，SDK `esp_jpeg` 解码单扫描 8-bit 基线 JPEG 底图；不支持渐进/CMYK JPEG，也不能把 JPEG 用于透明注记。256 像素 JPEG 解码临时 RGB888 缓冲约 192 KiB，使用 PSRAM 分配器。

未使用实际密钥、GNSS、SD 或显示屏做烧录实测。首次接入请检查真实瓦片编码、证书、坐标一致性、采样质量与实际剩余内存。
