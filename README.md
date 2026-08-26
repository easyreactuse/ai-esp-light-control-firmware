# AI ESP LIGHT CONTROL · Firmware

AI ESP LIGHT CONTROL 的 ESP32-C3 BLE 灯光固件。它可以同时控制 WS2812B RGB 灯板和独立红黄绿交通灯，并与配套的 [Desktop 桌面端](https://github.com/easyreactuse/ble-light-desktop) 联动，将 Codex 任务状态转换为实体灯光提示。

> [Desktop 桌面端](https://github.com/easyreactuse/ble-light-desktop) · [完整 BLE JSON 协议](BLE_COMMANDS.md)

## 功能

- HW-160B（8 颗 WS2812B）：纯色、闪烁、彩虹跑马灯、同色渐变流光
- 红黄绿交通灯：任意组合常亮、同步闪烁、PWM 呼吸
- RGB 灯板与交通灯独立控制，也可通过一个 JSON 数组同时更新
- BLE Write、Write Without Response、Read 和 Notify
- 可选电池电压监测，低电量时使用板载 RGB 红灯报警
- GPIO、BLE 设备名和电量阈值可通过 `menuconfig` 调整

## 硬件与默认引脚

当前目标芯片为 **ESP32-C3**，使用 NimBLE，不使用经典蓝牙。

| 功能 | 默认 GPIO |
|---|---:|
| HW-160B `DIN` | GPIO4 |
| 交通灯红灯 | GPIO5 |
| 交通灯黄灯 | GPIO6 |
| 交通灯绿灯 | GPIO7 |
| 板载 RGB | GPIO8 |
| 可选电池 ADC | GPIO3 |

GPIO8 是常见 ESP32-C3 DevKit 的板载 RGB 引脚。不同厂商开发板可能没有板载 RGB，或使用其他引脚，可在 `idf.py menuconfig` → **RGB light controller configuration** 中修改。

### 红黄绿 LED 接线

代码按高电平点亮设计，每颗 LED 都需要串联合适的限流电阻。

```text
GPIO5 ── 限流电阻 ── 红 LED 正极     红 LED 负极 ── GND
GPIO6 ── 限流电阻 ── 黄 LED 正极     黄 LED 负极 ── GND
GPIO7 ── 限流电阻 ── 绿 LED 正极     绿 LED 负极 ── GND
```

交通灯模块必须支持 3.3V 高电平控制。工作电流较大的灯需要使用三极管或 MOSFET 驱动，不能由 ESP32 GPIO 直接供电。

### HW-160B 接线

| HW-160B | 连接位置 |
|---|---|
| `DIN` | ESP32 GPIO4，建议串联 220～470Ω 电阻 |
| `4-7VDC` | 稳定的 5V 电源正极 |
| 任一 `GND` | 5V 电源负极，并与 ESP32 `GND` 共地 |
| `DOUT` | 不接；仅在串接下一块灯板时使用 |

建议使用 5V/1A 电源，并在灯板电源入口并联 470～1000µF 电解电容。线路较长或灯光异常时，可在 GPIO4 与 `DIN` 之间加入 `74AHCT125` 等 3.3V→5V 电平转换器。不要把 ESP32 GPIO 直接连接 5V。

## 快速开始

需要已安装并激活 ESP-IDF 环境。

```bash
git clone https://github.com/easyreactuse/esp32s3_rgb_flow.git
cd esp32s3_rgb_flow
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

请把串口设备名替换为本机实际端口。若串口仍报告芯片参数错误，执行 `idf.py fullclean`，然后再次设置目标芯片。退出监视器使用 `Ctrl+]`。

## BLE 控制

| 项目 | 值 |
|---|---|
| 广播名称 | `ESP-TRAFFIC-LIGHT` |
| Service UUID | `7b9a0001-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| Characteristic UUID | `7b9a0002-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| 数据格式 | UTF-8 JSON，最大 255 字节 |

示例：RGB 流光与黄色交通灯闪烁同时运行。

```json
[{"output":"8_BIT_RGB","cmd":"flow","brightness":25,"speed":25},{"output":"TRAFFIC_LIGHT","light":["YELLOW"],"blink_ms":500}]
```

命令字段、取值范围、返回结果和更多示例见 [BLE_COMMANDS.md](BLE_COMMANDS.md)。手机临时测试可使用 nRF Connect 或 LightBlue；日常控制及 Codex 状态联动推荐使用 [AI ESP LIGHT CONTROL Desktop](https://github.com/easyreactuse/ble-light-desktop)。

## 可选低电量检测

软件无法仅凭供电引脚可靠判断电池电压，需要额外的电阻分压采样。单节锂电池可按以下方式连接：

```text
电池正极 ── 100kΩ ──┬── GPIO3
                    └── 100kΩ ── GND
电池负极 ───────────────── GND
```

接线确认后，在 `idf.py menuconfig` 中启用 **Enable battery voltage monitoring**。默认低电量阈值为 3300mV，连续 3 次低于阈值后板载 RGB 以 20% 亮度红色闪烁；恢复到 3450mV 后关闭。

使用其他电池、电源模块或不同分压电阻时，必须先确认最高电压不会让 GPIO 超压，并相应修改分压比例与阈值。

## 项目关系

| 项目 | 用途 |
|---|---|
| **AI ESP LIGHT CONTROL · Firmware**（本仓库） | ESP32-C3 固件，驱动灯光并实现 BLE JSON 协议 |
| [**AI ESP LIGHT CONTROL · Desktop**](https://github.com/easyreactuse/ble-light-desktop) | BLE 设备管理、手动控制和 Codex Hooks 联动 |
