// See projector_lift.h for the design overview.
//
// Safety invariants:
//   1. set_state_(FAULT) or set_state_(CLOSED) is always paired with
//      kill_motion_() — PWM off, BTS off, phase_ = IDLE, timers cancelled.
//   2. Every scheduled callback re-checks state_ up front. If a callback
//      fires after emergency_stop(), it becomes a no-op.
//   3. Motion outputs (PWM + BTS + screen) only turn ON inside the
//      request_open() / request_close() ladders. Never inside set_state_().

#include "projector_lift.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"          // millis()
#include "esp_system.h"                // esp_reset_reason()

namespace esphome {
namespace projector_lift {

static const char *const TAG = "lift";

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

static const char *state_name_(State s) {
  switch (s) {
    case State::CLOSED:   return "CLOSED";
    case State::OPENING:  return "OPENING";
    case State::OPEN:     return "OPEN";
    case State::CLOSING:  return "CLOSING";
    case State::COOLDOWN: return "COOLDOWN";
    case State::FAULT:    return "FAULT";
  }
  return "?";
}

// Decode esp_reset_reason() so boot logs make the reboot cause obvious.
// POWERON / EXT are expected (cold boot, external reset); PANIC / TASK_WDT /
// INT_WDT / BROWNOUT are the ones worth chasing if they show up mid-session.
static const char *reset_reason_str_(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// Component lifecycle.
// ---------------------------------------------------------------------------

void ProjectorLift::setup() {
  // Log why we booted. A pattern of PANIC / BROWNOUT / TASK_WDT here across
  // multiple boots is the diagnostic hook for chasing the mid-session screen
  // drop bug — the reconcile fix restores outputs, but the underlying reboot
  // cause still needs finding.
  ESP_LOGI(TAG, "boot: reset_reason=%s", reset_reason_str_(esp_reset_reason()));

  // Pull every motion output to a known-safe state before WiFi is up — same
  // as the old on_boot priority=-100 `enter_closed` script.
  kill_motion_();
  fan_->turn_off().set_transition_length(0).perform();
  screen_->turn_off();
  state_ = State::CLOSED;
  phase_ = Phase::IDLE;
  ESP_LOGI(TAG, "setup → CLOSED");
}

void ProjectorLift::loop() {
  if (phase_ != Phase::CRUISING) return;

  // Extending vs retracting decides which IS pair to watch.
  //   OPENING: L_IS drops when PA-04 internal extend limit opens the motor.
  //   CLOSING: R_IS drops when PA-04 internal retract limit opens the motor.
  bool extending = (state_ == State::OPENING);
  if (!extending && state_ != State::CLOSING) return;  // state raced out

  int result = check_motion_(extending);
  if (result == 1) {
    // EOT reached — normal completion.
    if (extending) open_finish_(State::OPEN, "→ OPEN");
    else           close_finish_(State::COOLDOWN, "→ COOLDOWN");
    return;
  }
  if (result == 2) {
    // Sustained current above stall threshold — obstruction or broken limit.
    if (extending) open_finish_(State::FAULT, "OPENING stall — L_IS high");
    else           close_finish_(State::FAULT, "CLOSING stall — R_IS high");
    return;
  }

  // Watchdog: fell off the end of travel_timeout_s without EOT or stall.
  uint32_t elapsed = millis() - cruise_started_ms_;
  if (elapsed > (uint32_t)(travel_timeout_s_->state * 1000)) {
    if (extending) open_finish_(State::FAULT, "OPENING timeout — L_IS never collapsed");
    else           close_finish_(State::FAULT, "CLOSING timeout — R_IS never collapsed");
  }
}

// ---------------------------------------------------------------------------
// State transitions.
// ---------------------------------------------------------------------------

void ProjectorLift::set_state_(State s, const char *why) {
  if (s == state_) return;
  ESP_LOGI(TAG, "%s → %s (%s)", state_name_(state_), state_name_(s), why);
  state_ = s;
}

void ProjectorLift::kill_motion_() {
  cancel_timeout("open_deadtime");
  cancel_timeout("open_ramp_settle");
  cancel_timeout("close_deadtime");
  cancel_timeout("close_ramp_settle");
  cancel_timeout("postclose");
  cancel_timeout("cooldown_dwell");
  // Transition length 0 = instant kill. Faster than 100 ms is also safer if
  // a wedged scheduler is the reason we're here.
  rpwm_->turn_off().set_transition_length(0).perform();
  lpwm_->turn_off().set_transition_length(0).perform();
  bts_a_->turn_off();
  bts_b_->turn_off();
  phase_ = Phase::IDLE;
  eot_low_since_ = 0;
  stall_high_since_ = 0;
}

// ---------------------------------------------------------------------------
// EOT / stall check — the meat of the old wait_until lambda.
// ---------------------------------------------------------------------------

int ProjectorLift::check_motion_(bool extending) {
  float a = extending ? a_lis_->state : a_ris_->state;
  float b = extending ? b_lis_->state : b_ris_->state;
  float eot   = eot_v_->state;
  float stall = stall_v_->state;
  uint32_t now = millis();

  // EOT: both IS pins sit below eot_v for 250 ms sustained.
  bool both_low = (a < eot) && (b < eot);
  if (!both_low) {
    eot_low_since_ = 0;
  } else if (eot_low_since_ == 0) {
    eot_low_since_ = now;
  }
  if (eot_low_since_ != 0 && now - eot_low_since_ >= 250) return 1;

  // Stall: either IS pin above stall_v for 100 ms sustained.
  bool either_high = (a > stall) || (b > stall);
  if (!either_high) {
    stall_high_since_ = 0;
  } else if (stall_high_since_ == 0) {
    stall_high_since_ = now;
  }
  if (stall_high_since_ != 0 && now - stall_high_since_ >= 100) return 2;

  return 0;
}

// ---------------------------------------------------------------------------
// OPEN sequence.
// ---------------------------------------------------------------------------

void ProjectorLift::request_open() {
  if (state_ == State::FAULT) {
    ESP_LOGW(TAG, "request_open ignored — in FAULT, run Diag: reset first");
    return;
  }
  if (state_ == State::OPEN && phase_ == Phase::IDLE) {
    // Already there and idle — nothing to do.
    return;
  }
  // Cancel any in-flight motion (mirrors YAML `mode: restart`).
  kill_motion_();
  set_state_(State::OPENING, "request_open");

  fire_(on_opening_);
  screen_->turn_on();
  {
    auto call = fan_->turn_on();
    call.set_brightness(fan_running_pct_->state / 100.0f);
    call.set_transition_length(200);
    call.perform();
  }
  rpwm_->turn_off().set_transition_length(0).perform();

  set_timeout("open_deadtime", 50, [this]() { open_after_deadtime_(); });
}

void ProjectorLift::open_after_deadtime_() {
  if (state_ != State::OPENING) return;
  bts_a_->turn_on();
  bts_b_->turn_on();
  {
    auto call = lpwm_->turn_on();
    call.set_brightness(cruise_pct_->state / 100.0f);
    call.set_transition_length((uint32_t)ramp_ms_->state);
    call.perform();
  }
  // Hold off EOT / stall checks until ramp completes + 250 ms current settle.
  uint32_t settle_ms = (uint32_t)ramp_ms_->state + 250;
  set_timeout("open_ramp_settle", settle_ms, [this]() { open_after_ramp_settle_(); });
}

void ProjectorLift::open_after_ramp_settle_() {
  if (state_ != State::OPENING) return;
  eot_low_since_ = 0;
  stall_high_since_ = 0;
  cruise_started_ms_ = millis();
  phase_ = Phase::CRUISING;
}

void ProjectorLift::open_finish_(State result, const char *reason) {
  phase_ = Phase::IDLE;
  // Ramp PWM back down over ramp_ms, then cut BTS. Match YAML: BTS is
  // cut immediately after issuing the transition — the PWM ramp completes
  // into hi-Z outputs, which is fine.
  lpwm_->turn_off().set_transition_length((uint32_t)ramp_ms_->state).perform();
  bts_a_->turn_off();
  bts_b_->turn_off();
  set_state_(result, reason);
  if (result == State::OPEN) {
    // Endstops should both be released by now (we extended away from home).
    if (endstop_a_->state || endstop_b_->state) {
      ESP_LOGW(TAG, "OPEN reached but endstop still tripped (a=%d b=%d) — check actuator",
               endstop_a_->state, endstop_b_->state);
    }
  } else {
    ESP_LOGW(TAG, "%s (stall_v=%.2f a_lis=%.2f b_lis=%.2f)",
             reason, stall_v_->state, a_lis_->state, b_lis_->state);
  }
}

// ---------------------------------------------------------------------------
// CLOSE sequence.
// ---------------------------------------------------------------------------

void ProjectorLift::request_close() {
  if (state_ == State::FAULT) {
    ESP_LOGW(TAG, "request_close ignored — in FAULT, run Diag: reset first");
    return;
  }
  if (state_ == State::CLOSED || state_ == State::COOLDOWN) {
    // Already retracted (or cooling down) — nothing to do.
    return;
  }
  kill_motion_();
  set_state_(State::CLOSING, "request_close");

  fire_(on_closing_);
  lpwm_->turn_off().set_transition_length(0).perform();

  set_timeout("close_deadtime", 50, [this]() { close_after_deadtime_(); });
}

void ProjectorLift::close_after_deadtime_() {
  if (state_ != State::CLOSING) return;
  bts_a_->turn_on();
  bts_b_->turn_on();
  {
    auto call = rpwm_->turn_on();
    call.set_brightness(cruise_pct_->state / 100.0f);
    call.set_transition_length((uint32_t)ramp_ms_->state);
    call.perform();
  }
  uint32_t settle_ms = (uint32_t)ramp_ms_->state + 250;
  set_timeout("close_ramp_settle", settle_ms, [this]() { close_after_ramp_settle_(); });
}

void ProjectorLift::close_after_ramp_settle_() {
  if (state_ != State::CLOSING) return;
  eot_low_since_ = 0;
  stall_high_since_ = 0;
  cruise_started_ms_ = millis();
  phase_ = Phase::CRUISING;
}

void ProjectorLift::close_finish_(State result, const char *reason) {
  phase_ = Phase::IDLE;
  rpwm_->turn_off().set_transition_length((uint32_t)ramp_ms_->state).perform();
  bts_a_->turn_off();
  bts_b_->turn_off();
  set_state_(result, reason);
  if (result == State::COOLDOWN) {
    // Sanity-check endstops: both should read tripped after a full retract.
    // Don't FAULT if they disagree — could be travel variance — but flag.
    if (!endstop_a_->state || !endstop_b_->state) {
      ESP_LOGW(TAG, "CLOSED (R_IS dropped) but endstop not tripped (a=%d b=%d) — actuator may be short of home",
               endstop_a_->state, endstop_b_->state);
    }
    // Chain: hold screen up for screen_delay_s, then release + step fan down.
    uint32_t hold_ms = (uint32_t)(screen_delay_s_->state * 1000);
    set_timeout("postclose", hold_ms, [this]() { cooldown_after_postclose_(); });
  } else {
    // FAULT: release the screen too. The cooldown chain won't run so the
    // screen would otherwise stay energized.
    ESP_LOGW(TAG, "%s (stall_v=%.2f a_ris=%.2f b_ris=%.2f)",
             reason, stall_v_->state, a_ris_->state, b_ris_->state);
    screen_->turn_off();
  }
}

void ProjectorLift::cooldown_after_postclose_() {
  if (state_ != State::COOLDOWN) return;
  screen_->turn_off();
  {
    auto call = fan_->turn_on();
    call.set_brightness(fan_cooldown_pct_->state / 100.0f);
    call.set_transition_length(1000);
    call.perform();
  }
  uint32_t dwell_ms = (uint32_t)(cooldown_min_->state * 60.0f * 1000.0f);
  set_timeout("cooldown_dwell", dwell_ms, [this]() { cooldown_after_dwell_(); });
}

void ProjectorLift::cooldown_after_dwell_() {
  if (state_ != State::COOLDOWN) return;
  fan_->turn_off().set_transition_length(1000).perform();
  set_state_(State::CLOSED, "cooldown done");
}

// ---------------------------------------------------------------------------
// Public commands.
// ---------------------------------------------------------------------------

void ProjectorLift::emergency_stop() {
  ESP_LOGW(TAG, "EMERGENCY STOP — run 'Diag: reset to CLOSED' to recover");
  kill_motion_();
  screen_->turn_off();
  set_state_(State::FAULT, "emergency_stop");
  // Note: fan is intentionally left running — if the projector was on,
  // stored heat should keep flowing until it's clear.
}

void ProjectorLift::force_closed() {
  // Diag button: fully reset to the CLOSED baseline. Everything off.
  kill_motion_();
  fan_->turn_off().set_transition_length(0).perform();
  screen_->turn_off();
  set_state_(State::CLOSED, "force_closed");
}

void ProjectorLift::on_button_press() {
  bool both_tripped = endstop_a_->state && endstop_b_->state;
  ESP_LOGI(TAG, "button press (state=%s both_endstops=%d)",
           state_name_(state_), both_tripped);
  if (state_ == State::FAULT) {
    ESP_LOGW(TAG, "in FAULT — ignoring button; run Diag: reset");
    return;
  }
  if (both_tripped) {
    // Box is home → open. If we were in COOLDOWN we need to cancel that
    // chain first so the fan/screen timers don't stomp on us later.
    if (state_ == State::COOLDOWN) {
      ESP_LOGI(TAG, "canceling cooldown");
      cancel_timeout("postclose");
      cancel_timeout("cooldown_dwell");
    }
    request_open();
  } else {
    // Box is not confirmed home → close (return to safe endpoint).
    request_close();
  }
}

void ProjectorLift::reconcile_from_endstops() {
  // Called by boot_reconcile (yaml) after WiFi is up + endstops have
  // settled. Derives state from physical endstops so power-loss-while-open
  // reboots recover the correct AVR / projector state.
  bool a = endstop_a_->state;
  bool b = endstop_b_->state;
  if (a && b) {
    ESP_LOGI(TAG, "boot reconcile: both endstops tripped → CLOSED (confirmed)");
    // Leave state as-is (setup() already set CLOSED).
    fire_(on_boot_closed_);
  } else if (!a && !b) {
    set_state_(State::OPEN, "boot reconcile: neither endstop tripped");
    // Box was physically OPEN before this boot — projector is presumably still
    // running. setup() (and screen_relay's ALWAYS_OFF restore) zeroed the
    // screen relay + fan; restore them so a mid-session reboot doesn't
    // silently drop the screen or stop cooling while state reads OPEN.
    screen_->turn_on();
    {
      auto call = fan_->turn_on();
      call.set_brightness(fan_running_pct_->state / 100.0f);
      call.set_transition_length(200);
      call.perform();
    }
    fire_(on_boot_open_);
  } else {
    // Asymmetric endstops = mid-motion power loss or one endstop stuck. Prior
    // behavior FAULTed here, which locks out the wall button — parents don't
    // know about the Diag reset. Safer default: treat as OPEN. Wall-button
    // dispatch (see on_button_press) reads endstop state directly and will
    // drive a close on the next press since not both are tripped. Restore
    // outputs so the projector cooling + screen stay coherent with state.
    ESP_LOGW(TAG, "boot reconcile: endstops asymmetric (a=%d b=%d) — treating as OPEN; wall button will retract",
             a, b);
    set_state_(State::OPEN, "boot reconcile: asymmetric endstops");
    screen_->turn_on();
    {
      auto call = fan_->turn_on();
      call.set_brightness(fan_running_pct_->state / 100.0f);
      call.set_transition_length(200);
      call.perform();
    }
    fire_(on_boot_open_);
  }
  fire_(on_boot_reconciled_);
}

}  // namespace projector_lift
}  // namespace esphome
