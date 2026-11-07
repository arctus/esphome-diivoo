import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from .. import CONF_DIIVOO_VALVE, VALVE_REF_SCHEMA

TYPE_ONLINE = "online"
TYPE_BATTERY_LOW = "battery_low"

CONFIG_SCHEMA = cv.typed_schema(
    {
        # Heard from within the last 12 hours.
        TYPE_ONLINE: binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(VALVE_REF_SCHEMA),
        TYPE_BATTERY_LOW: binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(VALVE_REF_SCHEMA),
    },
    lower=True,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    if config[CONF_TYPE] == TYPE_ONLINE:
        cg.add(slot.set_online_sensor(var))
    else:
        cg.add(slot.set_battery_low_sensor(var))
