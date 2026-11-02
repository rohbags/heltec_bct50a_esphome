import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor, uart, light
from esphome.const import CONF_ID
from esphome.components import time
from esphome.components.esp32 import include_builtin_idf_component

from pathlib import Path

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "light"]

CONF_HOME_TIME_ID = "home_time_id"
CONF_SNTP_TIME_ID = "sntp_time_id"
CONF_STATUS_LIGHT_ID = "status_light_id"
CONF_CPU_TEMP_ID = "cpu_temp_id"

bct50a_ns = cg.esphome_ns.namespace("bct50a")
BCT50AComponent = bct50a_ns.class_("BCT50AComponent", cg.Component, uart.UARTDevice)

SENSOR_KEYS = {
    "voltage": sensor.sensor_schema(unit_of_measurement="V", accuracy_decimals=3),
    "current": sensor.sensor_schema(unit_of_measurement="A", accuracy_decimals=2),
    "capacity_ah": sensor.sensor_schema(unit_of_measurement="Ah", accuracy_decimals=2),
    "energy_wh": sensor.sensor_schema(unit_of_measurement="Wh", accuracy_decimals=2),
    "set_current": sensor.sensor_schema(unit_of_measurement="A", accuracy_decimals=2),
    "set_voltage": sensor.sensor_schema(unit_of_measurement="V", accuracy_decimals=2),
    "end_value": sensor.sensor_schema(accuracy_decimals=2),
    "runtime": sensor.sensor_schema(unit_of_measurement="s", accuracy_decimals=0),
}

TEXT_KEYS = ["device_state", "firmware_version", "product"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BCT50AComponent),
        **{cv.Required(key): schema for key, schema in SENSOR_KEYS.items()},
        **{cv.Required(key): text_sensor.text_sensor_schema() for key in TEXT_KEYS},
        cv.Optional(CONF_HOME_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Optional(CONF_SNTP_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Required(CONF_STATUS_LIGHT_ID): cv.use_id(light.LightState),
        cv.Optional(CONF_CPU_TEMP_ID): cv.use_id(sensor.Sensor),
    }
).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)

async def to_code(config):
    generate_dashboard_header()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    status_light = await cg.get_variable(config[CONF_STATUS_LIGHT_ID])
    cg.add(var.set_status_light(status_light))

    if CONF_CPU_TEMP_ID in config:
        cpu_temp = await cg.get_variable(config[CONF_CPU_TEMP_ID])
        cg.add(var.set_cpu_temp_sensor(cpu_temp))

    for key in SENSOR_KEYS:
        sens = await sensor.new_sensor(config[key])
        cg.add(getattr(var, f"set_{key}_sensor")(sens))

    for key in TEXT_KEYS:
        ts = await text_sensor.new_text_sensor(config[key])
        cg.add(getattr(var, f"set_{key}_text_sensor")(ts))
    
    if CONF_HOME_TIME_ID in config:
        home_time = await cg.get_variable(config[CONF_HOME_TIME_ID])
        cg.add(var.set_home_time(home_time))

    if CONF_SNTP_TIME_ID in config:
        sntp_time = await cg.get_variable(config[CONF_SNTP_TIME_ID])
        cg.add(var.set_sntp_time(sntp_time))


def generate_dashboard_header():
    component_dir = Path(__file__).resolve().parent
    html_path = component_dir / "bct50a.html"
    js_path = component_dir / "bct50a.js"
    header_path = component_dir / "bct50a_html.h"
    html = html_path.read_text(encoding="utf-8")
    js = js_path.read_text(encoding="utf-8")
    placeholder = "/* BCT50A_JS */"
    if html.count(placeholder) != 1:
        raise RuntimeError(
            f"Expected exactly one {placeholder} placeholder in bct50a.html"
        )
    html = html.replace(placeholder, js)
    delimiter = "BCTHTML"
    if f"){delimiter}\"" in html:
        raise RuntimeError(
            f"Dashboard HTML contains the raw-string terminator "
            f"){delimiter}\""
        )
    header = f'''#pragma once

// Generated from bct50a.html and bct50a.js by ESPHome configuration.
static const char DASHBOARD_HTML[] = R"{delimiter}(
{html}
){delimiter}";
'''

    # Avoid touching the header when the content has not changed.
    if not header_path.exists() or header_path.read_text(
        encoding="utf-8"
    ) != header:
        header_path.write_text(header, encoding="utf-8")


