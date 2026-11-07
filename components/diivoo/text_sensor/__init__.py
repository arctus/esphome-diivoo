import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_DIIVOO_VALVE, VALVE_REF_SCHEMA

TYPE_VALVE_ID = "valve_id"
TYPE_MODEL = "model"

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_VALVE_ID: text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:identifier",
        ).extend(VALVE_REF_SCHEMA),
        TYPE_MODEL: text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:information-outline",
        ).extend(VALVE_REF_SCHEMA),
    },
    lower=True,
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    if config[CONF_TYPE] == TYPE_VALVE_ID:
        cg.add(slot.set_valve_id_sensor(var))
    else:
        cg.add(slot.set_model_sensor(var))
