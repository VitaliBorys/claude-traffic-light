# Claude Traffic Light

A physical "traffic light" for Claude Code usage. A small Node.js proxy sits
between Claude Code and `api.anthropic.com`, reads the rate-limit headers
Anthropic sends back, and serves them as JSON. An ESP32-C3 board polls that
JSON over WiFi and drives 3 LEDs + a tiny OLED to show how close you are to
your 5-hour and 7-day usage limits.

- 🟢 green — under 75% on both windows
- 🟡 yellow — either window ≥ 75%
- 🔴 solid red — either window ≥ 100%, or Anthropic reports you're actively
  rate-limited/exceeded
- 🟡 blinking (1s on / 1s off) — offline / no data / can't reach the proxy

The OLED cycles every 30 seconds between the 5h view and the 7d view, each
showing percentage used and time until that window resets.

## Repo layout

```
claude-traffic-light-proxy.js      # the proxy (no dependencies, just Node's http/https)
esp-idf/
  hw_test_c3_oled/                 # no-WiFi bring-up test: cycles LEDs + OLED
  claude_traffic_light_c3_oled/    # the real app: WiFi + polling + full display
```

## Hardware

Built around an ESP32-C3 dev board with a built-in 0.42" SSD1306 OLED
(72x40, "ABRobot / 01Space"-style boards — the ones where the OLED is
already wired to the board, not a separate module), plus an external
3-LED "traffic light" module (common-cathode, active-high signal lines).

| Signal              | ESP32-C3 pin | Notes                              |
|---------------------|--------------|-------------------------------------|
| OLED SDA            | GPIO5        | on-board, no wiring needed          |
| OLED SCL            | GPIO6        | on-board, no wiring needed          |
| Traffic light RED    | GPIO3        | wire to the module's red input      |
| Traffic light YELLOW | GPIO4        | wire to the module's yellow input   |
| Traffic light GREEN  | GPIO10       | wire to the module's green input    |
| Traffic light GND    | any GND pin  | common ground with the ESP32-C3     |

GPIO8 (onboard status LED) and GPIO9 (BOOT button) are already in use by
the board itself — left alone.

If your traffic-light module is instead common-anode / active-low, flip
`ACTIVE_HIGH` to `false` near the top of `main.c` in both projects.

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/) v6.0.1+, with `idf.py` on your PATH
- Node.js (proxy has zero external dependencies)
- The ESP32-C3 connected over USB (native USB Serial/JTAG — no separate
  USB-UART adapter needed on these boards)

## 1. Run the proxy

```
node claude-traffic-light-proxy.js
```

This listens on `:8787` and prints the IP address(es) to use for the
ESP32's config. Point your Claude Code sessions at it:

```
export ANTHROPIC_BASE_URL=http://127.0.0.1:8787
```

`GET /util` on the proxy returns the current state as JSON — useful for a
quick sanity check with `curl http://localhost:8787/util`.

Keep this running in the background; the device polls it every 20s. The
proxy only forwards to `api.anthropic.com` and doesn't store or transmit
anything elsewhere — but your OAuth token does pass through it, so only
run it on a trusted local network.

## 2. Flash the bring-up test (recommended first)

Verifies wiring before dealing with WiFi at all — cycles RED → YELLOW →
GREEN once per second and shows the active color + a counter on the OLED.

```
cd esp-idf/hw_test_c3_oled
idf.py set-target esp32c3
idf.py -p COM10 build flash monitor
```

(replace `COM10` with your board's serial port — `/dev/ttyACM0` etc. on
Linux/macOS)

Pass criteria: all three LEDs light in order, OLED shows "TEST" / the
color name / an incrementing counter.

## 3. Configure and flash the real app

```
cd esp-idf/claude_traffic_light_c3_oled
idf.py set-target esp32c3
idf.py menuconfig   # under "Example Configuration": WiFi SSID, WiFi password, proxy /util URL
idf.py -p COM10 build flash monitor
```

The proxy URL should point at your PC's LAN IP (printed by the proxy on
startup), e.g. `http://192.168.1.50:8787/util` — `localhost` won't work
since the ESP32 is a separate device on the network.

WiFi credentials and the proxy URL live only in the generated `sdkconfig`
file (via `idf.py menuconfig`), which is git-ignored — they're never
committed to source.

## Notes

- Both ESP-IDF projects pull in [u8g2](https://github.com/olikraus/u8g2)
  automatically via the component manager (`main/idf_component.yml`) — no
  manual library install needed.
- `MODE_OFFLINE` (no data / proxy unreachable) blinks yellow 1s on/1s off.
  A dead solid state — no blink at all — usually means the OLED/I2C wiring
  itself is the problem, not WiFi/proxy connectivity.
