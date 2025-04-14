import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

ftp_http_proxy_ns = cg.esphome_ns.namespace('ftp_http_proxy')
FTPHTTPProxy = ftp_http_proxy_ns.class_('FTPHTTPProxy', cg.Component)

CONF_FTP_SERVER = 'ftp_server'
CONF_USERNAME = 'username'
CONF_PASSWORD = 'password'
CONF_LOCAL_PORT = 'local_port'

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(FTPHTTPProxy),
    cv.Required(CONF_FTP_SERVER): cv.string,
    cv.Required(CONF_USERNAME): cv.string,
    cv.Required(CONF_PASSWORD): cv.string,
    cv.Optional(CONF_LOCAL_PORT, default=8080): cv.port,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_ftp_server(config[CONF_FTP_SERVER]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_local_port(config[CONF_LOCAL_PORT]))

