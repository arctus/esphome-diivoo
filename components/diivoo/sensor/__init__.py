import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_VOLUME,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_SECOND,
)

from .. import CONF_DIIVOO_VALVE, CONF_ZONE, VALVE_REF_SCHEMA, ZONE_REF_SCHEMA

TYPE_REMAINING_TIME = "remaining_time"
TYPE_LAST_WATER_VOLUME = "last_water_volume"
TYPE_RSSI = "rssi"
TYPE_BATTERY_LEVEL = "battery_level"

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_REMAINING_TIME: sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:timer-sand",
            accuracy_decimals=0,
        ).extend(ZONE_REF_SCHEMA),
        # Water used by the last run, from the stop event. Unverified on
        # models without a flow meter: those may always report 0.
        TYPE_LAST_WATER_VOLUME: sensor.sensor_schema(
            unit_of_measurement="L",
            device_class=DEVICE_CLASS_VOLUME,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:water",
            accuracy_decimals=1,
        ).extend(ZONE_REF_SCHEMA),
        TYPE_RSSI: sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ).extend(VALVE_REF_SCHEMA),
        # The valve only reports OK or low: published as 100 % or 20 %.
        TYPE_BATTERY_LEVEL: sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ).extend(VALVE_REF_SCHEMA),
    },
    lower=True,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    kind = config[CONF_TYPE]
    if kind == TYPE_REMAINING_TIME:
        cg.add(slot.set_zone_remaining_sensor(config[CONF_ZONE], var))
    elif kind == TYPE_LAST_WATER_VOLUME:
        cg.add(slot.set_zone_water_sensor(config[CONF_ZONE], var))
    elif kind == TYPE_RSSI:
        cg.add(slot.set_rssi_sensor(var))
    elif kind == TYPE_BATTERY_LEVEL:
        cg.add(slot.set_battery_sensor(var))
