import esphome.codegen as cg
from esphome.components import valve
import esphome.config_validation as cv

from .. import CONF_DIIVOO_VALVE, CONF_ZONE, ZONE_REF_SCHEMA, diivoo_ns

DiivooZoneValve = diivoo_ns.class_("DiivooZoneValve", valve.Valve, cg.Component)

CONFIG_SCHEMA = (
    valve.valve_schema(DiivooZoneValve, device_class="water", icon="mdi:sprinkler")
    .extend(ZONE_REF_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await valve.new_valve(config)
    await cg.register_component(var, config)
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    cg.add(var.set_target(slot, config[CONF_ZONE]))
    cg.add(slot.set_zone_valve(config[CONF_ZONE], var))
