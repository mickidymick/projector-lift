# ESPHome external component: projector_lift
# Owns the OPEN / CLOSE / EMERGENCY-STOP state machine that used to live in
# projector-lift.yaml as scripts + globals. YAML still owns pins, entities,
# tunables, and network scripts (AVR / PJLink) — this component just consumes
# refs to them and drives the state machine.
#
# Hookup pattern in YAML:
#   projector_lift:
#     id: lift
#     bts_a_enable: bts_a_enable    # switch id
#     ...                            # every pin/entity ref
#     on_opening:                    # fires before OPENING motion starts
#       - script.execute: projector_power_on
#       - script.execute: avr_outdoor
#     on_closing:                    # fires before CLOSING motion starts
#       - script.execute: projector_power_off
#       - script.execute: avr_indoor
#     on_boot_closed:  { ... }
#     on_boot_open:    { ... }
#     on_boot_reconciled: { ... }
#
# C++ methods callable from YAML lambdas:
#   id(lift).request_open()
#   id(lift).request_close()
#   id(lift).emergency_stop()
#   id(lift).on_button_press()
#   id(lift).force_closed()
#   id(lift).reconcile_from_endstops()
#   id(lift).is_projector_active()        // bool: OPENING or OPEN

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import binary_sensor, light, number, sensor, switch
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@mickidymick"]

projector_lift_ns = cg.esphome_ns.namespace("projector_lift")
ProjectorLift = projector_lift_ns.class_("ProjectorLift", cg.Component)

OpeningTrigger = projector_lift_ns.class_("OpeningTrigger", automation.Trigger.template())
ClosingTrigger = projector_lift_ns.class_("ClosingTrigger", automation.Trigger.template())
BootClosedTrigger = projector_lift_ns.class_("BootClosedTrigger", automation.Trigger.template())
BootOpenTrigger = projector_lift_ns.class_("BootOpenTrigger", automation.Trigger.template())
BootReconciledTrigger = projector_lift_ns.class_("BootReconciledTrigger", automation.Trigger.template())

# Refs into other YAML components.
CONF_BTS_A_ENABLE       = "bts_a_enable"
CONF_BTS_B_ENABLE       = "bts_b_enable"
CONF_SCREEN_RELAY       = "screen_relay"
CONF_RPWM               = "rpwm"
CONF_LPWM               = "lpwm"
CONF_FAN_PWM            = "fan_pwm"
CONF_A_RIS              = "a_ris"
CONF_A_LIS              = "a_lis"
CONF_B_RIS              = "b_ris"
CONF_B_LIS              = "b_lis"
CONF_ENDSTOP_A          = "endstop_a"
CONF_ENDSTOP_B          = "endstop_b"
CONF_CRUISE_PCT         = "cruise_pct"
CONF_RAMP_MS            = "ramp_ms"
CONF_EOT_V              = "eot_v"
CONF_STALL_V            = "stall_v"
CONF_TRAVEL_TIMEOUT_S   = "travel_timeout_s"
CONF_FAN_RUNNING_PCT    = "fan_running_pct"
CONF_FAN_COOLDOWN_PCT   = "fan_cooldown_pct"
CONF_SCREEN_DELAY_S     = "screen_delay_s"
CONF_COOLDOWN_MIN       = "cooldown_min"
# Trigger hooks.
CONF_ON_OPENING         = "on_opening"
CONF_ON_CLOSING         = "on_closing"
CONF_ON_BOOT_CLOSED     = "on_boot_closed"
CONF_ON_BOOT_OPEN       = "on_boot_open"
CONF_ON_BOOT_RECONCILED = "on_boot_reconciled"

# (config_key, setter_method_name, esphome_type)
_ID_REFS = [
    (CONF_BTS_A_ENABLE,     "set_bts_a",             switch.Switch),
    (CONF_BTS_B_ENABLE,     "set_bts_b",             switch.Switch),
    (CONF_SCREEN_RELAY,     "set_screen_relay",      switch.Switch),
    (CONF_RPWM,             "set_rpwm",              light.LightState),
    (CONF_LPWM,             "set_lpwm",              light.LightState),
    (CONF_FAN_PWM,          "set_fan_pwm",           light.LightState),
    (CONF_A_RIS,            "set_a_ris",             sensor.Sensor),
    (CONF_A_LIS,            "set_a_lis",             sensor.Sensor),
    (CONF_B_RIS,            "set_b_ris",             sensor.Sensor),
    (CONF_B_LIS,            "set_b_lis",             sensor.Sensor),
    (CONF_ENDSTOP_A,        "set_endstop_a",         binary_sensor.BinarySensor),
    (CONF_ENDSTOP_B,        "set_endstop_b",         binary_sensor.BinarySensor),
    (CONF_CRUISE_PCT,       "set_cruise_pct",        number.Number),
    (CONF_RAMP_MS,          "set_ramp_ms",           number.Number),
    (CONF_EOT_V,            "set_eot_v",             number.Number),
    (CONF_STALL_V,          "set_stall_v",           number.Number),
    (CONF_TRAVEL_TIMEOUT_S, "set_travel_timeout_s",  number.Number),
    (CONF_FAN_RUNNING_PCT,  "set_fan_running_pct",   number.Number),
    (CONF_FAN_COOLDOWN_PCT, "set_fan_cooldown_pct",  number.Number),
    (CONF_SCREEN_DELAY_S,   "set_screen_delay_s",    number.Number),
    (CONF_COOLDOWN_MIN,     "set_cooldown_min",      number.Number),
]

# (config_key, adder_method_name, trigger_class)
_TRIGGERS = [
    (CONF_ON_OPENING,         "add_on_opening_trigger",         OpeningTrigger),
    (CONF_ON_CLOSING,         "add_on_closing_trigger",         ClosingTrigger),
    (CONF_ON_BOOT_CLOSED,     "add_on_boot_closed_trigger",     BootClosedTrigger),
    (CONF_ON_BOOT_OPEN,       "add_on_boot_open_trigger",       BootOpenTrigger),
    (CONF_ON_BOOT_RECONCILED, "add_on_boot_reconciled_trigger", BootReconciledTrigger),
]

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ProjectorLift),
    **{cv.Required(k): cv.use_id(t) for (k, _s, t) in _ID_REFS},
    **{
        cv.Optional(k): automation.validate_automation({
            cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trig_cls),
        })
        for (k, _a, trig_cls) in _TRIGGERS
    },
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for key, setter, _t in _ID_REFS:
        obj = await cg.get_variable(config[key])
        cg.add(getattr(var, setter)(obj))

    for key, adder, _tc in _TRIGGERS:
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
            cg.add(getattr(var, adder)(trigger))
            await automation.build_automation(trigger, [], conf)
