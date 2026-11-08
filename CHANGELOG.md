# Changelog

Newest first.

## 1.1.0 - 2026-11-08

- **Valve entities are created by the component.** `valves:` and the per-entity
  platforms are gone; set `max_valves` (default 4) instead. Pairing detects the
  model from the valve's hardware id and only that model's zones are shown.
  Empty slots are hidden, and the gateway restarts about 20 s after a pairing
  or unpairing to apply it. **Breaking:** 1.0.0 configs must drop `valves:`
  and every `platform: diivoo` entity, and valves have to be paired again.
- **The LED can blink while pairing** (`pairing_light`, `pairing_effect`).
- **The WG03 power quirks are handled in the example:** 160 MHz, 8.5 dB Wi-Fi
  TX power and no brownout reset, without which it boot-loops. OTA on the
  gateway's mains supply fails (see the README); on adapter power it works.
- Runs on a real WG03; still not confirmed against a real valve.

## 1.0.0 - 2026-11-07

- **First release.** ESPHome external component for the DIIVOO WG03 gateway,
  ported from the hassio-diivoo2mqtt protocol.
  - Pairing, plus open and close for each zone. The valve's own run time is
    used as a safety cut-off.
  - Per zone: remaining time and water used by the last run. Per valve:
    battery, signal, online status, id and model.
  - Pairing-mode switch, and refresh and unpair buttons.
  - Schedules are deliberately left to Home Assistant: valves are told they
    have none.
  - Validated and compiled. Not yet confirmed against real hardware.
