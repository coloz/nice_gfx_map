# 04：GNSS 轨迹记录

默认 SIM 每秒沿固定圆形轨迹产生位置，屏幕显示 FIX、速度和是否跟随。地图背景是过程生成测试地理图，不是真实道路。定位、轨迹与跟随使用 `setLocation`、`appendTrack`、`setFollow`。

切真实 GNSS 只替换位置来源，底图仍是 SYNTH；联网真实底图需按上级 README 更换实际传输/解码管线，不能只改 provider。

输入与操作：

- `m` 切换 SIM / UART；`c` 清除轨迹；其余通用键见上级 README。
- UART 使用 `HardwareSerial(1)`，默认 9600；S3 RX16/TX17，经典 ESP32 RX32/TX33，避免常见 PSRAM 板对 16/17 的占用。接 3.3V GNSS TX→配置的 RX，共地；按需改 `NICE_MAP_GNSS_RX/TX/BAUD`。
- 定义 `NICE_MAP_REAL_GNSS=1` 可默认进入真实模式。真实模式只接收校验有效的 NMEA RMC/GGA，不用模拟数据补缺。

期望结果：SIM 有移动定位和轨迹；切 UART 未接设备时显示 NO FIX，速度归零。切换数据来源会清轨迹，避免把模拟尾点与远处真实位置连成跨区折线。收到有效定位后恢复；超过 3 秒没有有效定位会清掉定位图标并归零速度。RMC/GGA 的状态、航向与速度来自 `NmeaParser`，不是磁罗盘测量。

真实数据接点：串口读取循环可替换为板载 GNSS UART；若从其他协议接入，可将实际 `GnssFix` 交给 `useFix()`。所有 Map 调用留在 loop owner task，不在 UART ISR 中操作。

依赖：ESP32/S3、NICE_GFX、ILI9341、建议 PSRAM；真实模式另需 GNSS。无网络/key/PNGdec 依赖。
