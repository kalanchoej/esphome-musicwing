import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome import pins
from esphome.components import spi
from esphome.const import CONF_ID, CONF_RESET_PIN

CONF_SPI_FAST_ID = "spi_fast_id"
CONF_SPI_SLOW_ID = "spi_slow_id"
CONF_SPI_WRAPPER_ID = "spi_wrapper_id"
CONF_DREQ_PIN = "dreq_pin"
CONF_XCS_PIN = "xcs_pin"
CONF_XDCS_PIN = "xdcs_pin"
CONF_PLUGINS = "plugins"

CODEOWNERS = ["@mmakaay"]
DEPENDENCIES = ["spi"]

vs10xx_base_ns = cg.esphome_ns.namespace("vs10xx_base")
VS10XXBase = vs10xx_base_ns.class_("VS10XXBase", cg.Component)
VS10XXPlugin = vs10xx_base_ns.class_("VS10XXPlugin")
VS10XXSlowSPI = vs10xx_base_ns.class_("VS10XXSlowSPI", cg.Component, spi.SPIDevice)
VS10XXFastSPI = vs10xx_base_ns.class_("VS10XXFastSPI", cg.Component, spi.SPIDevice)
VS10XXSPI = vs10xx_base_ns.class_("VS10XXSPI", cg.Component)


def vs10xx_device_schema(component_class, plugins = {}):
    schema = {
        cv.GenerateID(): cv.declare_id(component_class),
        cv.GenerateID(CONF_SPI_SLOW_ID): cv.declare_id(VS10XXSlowSPI),
        cv.GenerateID(CONF_SPI_FAST_ID): cv.declare_id(VS10XXFastSPI),
        cv.GenerateID(CONF_SPI_WRAPPER_ID): cv.declare_id(VS10XXSPI),
        cv.Required(CONF_DREQ_PIN): pins.gpio_input_pin_schema,
        cv.Required(CONF_XDCS_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_XCS_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_PLUGINS): cv.ensure_list(cv.enum(plugins)),
    }
    return (
        cv.Schema(schema)  
        .extend(cv.COMPONENT_SCHEMA)
        .extend(spi.spi_device_schema(False))
    )


async def register_vs10xx_component(config, plugins):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spi_wrapper = cg.new_Pvariable(config[CONF_SPI_WRAPPER_ID])
    await cg.register_component(spi_wrapper, config)
    cg.add(var.set_spi(spi_wrapper))

    spi_slow = cg.new_Pvariable(config[CONF_SPI_SLOW_ID])
    await spi.register_spi_device(spi_slow, config)
    cg.add(spi_wrapper.set_slow_spi(spi_slow))

    spi_fast = cg.new_Pvariable(config[CONF_SPI_FAST_ID])
    await spi.register_spi_device(spi_fast, config)
    cg.add(spi_wrapper.set_fast_spi(spi_fast))

    dreq_pin = await cg.gpio_pin_expression(config[CONF_DREQ_PIN])
    cg.add(var.set_dreq_pin(dreq_pin))

    xcs_pin = await cg.gpio_pin_expression(config[CONF_XCS_PIN])
    cg.add(spi_wrapper.set_xcs_pin(xcs_pin))

    xdcs_pin = await cg.gpio_pin_expression(config[CONF_XDCS_PIN])
    cg.add(spi_wrapper.set_xdcs_pin(xdcs_pin))

    if CONF_RESET_PIN in config:
        reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
        cg.add(var.set_reset_pin(reset_pin))

    if CONF_PLUGINS in config:
        for name in config[CONF_PLUGINS]:
            plugin_class = plugins[name]
            plugin_id = cv.declare_id(plugin_class)(f"plugin_{name}")
            plugin = cg.new_Pvariable(plugin_id, spi_wrapper)
            cg.add(var.add_plugin(plugin))
