# BLE Light Control Protocol

The firmware uses Bluetooth Low Energy and is compatible with ESP32-C3 and ESP32-S3. Pairing is not required by default, so any nearby device can control the lights. This is convenient for local testing; add bonding or application-level authentication before deploying in a public environment.

## Connection parameters

| Item | Value |
|---|---|
| Advertised name | `ESP-TRAFFIC-LIGHT` |
| Service UUID | `7b9a0001-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| Characteristic UUID | `7b9a0002-6d4f-4f4b-9f2a-1c5e7a3d1000` |
| Characteristic operations | Read, Write, Write Without Response, Notify |
| Payload format | UTF-8 JSON without a trailing newline or `\0` |
| Maximum command size | 255 bytes |

For manual testing, connect with nRF Connect or LightBlue, locate the characteristic, and write commands as UTF-8 text. Subscribe to Notify for immediate responses. You can also Read the characteristic after each write to retrieve the most recent response.

## Commands

The `output` field selects an output type. When omitted, it defaults to `8_BIT_RGB`, so earlier RGB commands remain compatible. RGB commands may also specify `"output":"8_BIT_RGB"` explicitly.

### Updating multiple outputs atomically

The top-level payload may be a non-empty JSON array. Every command is validated before any output changes, and all valid commands take effect together. If one item is invalid, none of them run.

This example starts RGB color flow and blinks the yellow traffic light:

```json
[{"output":"8_BIT_RGB","cmd":"flow","brightness":25,"speed":25},{"output":"TRAFFIC_LIGHT","light":["YELLOW"],"blink_ms":500}]
```

- `8_BIT_RGB` and `TRAFFIC_LIGHT` are independent and can run simultaneously.
- If the same output appears more than once in an array, the last command for that output wins.
- A single-object command changes only its selected output; the other output keeps its current state.
- Use `{"cmd":"off"}` to turn off RGB independently.
- Use `{"output":"TRAFFIC_LIGHT","light":[]}` to turn off the traffic lights independently.
- Arrays and single objects share the same 255-byte payload limit.

### Solid color

```json
{"cmd":"solid","r":255,"g":80,"b":0,"brightness":25,"blink_ms":0}
```

- `r`, `g`, `b`: required integers from 0 to 255.
- `brightness`: required integer from 0 to 100, expressed as a percentage.
- `blink_ms`: optional, default `0`. Zero means steady. A positive value is the duration of both the on and off phase. For example, `500` means 500 ms on and 500 ms off.

### Rainbow chase

```json
{"cmd":"chase","brightness":25,"step_ms":150}
```

- `brightness`: optional integer from 0 to 100; default `25`.
- `step_ms`: optional integer from 20 to 5000; default `150`. It controls how long each movement step lasts.
- The eight LEDs keep different rainbow colors and shift one position per step.

### Synchronized color flow

```json
{"cmd":"flow","brightness":25,"speed":25}
```

- `brightness`: optional integer from 0 to 100; default `25`.
- `speed`: optional integer from 1 to 100; default `25`. Higher values transition faster.
- All LEDs always show the same color and move smoothly through the hue spectrum together, avoiding the white light that can result from mixing different projected colors.

### Turn off the RGB board

```json
{"cmd":"off"}
```

This turns off only the external HW-160B. It does not change the low-battery warning logic.

### Red, yellow, and green traffic lights

```json
{"output":"TRAFFIC_LIGHT","light":["RED","GREEN"],"brightness":60,"blink_ms":500}
```

- `output`: required and must be `TRAFFIC_LIGHT`.
- `light`: required array containing any combination of `RED`, `YELLOW`, and `GREEN`. Multiple lights may be selected. An empty array turns them all off.
- `brightness`: optional integer from 0 to 100, expressed as a percentage; default `100`.
- `blink_ms`: optional, default `0`. Zero means steady. A positive value sets both the on and off duration in milliseconds.
- `effect`: optional and may be `steady`, `blink`, or `breathe`. When omitted, backward-compatible behavior applies: `blink_ms > 0` selects blinking; otherwise the output is steady.
- `period_ms`: complete fade-in/fade-out period for `breathe`, from 400 to 20000 ms; default `1800`.
- The traffic-light GPIO outputs are active-high: selected lights receive approximately 3.3 V and unselected lights receive 0 V.
- Traffic-light state is independent of the RGB board on GPIO4.

Green breathing with a 1.8-second period:

```json
{"output":"TRAFFIC_LIGHT","light":["GREEN"],"brightness":60,"effect":"breathe","period_ms":1800}
```

A smaller `period_ms` breathes faster; a larger value breathes slower. `brightness` sets the peak duty cycle for every effect. The effect uses ESP32 LEDC PWM. The GPIO still produces 0/3.3 V pulses rather than an analog voltage.

Steady yellow:

```json
{"output":"TRAFFIC_LIGHT","light":["YELLOW"],"blink_ms":0}
```

Turn off all traffic lights:

```json
{"output":"TRAFFIC_LIGHT","light":[]}
```

## Responses

Successful RGB command:

```json
{"ok":true,"mode":"solid"}
```

Successful traffic-light command:

```json
{"ok":true,"output":"TRAFFIC_LIGHT"}
```

Successful array command. `outputs` is a bit mask where RGB is `1` and traffic lights are `2`:

```json
{"ok":true,"count":2,"outputs":3}
```

Error response:

```json
{"ok":false,"error":"invalid solid parameter"}
```

A successful GATT write only confirms that the command reached the characteristic. Use the Notify response or a subsequent Read to determine whether the firmware accepted and executed the parameters.
