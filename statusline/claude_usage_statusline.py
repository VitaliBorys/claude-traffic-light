#!/usr/bin/env python3
"""
Claude Code statusline + MQTT publisher for the usage-indicator project.

Wired up via Claude Code's official `statusLine` feature:
  - Claude Code pipes session JSON (incl. rate_limits) to this script on stdin
  - `refreshInterval` in settings.json re-runs it on a timer, even while idle
  - This script prints a short status line AND publishes the current usage
    state to a local MQTT broker for the ESP32 devices (matrix, traffic light,
    future strips) to consume.

No proxy, no token handling, no direct calls to Anthropic — all usage data
comes straight from Claude Code itself.

Multiple concurrent sessions: if you run more than one Claude Code session,
every session's statusline publishes to the same shared, retained topic.
Each session only knows the rate_limits from its OWN last API response, so
without any coordination, whichever session's 15s timer fires last "wins" —
even if its data is staler than what's already retained. See
`should_publish()` below for how this is avoided: usage only climbs within
a window until it resets, so a session is only allowed to overwrite the
retained state if its own numbers are >= what's already there, unless a
window boundary (resets_at) has actually moved, which means a real reset
happened and the new snapshot is trusted unconditionally.

Deploy: copy this file to ~/.claude/claude_usage_statusline.py (that's the
path Claude Code actually runs — this repo copy is the version-controlled
source; re-copy after editing either one).

Install once:
    pip install paho-mqtt

Wire into ~/.claude/settings.json (Windows: %USERPROFILE%\\.claude\\settings.json):
    {
      "statusLine": {
        "type": "command",
        "command": "python C:/Users/YOURNAME/.claude/claude_usage_statusline.py",
        "refreshInterval": 15
      }
    }
(Use forward slashes in the path, per Claude Code's Windows notes.)
"""

import json
import os
import sys
import time
from datetime import datetime, timezone

# ---- config ---------------------------------------------------------------

MQTT_HOST = "192.168.0.68"     # your local Mosquitto broker
MQTT_PORT = 1883
MQTT_TOPIC = "claude/usage/state"
MERGE_CHECK_TIMEOUT_S = 1.0    # how long to wait for the retained message before publishing

# Daily is a self-imposed budget, not an official Anthropic limit:
# "don't burn more than this fraction of the weekly budget in one day."
DAILY_BUDGET_FRACTION = 0.20   # 1/5 of the week per day

STATE_FILE = os.path.join(os.path.expanduser("~"), ".claude", "claude_usage_daily_state.json")

# ---- helpers ----------------------------------------------------------------


def load_daily_state():
    try:
        with open(STATE_FILE, "r") as f:
            return json.load(f)
    except Exception:
        return {"date": None, "baseline_pct": 0.0}


def save_daily_state(state):
    try:
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        with open(STATE_FILE, "w") as f:
            json.dump(state, f)
    except Exception:
        pass  # non-fatal — worst case daily resets on next successful write


def effective_window(window):
    """
    Given a rate_limits.<window> dict ({used_percentage, resets_at} or None),
    return the used_percentage, correcting for a reset that has already
    passed locally but that Claude Code hasn't re-fetched from the server yet.
    Returns (used_percentage_or_None, resets_at_or_None, was_force_reset).
    """
    if not window:
        return None, None, False

    used = window.get("used_percentage")
    resets_at = window.get("resets_at")

    if resets_at is not None and time.time() > resets_at:
        # The window's reset time has passed — trust the clock over the
        # possibly-stale cached percentage from the last real API response.
        return 0.0, None, True  # next resets_at is unknown until a new request happens

    return used, resets_at, False


def compute_daily(seven_day_used, was_weekly_reset):
    today = datetime.now().astimezone().date().isoformat()
    state = load_daily_state()

    if was_weekly_reset and seven_day_used is not None:
        # Weekly window just reset — daily budget starts fresh from zero too.
        state = {"date": today, "baseline_pct": 0.0}
        save_daily_state(state)
    elif state.get("date") != today:
        if seven_day_used is None:
            # No real data yet this tick (e.g. very first statusline call of
            # the session, before any API response arrived) — don't lock in
            # a wrong baseline. Wait for a tick that actually has data.
            return None
        # First run of a new local day with real data — snapshot current
        # weekly usage as the baseline; daily = usage accumulated since.
        state = {"date": today, "baseline_pct": seven_day_used}
        save_daily_state(state)

    if seven_day_used is None:
        return None

    delta_pct = max(0.0, seven_day_used - state["baseline_pct"])
    daily_pct = min(100.0, (delta_pct / (DAILY_BUDGET_FRACTION * 100.0)) * 100.0)
    return round(daily_pct, 1)


def should_publish(new_payload, retained_payload):
    """
    True if new_payload is safe to publish over whatever's currently
    retained. Guards against two concurrent sessions racing to publish to
    the same shared topic (see module docstring).
    """
    if retained_payload is None:
        return True

    for w in ("five_hour", "seven_day"):
        new_resets_at = (new_payload.get(w) or {}).get("resets_at")
        old_resets_at = (retained_payload.get(w) or {}).get("resets_at")
        if new_resets_at != old_resets_at:
            return True  # window boundary moved -> a real reset happened; trust this snapshot

    for w in ("five_hour", "seven_day"):
        new_used = (new_payload.get(w) or {}).get("used_pct")
        old_used = (retained_payload.get(w) or {}).get("used_pct")
        if new_used is None or old_used is None:
            continue
        if new_used < old_used:
            return False  # would regress usage within the same window -> this session is stale

    return True


def publish_mqtt(payload):
    """
    Reads the currently-retained state and only overwrites it if `payload`
    isn't a stale regression (should_publish). Fails open on any MQTT error
    (including the merge-check read) — publishing a possibly-racy update is
    better than the device silently going stale forever on a broker hiccup.
    """
    try:
        import paho.mqtt.client as mqtt

        retained = {}

        def on_message(client, userdata, msg):
            try:
                retained["payload"] = json.loads(msg.payload)
            except Exception:
                pass

        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        client.on_message = on_message
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=int(MERGE_CHECK_TIMEOUT_S) + 5)
        client.subscribe(MQTT_TOPIC)
        client.loop_start()
        time.sleep(MERGE_CHECK_TIMEOUT_S)
        client.loop_stop()

        if should_publish(payload, retained.get("payload")):
            client.publish(MQTT_TOPIC, payload=json.dumps(payload), qos=1, retain=True)
        client.disconnect()
        return True
    except Exception as e:
        # Don't let a broker hiccup break the statusline itself
        print(f"[mqtt error: {e}]", file=sys.stderr)
        return False


def bar(pct, width=10):
    if pct is None:
        return "?" * width
    filled = int(round(pct / 100.0 * width))
    return "#" * filled + "-" * (width - filled)


# ---- main -------------------------------------------------------------------


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        print("[usage: bad input]")
        return

    model = data.get("model", {}).get("display_name", "Claude")
    rate_limits = data.get("rate_limits") or {}

    five_hour_used, five_hour_reset, five_reset_flag = effective_window(rate_limits.get("five_hour"))
    seven_day_used, seven_day_reset, week_reset_flag = effective_window(rate_limits.get("seven_day"))
    daily_used = compute_daily(seven_day_used, week_reset_flag)

    payload = {
        "five_hour": {"used_pct": five_hour_used, "resets_at": five_hour_reset},
        "seven_day": {"used_pct": seven_day_used, "resets_at": seven_day_reset},
        "daily": {"used_pct": daily_used},
        "ts": int(time.time()),
    }

    if five_hour_used is not None or seven_day_used is not None:
        publish_mqtt(payload)

    # what actually shows in the Claude Code status bar
    parts = [f"[{model}]"]
    if five_hour_used is not None:
        parts.append(f"5h {bar(five_hour_used)} {five_hour_used:.0f}%")
    if daily_used is not None:
        parts.append(f"day {daily_used:.0f}%")
    if seven_day_used is not None:
        parts.append(f"wk {bar(seven_day_used)} {seven_day_used:.0f}%")

    print(" | ".join(parts))


if __name__ == "__main__":
    main()
