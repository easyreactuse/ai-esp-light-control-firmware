# ESP32-C3 BLE RGB 灯条控制器

当前工程目标芯片为 ESP32-C3。BLE 命令可以在外接 HW-160B（8 颗 WS2812B）与独立红黄绿交通灯之间切换输出。RGB 灯板支持纯色、彩虹跑马灯和所有灯珠同色渐变的流光模式；交通灯支持常亮、闪烁和可调周期的 PWM 呼吸效果；板载 RGB 默认关闭，启用并接好电池采样电路后，仅在低电量时红色闪烁。

完整的测试命令见 [BLE_COMMANDS.md](BLE_COMMANDS.md)。

## 默认引脚

| 功能 | ESP32-C3 GPIO |
|---|---:|
| HW-160B `DIN` | GPIO4 |
| 交通灯红灯 | GPIO5 |
| 交通灯黄灯 | GPIO6 |
| 交通灯绿灯 | GPIO7 |
| 板载 RGB | GPIO8 |
| 可选电池 ADC | GPIO3 |

GPIO8 是常见 ESP32-C3 DevKit 的板载 RGB 引脚。不同厂商的小板可能没有板载 RGB，或使用其他引脚，可在 `idf.py menuconfig` → **RGB light controller configuration** 中修改。

## 红黄绿 LED 接线

三个独立 LED 默认使用 GPIO5、GPIO6、GPIO7，分别控制红、黄、绿灯。代码按高电平点亮设计：GPIO 输出约 3.3V 时灯亮，输出 0V 时灯灭。每颗 LED 都需要串联合适的限流电阻。

```text
GPIO5 ── 限流电阻 ── 红 LED 正极     红 LED 负极 ── GND
GPIO6 ── 限流电阻 ── 黄 LED 正极     黄 LED 负极 ── GND
GPIO7 ── 限流电阻 ── 绿 LED 正极     绿 LED 负极 ── GND
```

如果使用交通灯模块，必须确认它支持 3.3V 高电平控制。工作电流较大的灯需要使用三极管或 MOSFET 驱动，不能由 ESP32 GPIO 直接供电。

## HW-160B 接线

| HW-160B | 连接位置 |
|---|---|
| `DIN` | ESP32 GPIO4，建议串联 220～470Ω 电阻 |
| `4-7VDC` | 稳定的 5V 电源正极 |
| 任一 `GND` | 5V 电源负极，并与 ESP32 `GND` 共地 |
| `DOUT` | 不接；仅在串接下一块灯板时使用 |

建议使用 5V/1A 电源，并在灯条电源入口并联 470～1000µF 电解电容。ESP32 输出 3.3V 数据；线路较长或灯光异常时，在 GPIO4 与 `DIN` 之间加入 `74AHCT125` 等 3.3V→5V 电平转换器。不要把 ESP32 GPIO 直接连接 5V。

## 低电量检测（默认关闭）

软件无法仅凭供电引脚可靠知道电池电压，需要额外的电阻分压采样。单节锂电池可按以下方式连接：

```text
电池正极 ── 100kΩ ──┬── GPIO3
                    └── 100kΩ ── GND
电池负极 ───────────────── GND
```

接线确认后，在 `idf.py menuconfig` 中启用 **Enable battery voltage monitoring**。默认低电量阈值为 3300mV，连续 3 次低于阈值后板载 RGB 以 20% 亮度红色闪烁；恢复到 3450mV 后关闭。默认关闭此功能是为了防止未接 ADC 时误报警。

以上默认值按单节锂电池设计。使用其他电池、电源模块或不同分压电阻时，必须先确认最高电压不会让 GPIO 超压，并修改分压比例与阈值。

## 编译和烧录

```bash
cd /Users/roger/work/esp32/esp32s3_rgb_flow
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

如果串口仍报告芯片参数错误，执行 `idf.py fullclean` 后再次运行 `idf.py set-target esp32c3`。退出监视器使用 `Ctrl+]`。本项目使用 ESP32-C3 支持的 BLE NimBLE，不使用经典蓝牙。
