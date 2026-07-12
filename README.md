# Claude Traffic Light

A physical "traffic light" for Claude Code usage. `~/.claude/claude_usage_statusline.py`
is wired into Claude Code's `statusLine` hook: it reads the `rate_limits`
Claude Code already has for the session and publishes them as retained JSON
to a local MQTT broker (topic `claude/usage/state`) — no proxy, no token
handling, no direct calls to Anthropic. An ESP32-C3 board subscribes to that
topic over WiFi and drives 3 LEDs + a tiny OLED to show how close you are to
your 5-hour and 7-day usage limits.

- 🟢 green — under 75% on both windows
- 🟡 yellow — either window ≥ 75%
- 🔴 solid red — either window ≥ 100%
- 🟡 blinking (1s on / 1s off) — offline / no data / broker unreachable / no
  message received in the last 60s

The OLED cycles every 30 seconds between the 5h view and the 7d view, each
showing percentage used and time until that window resets.

## Repo layout

```
claude-traffic-light-proxy.js      # legacy HTTP proxy (no dependencies, just Node's http/https) — not used by claude_traffic_light_c3_oled anymore, kept for reference/other uses
esp-idf/
  hw_test_c3_oled/                 # no-WiFi bring-up test: cycles LEDs + OLED
  claude_traffic_light_c3_oled/    # the real app: WiFi + MQTT + full display
```

The statusline publisher itself (`claude_usage_statusline.py`) lives outside
this repo, in `~/.claude/`, alongside the rest of your Claude Code config —
see [Set up the usage-state source](#1-set-up-the-usage-state-source-mqtt)
below.

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

GPIO8 (onboard status LED) doubles as a WiFi indicator: blinking while
(re)connecting, solid once associated. GPIO9 (BOOT button) is already in
use by the board itself — left alone.

If your traffic-light module is instead common-anode / active-low, flip
`ACTIVE_HIGH` to `false` near the top of `main.c` in both projects.

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/) v6.0.1+, with `idf.py` on your PATH
- A local MQTT broker reachable from both your PC and the ESP32 (e.g.
  [Mosquitto](https://mosquitto.org/), on your LAN — no auth/TLS required
  for a trusted home network)
- Python 3 + `pip install paho-mqtt` (for the statusline publisher)
- The ESP32-C3 connected over USB (native USB Serial/JTAG — no separate
  USB-UART adapter needed on these boards)

## 1. Set up the usage-state source (MQTT)

Point Claude Code's statusline at `claude_usage_statusline.py`. In
`~/.claude/settings.json` (`%USERPROFILE%\.claude\settings.json` on
Windows):

```json
{
  "statusLine": {
    "type": "command",
    "command": "python C:/Users/YOURNAME/.claude/claude_usage_statusline.py",
    "refreshInterval": 15
  }
}
```

Edit `MQTT_HOST` near the top of the script to point at your broker's LAN
IP. Every `refreshInterval` seconds (and on every real prompt), it reads
the `rate_limits` Claude Code hands it and publishes a retained JSON message
to `claude/usage/state`:

```json
{"five_hour": {"used_pct": 31, "resets_at": 1783893600},
 "seven_day":  {"used_pct": 17, "resets_at": 1784152800},
 "daily":      {"used_pct": 5.0},
 "ts": 1783890405}
```

No proxy, no token handling — this data comes straight from Claude Code
itself. You can sanity-check it's flowing with any MQTT client, e.g.
`mosquitto_sub -h <broker-ip> -t claude/usage/state -v`.

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
idf.py menuconfig   # under "Example Configuration": WiFi SSID, WiFi password, MQTT broker URI, MQTT topic
idf.py -p COM10 build flash monitor
```

The MQTT broker URI should point at the broker's LAN IP, e.g.
`mqtt://192.168.1.50:1883` — `localhost` won't work since the ESP32 is a
separate device on the network. The topic defaults to `claude/usage/state`,
matching the statusline publisher.

WiFi credentials and the MQTT broker URI/topic live only in the generated
`sdkconfig` file (via `idf.py menuconfig`), which is git-ignored — they're
never committed to source.

## Notes

- Both ESP-IDF projects pull in [u8g2](https://github.com/olikraus/u8g2)
  automatically via the component manager (`main/idf_component.yml`) — no
  manual library install needed. `claude_traffic_light_c3_oled` also pulls
  in the `espressif/mqtt` managed component.
- The MQTT topic is retained (QoS 1), so the device shows the last known
  state immediately on connect/reconnect, even if the statusline publisher
  isn't actively running.
- Since `resets_at` is an absolute Unix timestamp rather than a countdown,
  the firmware derives "now" from each message's `ts` field plus elapsed
  device uptime since it arrived — no SNTP/RTC needed.
- `MODE_OFFLINE` (no data / broker unreachable / stale) blinks yellow 1s
  on/1s off — triggered if no MQTT message arrives for 60s. A dead solid
  state — no blink at all — usually means the OLED/I2C wiring itself is the
  problem, not WiFi/MQTT connectivity.
