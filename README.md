# esphome-diivoo

ESPHome external component that turns the **DIIVOO WG03** irrigation gateway
into a fully local controller for DIIVOO battery water timers. The gateway's
own ESP32 drives its CMT2300A sub-GHz radio and talks to Home Assistant over
the native ESPHome API: no DIIVOO/HomGar cloud, no app, no add-on, no extra
hardware.

Each valve zone shows up in Home Assistant as a native `valve` entity. The
entities are created by the component itself: pair a valve and the gateway
detects its model and shows exactly its zones. Home Assistant does the
scheduling, so the valves' built-in schedules are not used.

> **Status: experimental.** The firmware runs on a real WG03 (Wi-Fi, API,
> OTA). The radio code is a faithful port of a protocol that works in
> [hassio-diivoo2mqtt](https://github.com/Technerd-SG/hassio-diivoo2mqtt),
> but this ESPHome build has not yet been confirmed against a real valve.
> Reports, especially logs at `VERBOSE`, are very welcome.

## Supported hardware

| Device | Zones | Hardware id |
|---|---|---|
| WT-07W | 1 | `0x0D1F` |
| WT-09W | 2 | `0x0E20` |
| WT-11W(1) | 3 | `0x0F25` |
| WT-13W | 4 | `0x1026` |

**Gateway: WG03 only.** It is a classic ESP32 (4 MB) with a CMT2300A radio.
Other HomGar/RainPoint gateways use the same radio chip but may be wired
differently. Open yours and compare it with the pin table below before
flashing.

| Signal | GPIO | Notes |
|---|---|---|
| SCLK | 27 | |
| SDIO | 19 | one data line, direction switched per transfer |
| CSB | 25 | register chip-select |
| FCSB | 26 | FIFO chip-select |
| Radio power | 21 | active low |
| Radio IRQ | 18 | packet done, rising edge, pull-down |
| LED | 14 | active low |
| Button | 13 | pull-up |

## Features

- **Open and close each zone.** The run time comes from a per-zone *Duration*
  number (minutes). The valve also uses that duration as its own safety
  cut-off: it closes by itself even if the gateway or Home Assistant goes away.
- **Live state:** running or idle, remaining time, and the water used by the
  last run.
- **Valve health:** battery (OK or low), signal strength, online status, and
  the valve id and model.
- **Entities appear by themselves.** Up to `max_valves` slots of four zones
  are built in. Pairing records the model from the valve's hardware id, and
  only that model's zones are shown; empty slots stay hidden.
- **Pairing from Home Assistant** with a switch, or with the gateway's button,
  with the LED blinking while pairing mode is on.
- **Silent valves are pinged.** A valve not heard from for 5 hours is woken
  up, and it shows offline after 12 hours.
- **Pairing survives reflashes:** it is stored in flash.

Not included: schedules, rain delay and mist mode. Use Home Assistant
automations instead.

## Installation

### 1. Back up the stock firmware

**There is no other way back to the DIIVOO app.** Open the gateway, connect
a **3.3 V** USB-serial adapter to its UART pads, hold the gateway in download
mode, and read the whole flash:

```bash
esptool.py --port COM5 read_flash 0 0x400000 wg03-stock.bin
```

Keep that file. Writing it back with `esptool.py write_flash 0 wg03-stock.bin`
restores the original gateway. Your valves would then have to be paired to it
again in the app.

### 2. Configure

Add the component to your ESPHome config:

```yaml
external_components:
  - source: github://arctus/esphome-diivoo@v1.1.0
    components: [diivoo]

time:
  - platform: homeassistant
    id: ha_time

diivoo:
  sclk_pin: GPIO27
  sdio_pin: GPIO19
  csb_pin: GPIO25
  fcsb_pin: GPIO26
  power_pin:
    number: GPIO21
    inverted: true
  irq_pin:
    number: GPIO18
    mode:
      input: true
      pulldown: true
  hub_id: 0x12345678 # any non-zero number, keep it stable
  time_id: ha_time
  max_valves: 4
```

That is all: every valve, zone and diagnostic entity comes from the
component. [`example/diivoo-gateway.yaml`](example/diivoo-gateway.yaml) is a
complete config, with the button, the blinking LED and the power settings the
WG03 needs.

### 3. Flash

The first flash goes over USB (`esphome run your-config.yaml`). Later updates
go over Wi-Fi, **but see [Power](#power-wg03-quirks) below**: on the
gateway's own mains supply they fail.

### 4. Pair your valves

1. Turn on **Pairing Mode**, or press the gateway button.
2. Power-cycle the valve: take a battery out and put it back in.
3. The log shows `Slot 'valve_1': paired valve 12345678`. Pairing mode
   switches itself off after one valve, or after `pairing_timeout`.
4. **The gateway restarts about 20 s later**, and Home Assistant then shows
   *Valve 1 Zone 1*, *Valve 1 Zone 2*, ... for as many zones as the model has.
5. Press **Valve 1 Refresh** once. The valve re-reads its settings and is told
   it has no schedules, which clears any plans left over from the app.

Pairing fills the first empty slot. **Valve N Unpair** frees it again (and
restarts the gateway to hide its entities). A valve with an unknown hardware
id gets all four zones.

## Configuration reference

### `diivoo:`

| Option | Required | Description |
|---|---|---|
| `sclk_pin`, `sdio_pin`, `csb_pin`, `fcsb_pin` | yes | radio SPI pins |
| `power_pin` | no | radio supply enable (WG03: GPIO21, inverted) |
| `irq_pin` | yes | radio packet interrupt |
| `hub_id` | no | 32-bit id the valves pair to. Defaults to a value derived from the MAC address. Set it so a new board keeps the pairing. |
| `time_id` | yes | a `time` component. Valves are sent the current time. |
| `pairing_timeout` | no | default `5min` |
| `max_valves` | no | number of valve slots, 1-8, default `4` |
| `pairing_light` | no | a light to drive while pairing mode is on |
| `pairing_effect` | no | effect of that light to run while pairing (else it is just on) |

### Entities

Created automatically, per slot `N` (1 ... `max_valves`):

| Entity | Scope | What it is |
|---|---|---|
| valve *Valve N Zone Z* | zone | open or close the zone |
| number *Valve N Zone Z Duration* | zone | run time in minutes (1-1092) |
| sensor *Valve N Zone Z Remaining Time* | zone | seconds left in the current run |
| sensor *Valve N Zone Z Last Water Volume* | zone | litres used by the last run |
| sensor *Valve N Battery Level* | valve | 100 % (OK) or 20 % (low) |
| sensor *Valve N Signal Strength* | valve | dBm |
| binary sensor *Valve N Online* | valve | heard from in the last 12 h |
| binary sensor *Valve N Battery Low* | valve | low battery |
| text sensor *Valve N ID* / *Valve N Model* | valve | radio id, WT-07W ... WT-13W |
| button *Valve N Refresh* / *Valve N Unpair* | valve | re-sync, or forget the valve |
| switch *Pairing Mode* | gateway | pairing on/off |

The gateway button can call `id(<diivoo id>).toggle_pairing_mode()` from a
lambda. Hiding uses `set_internal()` at boot, which ESPHome deprecates for
removal in 2027.3; the component skips it from that version on, so empty
slots would then show as unavailable instead.

## Power (WG03 quirks)

- **The 3.3 V rail is weak.** At full Wi-Fi TX power the board brown-out-loops
  at Wi-Fi start. Run it at `cpu_frequency: 160MHZ`, Wi-Fi `output_power:
  8.5dB` and with `CONFIG_ESP_BROWNOUT_DET: n`, as the example does.
- **OTA fails on the gateway's own mains supply.** The upload completes, then
  the device logs `esp_image: image at 0x1d0000 has invalid magic byte` and
  `esp_ota_end failed (err=0x1503)`: the flash erase silently never happens
  (it is not write protection; the block-protect bits are clear). Powered
  from a USB-serial adapter's 3.3 V instead, the same OTA succeeds. Update it
  on the adapter, **with mains disconnected - never both at once**.

## How it works

Frames are 32 bytes, the radio checks the CRC, and nothing is encrypted:

```
0x51 | target id (LE32) | sender id (LE32) | seq | cmd | len | payload | zero padding
```

- **Channels.** The gateway listens on channel 0. It sends to a valve on that
  valve's assigned channel, and pairing runs on channel 4.
- **Waking a valve.** Valves sleep and listen for less than half a millisecond
  every ~100 ms, so anything that has to wake one is sent with a long
  preamble (300 units). Replies use the short one (40).

| cmd | direction | meaning | answer |
|---|---|---|---|
| `0x01` | valve | join request | `0x81`: address, channel, time |
| `0x02` | valve | status: zone, state, remaining time, battery | `0x82` ack, with the time when asked |
| `0x04` | valve | event (e.g. stopped), water used | `0x84` |
| `0x05` | valve | parameter request | `0x85`: default duration, no rain delay |
| `0x06` | valve | schedule request | `0x86`: no schedules |
| `0xA1` | valve | action acknowledged | `0x82` |
| `0x20` | gateway | wake / refresh | |
| `0x21` | gateway | open `[zone, 02, 01, seconds]` / close `[zone, 02, 00]` | |

An open or close is sent twice with the long preamble, each followed by a
350 ms listen. If neither is confirmed, up to 12 short-preamble sends follow
before the command is reported as failed.

Set `logger: level: VERBOSE` to see every raw frame in both directions.

## Credits

The radio protocol was reverse-engineered by **Stefan
([@Technerd-SG](https://github.com/Technerd-SG))**. He spent more than a year
on it with a logic analyzer and SPI captures, and published the result as
[**hassio-diivoo2mqtt**](https://github.com/Technerd-SG/hassio-diivoo2mqtt): a
Home Assistant add-on plus replacement gateway firmware. If this component
saves you from the cloud, please consider
[supporting his work](https://ko-fi.com/stefantechnerdsg).

This component is a port of that work to ESPHome: the gateway ESP32 runs the
protocol itself, instead of acting as a TCP radio bridge for the add-on.
Taken from hassio-diivoo2mqtt (add-on 0.1.60, gateway firmware 0.1.11):

- the CMT2300A register profile, the SPI timing and the transmit sequence
  (`cmt2300a.cpp`), copied unchanged;
- the frame format, the command set, the pairing and channel allocation, and
  the action retry strategy (`diivoo.cpp`).

Thanks also to the contributors of that project, whose issues and pull
requests filled in much of the protocol.

This project is not affiliated with DIIVOO, HomGar or RainPoint. Flashing
replaces the vendor firmware: do it at your own risk.

## License

[GNU Affero General Public License v3.0](LICENSE).
