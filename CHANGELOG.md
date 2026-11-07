# Changelog

Newest first.

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
