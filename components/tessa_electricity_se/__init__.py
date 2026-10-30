"""Tessa electricity SE: the ESPHome side. The tiles, the card and the top bar's item are in tessa_electricity_se.cpp; the
item shows the price of a tile on the screen. smart_display.register_plugin() reads the id, version, tile memory and
the screen's texts from the manifest and translations/ beside this folder (docs/FIRMWARE_API.md)."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

tessa_electricity_se_ns = cg.esphome_ns.namespace("tessa_electricity_se")
ElectricityPrice = tessa_electricity_se_ns.class_("ElectricityPrice", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ElectricityPrice),
        cv.Optional("nordpool_config_entry", default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_nordpool_config_entry(config["nordpool_config_entry"]))
    await smart_display.register_plugin(var, __file__)

