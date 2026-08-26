# BLE 灯条控制协议

ESP32 使用 BLE（低功耗蓝牙），ESP32-S3 和 ESP32-C3 均可使用。默认不要求配对，附近设备都能控制，适合当前本地测试；若用于公开环境，后续应增加绑定或应用层鉴权。

## 连接参数

| 项目 | 值 |
|---|---|
| 广播名称 | `ESP-TRAFFIC-LIGHT` |
| Service UUID | `7b9a0001-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| Characteristic UUID | `7b9a0002-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| Characteristic 权限 | Read、Write、Write Without Response、Notify |
| 数据格式 | UTF-8 JSON，不含结尾换行或 `\0` |
| 单条命令最大长度 | 255 字节 |

手机可使用 nRF Connect 或 LightBlue 测试。连接设备后找到上述 Characteristic，用 UTF-8/Text 方式写入命令。建议订阅 Notify；每次写入后也可 Read 获取最后一次响应。

## 命令

`output` 为可选输出类型。省略时默认为 `8_BIT_RGB`，因此原有命令无需修改。
显式使用 RGB 灯板时也可以传入 `"output":"8_BIT_RGB"`。

### 同时执行多个输出

顶层可以传入非空 JSON 数组。数组中的命令会先全部校验，再在同一时刻生效；任何一条无效时整组都不会执行。

例如，让 8 位 RGB 灯板运行流光，同时让交通灯黄灯闪烁：

```json
[{"output":"8_BIT_RGB","cmd":"flow","brightness":25,"speed":25},{"output":"TRAFFIC_LIGHT","light":["YELLOW"],"blink_ms":500}]
```

- 一个数组可以同时控制 `8_BIT_RGB` 和 `TRAFFIC_LIGHT`，两套输出互不清除。
- 同一输出在数组中出现多次时，以最后一条为准。
- 两类输出始终互不干扰；发送单个对象时，只更新对象指定的输出，另一类输出保持原状态。
- RGB 使用 `{"cmd":"off"}` 独立关闭；交通灯使用 `{"output":"TRAFFIC_LIGHT","light":[]}` 独立关闭。
- 顶层数组与单对象共用 255 字节长度限制。

### 全部灯设置为相同颜色

```json
{"cmd":"solid","r":255,"g":80,"b":0,"brightness":25,"blink_ms":0}
```

- `r`、`g`、`b`：必填，范围 0～255。
- `brightness`：必填，范围 0～100，表示百分比。
- `blink_ms`：可选，默认 0。0 表示常亮；大于 0 时表示亮、灭各持续多少毫秒。例如 500 表示亮 500 ms、灭 500 ms。

### 彩虹跑马灯

```json
{"cmd":"chase","brightness":25,"step_ms":150}
```

- `brightness`：可选，范围 0～100，默认 25。
- `step_ms`：可选，范围 20～5000，表示移动一步的时间，默认 150 ms。
- 8 颗灯保持不同彩虹颜色，并依次移动。

### 流光

```json
{"cmd":"flow","brightness":25,"speed":25}
```

- `brightness`：可选，范围 0～100，默认 25。
- `speed`：可选，范围 1～100，默认 25；数值越大，渐变越快。
- 所有灯珠始终保持相同颜色，并同步、平滑地遍历全部色相，避免不同颜色投射到同一位置后混合成白色。

### 关闭外接灯条

```json
{"cmd":"off"}
```

该命令只关闭外接 HW-160B，不影响低电量报警逻辑。

### 红黄绿交通灯

```json
{"output":"TRAFFIC_LIGHT","light":["RED","GREEN"],"blink_ms":500}
```

- `output`：必须为 `TRAFFIC_LIGHT`。
- `light`：必填数组，可包含 `RED`、`YELLOW`、`GREEN`。可以同时选择多个；空数组 `[]` 表示全部关闭。
- `blink_ms`：可选，默认 0。0 表示常亮；大于 0 时，数组内的灯同步亮、灭，各持续指定的毫秒数。
- `effect`：可选，可设为 `steady`、`blink` 或 `breathe`。省略时保持兼容：`blink_ms > 0` 为闪烁，否则为常亮。
- `period_ms`：呼吸效果的完整渐亮、渐暗周期，范围 400～20000 ms，默认 1800 ms。
- 三个交通灯采用高电平点亮：选中的灯对应 GPIO 输出约 3.3V，未选中的灯输出低电平。
- 交通灯与 GPIO4 上的 RGB 灯板状态相互独立，控制或关闭其中一类不会改变另一类。

绿灯以 1.8 秒周期呼吸：

```json
{"output":"TRAFFIC_LIGHT","light":["GREEN"],"effect":"breathe","period_ms":1800}
```

`period_ms` 越小呼吸越快，越大越慢。呼吸效果由 ESP32 LEDC PWM 产生，GPIO 仍输出 0/3.3V 脉冲，不是模拟电压输出。

常亮黄灯：

```json
{"output":"TRAFFIC_LIGHT","light":["YELLOW"],"blink_ms":0}
```

关闭三个交通灯：

```json
{"output":"TRAFFIC_LIGHT","light":[]}
```

## 返回结果

成功示例：

```json
{"ok":true,"mode":"solid"}
```

交通灯成功示例：

```json
{"ok":true,"output":"TRAFFIC_LIGHT"}
```

数组成功示例（`outputs` 为输出位掩码：RGB=1、交通灯=2）：

```json
{"ok":true,"count":2,"outputs":3}
```

失败示例：

```json
{"ok":false,"error":"invalid solid parameter"}
```

GATT 写操作本身成功只表示命令已送达；参数是否合法以 Notify 或随后 Read 得到的 JSON 为准。
