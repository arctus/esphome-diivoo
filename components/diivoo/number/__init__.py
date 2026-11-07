import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_INITIAL_VALUE,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STEP,
    DEVICE_CLASS_DURATION,
    UNIT_MINUTE,
)

from .. import CONF_DIIVOO_VALVE, CONF_ZONE, ZONE_REF_SCHEMA, diivoo_ns

DiivooDurationNumber = diivoo_ns.class_(
    "DiivooDurationNumber", number.Number, cg.Component
)

# The protocol carries the run time as 16-bit seconds: at most 1092 minutes.
CONFIG_SCHEMA = (
    number.number_schema(
        DiivooDurationNumber,
        unit_of_measurement=UNIT_MINUTE,
        device_class=DEVICE_CLASS_DURATION,
        icon="mdi:timer-outline",
    )
    .extend(ZONE_REF_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_MIN_VALUE, default=1): cv.float_range(min=1, max=1092),
            cv.Optional(CONF_MAX_VALUE, default=1092): cv.float_range(min=1, max=1092),
            cv.Optional(CONF_STEP, default=1): cv.positive_float,
            cv.Optional(CONF_INITIAL_VALUE, default=10): cv.float_range(
                min=1, max=1092
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)
    cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    cg.add(var.set_target(slot, config[CONF_ZONE]))
    cg.add(slot.set_zone_duration(config[CONF_ZONE], var))
