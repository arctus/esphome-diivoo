"""DIIVOO irrigation timers driven directly from the WG03 gateway's ESP32.

Radio protocol ported from Technerd-SG/hassio-diivoo2mqtt (AGPL-3.0). See
../../README.md for the protocol notes and what was left out.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID
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

diivoo_ns = cg.esphome_ns.namespace("diivoo")
DiivooHub = diivoo_ns.class_("DiivooHub", cg.Component)
DiivooValve = diivoo_ns.class_("DiivooValve")

CONF_DIIVOO_ID = "diivoo_id"
CONF_DIIVOO_VALVE = "diivoo_valve"
CONF_ZONE = "zone"

CONF_SCLK_PIN = "sclk_pin"
CONF_SDIO_PIN = "sdio_pin"
CONF_CSB_PIN = "csb_pin"
CONF_FCSB_PIN = "fcsb_pin"
CONF_POWER_PIN = "power_pin"
CONF_IRQ_PIN = "irq_pin"
CONF_HUB_ID = "hub_id"
CONF_PAIRING_TIMEOUT = "pairing_timeout"
CONF_VALVES = "valves"
CONF_ZONES = "zones"

VALVE_SLOT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(DiivooValve),
        cv.Optional(CONF_ZONES, default=1): cv.int_range(min=1, max=4),
    }
)

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
        cv.Required(CONF_VALVES): cv.All(
            cv.ensure_list(VALVE_SLOT_SCHEMA), cv.Length(min=1, max=8)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

# Shared by the entity platforms.
VALVE_REF_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_DIIVOO_VALVE): cv.use_id(DiivooValve),
    }
)
ZONE_REF_SCHEMA = VALVE_REF_SCHEMA.extend(
    {
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=4),
    }
)


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

    for slot in config[CONF_VALVES]:
        name = str(slot[CONF_ID].id)
        pref_hash = fnv1a_32bit_hash(f"diivoo_valve_{name}")
        valve = cg.new_Pvariable(
            slot[CONF_ID],
            slot[CONF_ZONES],
            cg.RawExpression(f"{pref_hash}UL"),
            name,
        )
        cg.add(var.add_valve(valve))
