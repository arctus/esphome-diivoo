"""DIIVOO irrigation timers driven directly from the WG03 gateway's ESP32.

Radio protocol ported from Technerd-SG/hassio-diivoo2mqtt (AGPL-3.0).

The component creates every entity itself: `max_valves` slots, each with a
full set of entities for four zones. Pairing detects the valve model, and at
boot each slot shows Home Assistant only the zones that model has (an empty
slot shows nothing). The gateway restarts by itself after a pairing or an
unpairing so the new entity set is published.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import (
    binary_sensor,
    button,
    number,
    sensor,
    switch,
    text_sensor,
    valve,
)
from esphome.components import time as time_
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_TIME_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_VOLUME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_SECOND,
)
from esphome.core import CORE
from esphome.helpers import fnv1a_32bit_hash

DEPENDENCIES = ["time"]
AUTO_LOAD = [
    "binary_sensor",
    "button",
    "number",
    "sensor",
    "switch",
    "text_sensor",
    "valve",
]

MAX_ZONES = 4

diivoo_ns = cg.esphome_ns.namespace("diivoo")
DiivooHub = diivoo_ns.class_("DiivooHub", cg.Component)
DiivooValve = diivoo_ns.class_("DiivooValve")
DiivooZoneValve = diivoo_ns.class_("DiivooZoneValve", valve.Valve, cg.Component)
DiivooDurationNumber = diivoo_ns.class_(
    "DiivooDurationNumber", number.Number, cg.Component
)
DiivooPairingSwitch = diivoo_ns.class_("DiivooPairingSwitch", switch.Switch)
DiivooValveButton = diivoo_ns.class_("DiivooValveButton", button.Button)
ValveButtonAction = diivoo_ns.enum("ValveButtonAction", is_class=True)

CONF_SCLK_PIN = "sclk_pin"
CONF_SDIO_PIN = "sdio_pin"
CONF_CSB_PIN = "csb_pin"
CONF_FCSB_PIN = "fcsb_pin"
CONF_POWER_PIN = "power_pin"
CONF_IRQ_PIN = "irq_pin"
CONF_HUB_ID = "hub_id"
CONF_PAIRING_TIMEOUT = "pairing_timeout"
CONF_MAX_VALVES = "max_valves"
CONF_PAIRING_LIGHT = "pairing_light"
CONF_PAIRING_EFFECT = "pairing_effect"


def _light_id(value):
    # Imported lazily so configs without a light do not load the component.
    from esphome.components import light

    return cv.use_id(light.LightState)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DiivooHub),
        cv.Required(CONF_SCLK_PIN): pins.gpio_output_pin_schema,
        # Shared data line: the driver switches its direction at run time.
        cv.Required(CONF_SDIO_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_CSB_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_FCSB_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_POWER_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_IRQ_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_HUB_ID): cv.All(cv.uint32_t, cv.Range(min=1)),
        cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(
            CONF_PAIRING_TIMEOUT, default="5min"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_VALVES, default=4): cv.int_range(min=1, max=8),
        # A light that shows pairing mode (on with this effect while pairing).
        cv.Optional(CONF_PAIRING_LIGHT): _light_id,
        cv.Optional(CONF_PAIRING_EFFECT): cv.string_strict,
    }
).extend(cv.COMPONENT_SCHEMA)


def _entity_config(schema, entity_id, name):
    """Run a platform's own schema over a generated entity, filling defaults."""
    return schema({CONF_ID: entity_id, CONF_NAME: name})


async def _register_component(var, conf):
    # Generated here, after config validation, so ESPHome has not listed the
    # id among the declared components that register_component expects.
    CORE.component_ids.add(str(conf[CONF_ID].id))
    await cg.register_component(var, conf)


async def _new_valve_slot(hub, base, index):
    slot_name = f"valve_{index}"
    pref_hash = fnv1a_32bit_hash(f"diivoo_valve_{slot_name}")
    slot = cg.new_Pvariable(
        cv.declare_id(DiivooValve)(f"{base}_{slot_name}"),
        cg.RawExpression(f"{pref_hash}UL"),
        slot_name,
    )
    cg.add(hub.add_valve(slot))
    label = f"Valve {index}"
    prefix = f"{base}_v{index}"

    for zone in range(1, MAX_ZONES + 1):
        zone_label = f"{label} Zone {zone}"
        zprefix = f"{prefix}_z{zone}"

        conf = _entity_config(
            valve.valve_schema(
                DiivooZoneValve, device_class="water", icon="mdi:sprinkler"
            ).extend(cv.COMPONENT_SCHEMA),
            f"{zprefix}_valve",
            zone_label,
        )
        var = await valve.new_valve(conf)
        await _register_component(var, conf)
        cg.add(var.set_target(slot, zone))
        cg.add(slot.set_zone_valve(zone, var))

        # Run time in minutes; the protocol carries 16-bit seconds.
        conf = _entity_config(
            number.number_schema(
                DiivooDurationNumber,
                unit_of_measurement=UNIT_MINUTE,
                device_class=DEVICE_CLASS_DURATION,
                icon="mdi:timer-outline",
            ).extend(cv.COMPONENT_SCHEMA),
            f"{zprefix}_duration",
            f"{zone_label} Duration",
        )
        var = await number.new_number(conf, min_value=1, max_value=1092, step=1)
        await _register_component(var, conf)
        cg.add(var.set_target(slot, zone))
        cg.add(slot.set_zone_duration(zone, var))

        conf = _entity_config(
            sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:timer-sand",
                accuracy_decimals=0,
            ),
            f"{zprefix}_remaining",
            f"{zone_label} Remaining Time",
        )
        var = await sensor.new_sensor(conf)
        cg.add(slot.set_zone_remaining_sensor(zone, var))

        conf = _entity_config(
            sensor.sensor_schema(
                unit_of_measurement="L",
                device_class=DEVICE_CLASS_VOLUME,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:water",
                accuracy_decimals=1,
            ),
            f"{zprefix}_water",
            f"{zone_label} Last Water Volume",
        )
        var = await sensor.new_sensor(conf)
        cg.add(slot.set_zone_water_sensor(zone, var))

    conf = _entity_config(
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ),
        f"{prefix}_battery",
        f"{label} Battery Level",
    )
    cg.add(slot.set_battery_sensor(await sensor.new_sensor(conf)))

    conf = _entity_config(
        sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ),
        f"{prefix}_rssi",
        f"{label} Signal Strength",
    )
    cg.add(slot.set_rssi_sensor(await sensor.new_sensor(conf)))

    conf = _entity_config(
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        f"{prefix}_online",
        f"{label} Online",
    )
    cg.add(slot.set_online_sensor(await binary_sensor.new_binary_sensor(conf)))

    conf = _entity_config(
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        f"{prefix}_battery_low",
        f"{label} Battery Low",
    )
    cg.add(slot.set_battery_low_sensor(await binary_sensor.new_binary_sensor(conf)))

    conf = _entity_config(
        text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:identifier"
        ),
        f"{prefix}_id",
        f"{label} ID",
    )
    cg.add(slot.set_valve_id_sensor(await text_sensor.new_text_sensor(conf)))

    conf = _entity_config(
        text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:information-outline",
        ),
        f"{prefix}_model",
        f"{label} Model",
    )
    cg.add(slot.set_model_sensor(await text_sensor.new_text_sensor(conf)))

    for action, icon, category, suffix in (
        ("REFRESH", "mdi:refresh", ENTITY_CATEGORY_DIAGNOSTIC, "Refresh"),
        ("UNPAIR", "mdi:link-variant-off", ENTITY_CATEGORY_CONFIG, "Unpair"),
    ):
        conf = _entity_config(
            button.button_schema(
                DiivooValveButton, icon=icon, entity_category=category
            ),
            f"{prefix}_{suffix.lower()}",
            f"{label} {suffix}",
        )
        var = await button.new_button(conf)
        cg.add(var.set_target(slot, getattr(ValveButtonAction, action)))
        if action == "REFRESH":
            cg.add(slot.set_refresh_button(var))
        else:
            cg.add(slot.set_unpair_button(var))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for key, setter in (
        (CONF_SCLK_PIN, var.set_sclk_pin),
        (CONF_SDIO_PIN, var.set_sdio_pin),
        (CONF_CSB_PIN, var.set_csb_pin),
        (CONF_FCSB_PIN, var.set_fcsb_pin),
        (CONF_IRQ_PIN, var.set_irq_pin),
    ):
        pin = await cg.gpio_pin_expression(config[key])
        cg.add(setter(pin))
    if CONF_POWER_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_POWER_PIN])
        cg.add(var.set_power_pin(pin))

    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))
    if CONF_HUB_ID in config:
        cg.add(var.set_hub_id(config[CONF_HUB_ID]))
    cg.add(var.set_pairing_timeout(config[CONF_PAIRING_TIMEOUT].total_milliseconds))

    base = str(config[CONF_ID].id)

    conf = _entity_config(
        switch.switch_schema(
            DiivooPairingSwitch,
            icon="mdi:link-variant",
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="ALWAYS_OFF",
        ),
        f"{base}_pairing_mode",
        "Pairing Mode",
    )
    pairing = await switch.new_switch(conf)
    cg.add(pairing.set_parent(var))
    cg.add(var.set_pairing_switch(pairing))

    if CONF_PAIRING_LIGHT in config:
        light = await cg.get_variable(config[CONF_PAIRING_LIGHT])
        effect = config.get(CONF_PAIRING_EFFECT)
        cg.add(
            var.set_pairing_light(
                light, effect if effect is not None else cg.RawExpression("nullptr")
            )
        )

    for index in range(1, config[CONF_MAX_VALVES] + 1):
        await _new_valve_slot(var, base, index)
