import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_DIIVOO_ID, DiivooHub, diivoo_ns

DiivooPairingSwitch = diivoo_ns.class_("DiivooPairingSwitch", switch.Switch)

CONFIG_SCHEMA = switch.switch_schema(
    DiivooPairingSwitch,
    icon="mdi:link-variant",
    entity_category=ENTITY_CATEGORY_CONFIG,
    default_restore_mode="ALWAYS_OFF",
).extend(
    {
        cv.GenerateID(CONF_DIIVOO_ID): cv.use_id(DiivooHub),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_parented(var, config[CONF_DIIVOO_ID])
    hub = await cg.get_variable(config[CONF_DIIVOO_ID])
    cg.add(hub.set_pairing_switch(var))
