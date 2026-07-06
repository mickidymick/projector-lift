// Projector-lift state machine.
//
// Owns the OPEN/CLOSE/EMERGENCY-STOP flows that used to live in the YAML as
// scripts + globals. Fixed-duration waits (deadtime, ramp settle, screen post-
// close delay, cooldown) run as scheduled callbacks; sensor monitoring during
// the CRUISING phase runs in loop(). Every scheduled callback and loop() step
// re-checks state_ up front so emergency_stop() is safe by construction —
// changing state_ = FAULT makes every subsequent callback a no-op.
#pragma once

#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace projector_lift {

// Mirrors the integer values the old YAML lift_state global used so any
// remaining lambdas that compare against 0/1/2/3/4/5 keep working:
//   0=CLOSED  1=OPENING  2=OPEN  3=CLOSING  4=COOLDOWN  5=FAULT
enum class State : int {
  CLOSED   = 0,
  OPENING  = 1,
  OPEN     = 2,
  CLOSING  = 3,
  COOLDOWN = 4,
  FAULT    = 5,
};

// Sub-phase of a motion command. IDLE means we're not currently running the
// state machine (state_ is CLOSED / OPEN / COOLDOWN / FAULT).
enum class Phase {
  IDLE,
  CRUISING,     // ramp settled, monitoring IS pins for EOT / stall / timeout
};

class ProjectorLift : public Component {
 public:
  // Component API.
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Setters populated by codegen (__init__.py).
  void set_bts_a(switch_::Switch *s)                 { bts_a_ = s; }
  void set_bts_b(switch_::Switch *s)                 { bts_b_ = s; }
  void set_screen_relay(switch_::Switch *s)          { screen_ = s; }
  void set_rpwm(light::LightState *l)                { rpwm_ = l; }
  void set_lpwm(light::LightState *l)                { lpwm_ = l; }
  void set_fan_pwm(light::LightState *l)             { fan_ = l; }
  void set_a_ris(sensor::Sensor *s)                  { a_ris_ = s; }
  void set_a_lis(sensor::Sensor *s)                  { a_lis_ = s; }
  void set_b_ris(sensor::Sensor *s)                  { b_ris_ = s; }
  void set_b_lis(sensor::Sensor *s)                  { b_lis_ = s; }
  void set_endstop_a(binary_sensor::BinarySensor *b) { endstop_a_ = b; }
  void set_endstop_b(binary_sensor::BinarySensor *b) { endstop_b_ = b; }
  void set_cruise_pct(number::Number *n)             { cruise_pct_ = n; }
  void set_ramp_ms(number::Number *n)                { ramp_ms_ = n; }
  void set_eot_v(number::Number *n)                  { eot_v_ = n; }
  void set_stall_v(number::Number *n)                { stall_v_ = n; }
  void set_travel_timeout_s(number::Number *n)       { travel_timeout_s_ = n; }
  void set_fan_running_pct(number::Number *n)        { fan_running_pct_ = n; }
  void set_fan_cooldown_pct(number::Number *n)       { fan_cooldown_pct_ = n; }
  void set_screen_delay_s(number::Number *n)         { screen_delay_s_ = n; }
  void set_cooldown_min(number::Number *n)           { cooldown_min_ = n; }

  void add_on_opening_trigger(Trigger<> *t)          { on_opening_.push_back(t); }
  void add_on_closing_trigger(Trigger<> *t)          { on_closing_.push_back(t); }
  void add_on_boot_closed_trigger(Trigger<> *t)      { on_boot_closed_.push_back(t); }
  void add_on_boot_open_trigger(Trigger<> *t)        { on_boot_open_.push_back(t); }
  void add_on_boot_reconciled_trigger(Trigger<> *t)  { on_boot_reconciled_.push_back(t); }

  // Public command surface — callable from YAML lambdas.
  void request_open();
  void request_close();
  void emergency_stop();
  void on_button_press();
  void force_closed();                // used by "Diag: reset to CLOSED"
  void reconcile_from_endstops();     // used by boot_reconcile after wifi

  // Public read-only accessors.
  State get_state() const { return state_; }
  int get_state_int() const { return static_cast<int>(state_); }
  bool is_projector_active() const {
    return state_ == State::OPENING || state_ == State::OPEN;
  }
  bool is_open() const { return state_ == State::OPEN; }

 protected:
  // State transitions. set_state_() is the only path that flips outputs.
  void set_state_(State s, const char *why);

  // Fixed-duration steps of the OPEN / CLOSE sequences, chained via
  // set_timeout callbacks so their delays match the old YAML semantics.
  void open_after_deadtime_();
  void open_after_ramp_settle_();
  void open_finish_(State result, const char *reason);
  void close_after_deadtime_();
  void close_after_ramp_settle_();
  void close_finish_(State result, const char *reason);
  void cooldown_after_postclose_();
  void cooldown_after_dwell_();

  // CRUISING-phase sensor check. Returns:
  //   0 → keep running
  //   1 → EOT reached (motor circuit opened, current dropped)
  //   2 → stall (current spiked above stall_v)
  int check_motion_(bool extending);

  // Fire every attached Trigger<> in a vector.
  void fire_(const std::vector<Trigger<> *> &triggers) {
    for (auto *t : triggers) t->trigger();
  }

  // Halt anything moving. Called by set_state_() when leaving a motion state.
  void kill_motion_();

  // ---- State ------------------------------------------------------------
  State state_{State::CLOSED};
  Phase phase_{Phase::IDLE};

  uint32_t cruise_started_ms_{0};
  uint32_t eot_low_since_{0};
  uint32_t stall_high_since_{0};

  // ---- Injected refs ----------------------------------------------------
  switch_::Switch *bts_a_{nullptr};
  switch_::Switch *bts_b_{nullptr};
  switch_::Switch *screen_{nullptr};
  light::LightState *rpwm_{nullptr};
  light::LightState *lpwm_{nullptr};
  light::LightState *fan_{nullptr};
  sensor::Sensor *a_ris_{nullptr};
  sensor::Sensor *a_lis_{nullptr};
  sensor::Sensor *b_ris_{nullptr};
  sensor::Sensor *b_lis_{nullptr};
  binary_sensor::BinarySensor *endstop_a_{nullptr};
  binary_sensor::BinarySensor *endstop_b_{nullptr};
  number::Number *cruise_pct_{nullptr};
  number::Number *ramp_ms_{nullptr};
  number::Number *eot_v_{nullptr};
  number::Number *stall_v_{nullptr};
  number::Number *travel_timeout_s_{nullptr};
  number::Number *fan_running_pct_{nullptr};
  number::Number *fan_cooldown_pct_{nullptr};
  number::Number *screen_delay_s_{nullptr};
  number::Number *cooldown_min_{nullptr};

  std::vector<Trigger<> *> on_opening_;
  std::vector<Trigger<> *> on_closing_;
  std::vector<Trigger<> *> on_boot_closed_;
  std::vector<Trigger<> *> on_boot_open_;
  std::vector<Trigger<> *> on_boot_reconciled_;
};

// Trigger<> subclasses so the Python codegen can `declare_id()` them.
class OpeningTrigger        : public Trigger<> {};
class ClosingTrigger        : public Trigger<> {};
class BootClosedTrigger     : public Trigger<> {};
class BootOpenTrigger       : public Trigger<> {};
class BootReconciledTrigger : public Trigger<> {};

}  // namespace projector_lift
}  // namespace esphome
