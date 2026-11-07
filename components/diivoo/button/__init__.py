import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_CONFIG, ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_DIIVOO_VALVE, VALVE_REF_SCHEMA, diivoo_ns

DiivooValveButton = diivoo_ns.class_("DiivooValveButton", button.Button)
ValveButtonAction = diivoo_ns.enum("ValveButtonAction", is_class=True)

TYPE_REFRESH = "refresh"
TYPE_UNPAIR = "unpair"

ACTIONS = {
    TYPE_REFRESH: ValveButtonAction.REFRESH,
    TYPE_UNPAIR: ValveButtonAction.UNPAIR,
}

CONFIG_SCHEMA = cv.typed_schema(
    {
        # Wake the valve: it answers with its state and re-reads its
        # parameters and its (empty) schedule.
        TYPE_REFRESH: button.button_schema(
            DiivooValveButton,
            icon="mdi:refresh",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(VALVE_REF_SCHEMA),
        TYPE_UNPAIR: button.button_schema(
            DiivooValveButton,
            icon="mdi:link-variant-off",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(VALVE_REF_SCHEMA),
    },
    lower=True,
)


async def to_code(config):
    var = await button.new_button(config)
    slot = await cg.get_variable(config[CONF_DIIVOO_VALVE])
    cg.add(var.set_target(slot, ACTIONS[config[CONF_TYPE]]))
