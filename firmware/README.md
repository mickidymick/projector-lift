# Projector lift — ESP32 firmware

ESPHome config for the porch projector lift box. Wiring + state-machine
reference: [`../index.html`](../index.html) (rendered at
<https://mickidymick.github.io/projector-lift/>) — §9 (wiring sketch),
§10 (firmware target), §12 (AV chain Telnet + PJLink), §13 (Alexa).

## Pin map

| GPIO  | Direction      | Connects to                                                |
|-------|----------------|------------------------------------------------------------|
| 25    | OUT (PWM)      | RPWM on BTS7960 #1 + #2 (retract / close, tied)            |
| 26    | OUT (PWM)      | LPWM on BTS7960 #1 + #2 (extend  / open,  tied)            |
| 27    | OUT            | BTS7960 #1 R_EN + L_EN (actuator A independent enable)     |
| 19    | OUT            | BTS7960 #2 R_EN + L_EN (actuator B independent enable)     |
| 32    | ADC1_4         | BTS7960 #1 R_IS — actuator A retract (close) current       |
| 33    | ADC1_5         | BTS7960 #1 L_IS — actuator A extend  (open)  current       |
| 34    | ADC1_6         | BTS7960 #2 R_IS — actuator B retract (close) current       |
| 35    | ADC1_7         | BTS7960 #2 L_IS — actuator B extend  (open)  current       |
| 17    | OUT (PWM)      | IRLZ44N gate via 220 Ω — fan PWM (6.5 V rail)             |
| 16    | OUT            | 1 kΩ → BC547 base → F-1001 coil (screen relay)             |
| 18    | IN, pull-up    | Momentary wall switch (Square D X-Series), switch-to-GND   |
| 23    | IN, pull-up    | Endstop A SIG (deployed end, Prusa MK3 NO-to-GND)          |
| 22    | IN, pull-up    | Endstop B SIG (deployed end, Prusa MK3 NO-to-GND)          |

All current-sense pins are on ADC1 (ADC2 is unusable while WiFi is active).
Endstop +Vcc is taken from the ESP32's 3V3 pin (AMS1117 output) so the Prusa
board's pull-up/LED network can never drive SIG above the ESP32's 3.6 V max.

## First-time setup

```bash
cp secrets.yaml.example secrets.yaml
$EDITOR secrets.yaml          # fill WiFi creds + generate keys
```

Edit the `avr_ip` and `projector_ip` substitutions at the top of
`projector-lift.yaml` to match your DHCP reservations.

### Option A — pip / native

```bash
pip install esphome
esphome run projector-lift.yaml --device /dev/ttyUSB0
```

### Option B — Docker (no pip pollution)

```bash
docker run --rm -it \
  --user $(id -u):$(id -g) \
  -e HOME=/config \
  -v "$PWD":/config -w /config \
  --device=/dev/ttyUSB0 \
  ghcr.io/esphome/esphome:latest run projector-lift.yaml --device /dev/ttyUSB0
```

`HOME=/config` gives platformio a writable home for `.platformio/` — without
it, the compile aborts on `PermissionError: '/.platformio'` because uid 1000
has no home in the container.

After the first wired flash, subsequent updates can go over WiFi. mDNS does
not resolve inside the container, so pass `--device <IP>` explicitly:

```bash
docker run --rm --user $(id -u):$(id -g) -e HOME=/config \
  -v "$PWD":/config -w /config \
  ghcr.io/esphome/esphome:latest upload projector-lift.yaml --device 10.0.0.175
```

**Do not add `auth:` to `web_server:`** — ESPHome web_server v3 (2026.6.3)
silently disables the SPA's button-click dispatch when auth is enabled. The
state stream keeps working so the failure is invisible until you try to
press a button. LAN-only is the trust boundary; use a reverse proxy if you
need auth.

## Bench bring-up sequence

Don't wire the whole box at once. Work outward from the ESP32:

1. **ESP32 + LM2596 only.** Power the LM2596 from 12 V, confirm 5 V on its
   output, feed ESP32 via the 5V/VIN pin. Flash firmware. Confirm WiFi + HA
   discovery. Confirm `Wall button` toggles when you short GPIO18 to GND with
   a wire, and that `Endstop A` / `Endstop B` toggle when you short GPIO23 /
   GPIO22 to GND.

2. **Add ONE BTS7960 + ONE PA-04.** Wire actuator A only — RPWM/LPWM to the
   BTS's PWM pins, GPIO27 to its R_EN + L_EN, current-sense pins back to
   GPIO32 + GPIO33. Verify with `Diag: jog extend 500ms` and
   `Diag: jog retract 500ms`. Watch `Actuator A R_IS` / `L_IS` — voltage
   spikes during motion, drops when the motor stops.

3. **Calibrate the EOT threshold.** Trigger a longer retract by holding
   `Diag: jog retract` repeatedly, or temporarily extend the jog duration in
   the YAML. Watch the `Actuator A R_IS` reading:
   - **Running**: voltage rails high (the on-module 10 kΩ pulldown saturates
     the ADC at expected motor current — this is fine, we only care about
     the low end).
   - **End-of-travel** (PA-04 internal limit opens the motor circuit): drops
     to near 0 V.

   Set `End-of-travel threshold` somewhere comfortably above the noise floor
   but below the lowest "running" reading you ever see — 0.1–0.3 V is the
   usual band. Stall detection is not implemented in this build; we rely on
   the deployed-end endstops on extend and the PA-04 internal limits on
   retract, with `Travel safety timeout` as the failsafe in both directions.

4. **Add second BTS7960 + PA-04.** Wire actuator B with GPIO19 to its EN
   pins and GPIO34 / GPIO35 for current sense. Re-test jogs — both actuators
   should move together (RPWM/LPWM are tied). If one runs noticeably faster
   it's a mechanical imbalance, not electrical.

5. **Add endstops.** Mount one Prusa MK3 endstop on each main-box wall at the
   chosen deployed position; mount a small bracket on the top of the inner
   box so it trips both endstops as the box descends to that position. Wire
   NO to GND, SIG to GPIO23/22, +Vcc to the perfboard's 3.3 V rail (not 5 V —
   see warning in doc §8). Confirm `Endstop A` / `Endstop B` flip when you
   manually actuate them. With both BTSes wired, run a full extend via the
   HA cover — the lift should stop the moment both sides' endstops trip,
   with the first-tripped side's EN dropping while the other continues.
   (The cover's wait_until requires *both* endstops to fire; with only one
   BTS wired the second endstop never trips and the open sequence terminates
   on the `Travel safety timeout` instead — useful as a fallback test, but
   not the primary check.)

6. **Add screen relay.** Connect GPIO16 → 1 kΩ → BC547 base, BC547 collector
   → F-1001 trigger. Test the `Screen relay` switch in HA. Listen for the
   F-1001 click.

7. **Add fan-driver board.** Connect GPIO17 (via the FAN SIG plug, 220 Ω in
   series at the fan-driver perfboard) to the IRLZ44N gate. Use the
   `Fan running duty` slider in HA to verify smooth PWM control of both
   AIRPLATE fans.

8. **Full cycle.** Use the cover "open" / "close" in HA. Confirm the full
   state machine: CLOSED → OPENING → OPEN → CLOSING → COOLDOWN → CLOSED.
   Watch `Lift state` text sensor for transitions.

9. **Wire up AVR + projector.** Set DHCP reservations for both, update the
   `avr_ip` / `projector_ip` substitutions, re-flash. On the AVR enable
   Network → Network Standby (so it accepts Telnet while in standby). On the
   Epson set Network → PJLink → Authentication to OFF (or add MD5 auth
   support to the firmware). Use the diag buttons (`Diag: AVR → INDOOR`
   etc.) to confirm each command takes effect.

## State machine

| State    | Trigger                | Behavior                                                                 |
|----------|------------------------|--------------------------------------------------------------------------|
| CLOSED   | boot / cooldown done   | All outputs OFF. BTS ENs LOW. Wait for button or HA open.                |
| OPENING  | open command           | PJLink ON, AVR → OUTDOOR, screen drops, fan PWM at running duty, ENs HIGH, LPWM ramps. Each endstop trip drops its own EN; both tripped ⇒ OPEN. Timeout ⇒ FAULT. |
| OPEN     | both endstops tripped  | PWM 0, ENs LOW. Fan stays at running duty. AVR polled every 5 s for HA visibility. Wait for close. |
| CLOSING  | close command          | PJLink OFF, AVR → INDOOR, ENs HIGH, RPWM ramps. R_IS collapse on both sides ⇒ COOLDOWN. Timeout ⇒ FAULT. |
| COOLDOWN | retract EOT            | Screen retracts after `screen_delay_s`. Fan steps down to cooldown duty for `cooldown_min`. ENs LOW. |
| FAULT    | travel timeout / panic | All PWM off, ENs LOW. Manual reset via `Diag: reset to CLOSED`.          |

The wall button is a **momentary single-press** input — each press dispatches
based on endstop state, not lift state: both deployed-end endstops tripped
⇒ CLOSE, otherwise ⇒ OPEN. A press during CLOSING reverses cleanly to
OPENING; a press during COOLDOWN cancels the timer and reopens; a press
during FAULT is ignored (run the Diag reset instead).

## Tunables (HA `number` entities, persisted across reboots)

- `Cooldown duration` — 30 min default
- `PWM ramp duration` — 500 ms soft-start/stop
- `Cruise duty` — 100 % default; lower for quieter / slower travel
- `Fan running duty` — 100 % during OPENING/OPEN/CLOSING
- `Fan cooldown duty` — 50 % during the long post-close dwell
- `End-of-travel threshold (V)` — calibrate per step 3 (only used for
  CLOSING; OPENING uses the endstops)
- `Screen post-close delay` — 3 s; how long the box settles before the
  screen retracts
- `Travel safety timeout` — 15 s; hard cap on any single motion phase

## Network endpoints

The firmware opens TCP connections to the AVR and projector on lift state
changes:

- **Denon AVR-X2800H** (`${avr_ip}:23`) — VSMONI / MU / Z2 commands per §12
  - OPENING fires `VSMONI2 / MUON / Z2SOURCE / Z2ON`
  - CLOSING fires `VSMONI1 / MUOFF / Z2OFF`
  - `avr_both` script fires `VSMONIAUTO / MUOFF / Z2SOURCE / Z2ON` — callable
    from a HA automation when the Sony TV is woken with its own remote
    during OUTDOOR mode
  - Polled every 5 s while OPEN — exposes `AVR monitor mode` text sensor
- **Epson PowerLite L730U** (`${projector_ip}:4352`) — PJLink Class 1
  - OPENING fires `%1POWR 1` (before the actuators move)
  - CLOSING fires `%1POWR 0` (before the actuators move)
  - Boot fires `%1POWR ?` for state reconciliation
  - Requires PJLink Authentication = OFF on the projector (or extend the
    scripts to do the MD5 challenge dance)

Both endpoints should have DHCP reservations on the parents' router so the
substitutions in the YAML stay valid.

## What's intentionally NOT here

- **Position feedback.** PA-04 internal limits + the new external endstops
  give us "open" + "closed" for free; encoders would be overkill.
- **BOTH-mode auto-detect from the AVR alone.** The X2800H doesn't expose a
  reliable "Sony TV just woke" signal over Telnet without empirical testing.
  The polling skeleton + `avr_both` script + `AVR monitor mode` text sensor
  are wired up so a HA automation can decide when to fire it.
- **PJLink MD5 authentication.** Assumed off; add if the projector's network
  needs auth enabled for compliance reasons.
- **Static IP for the ESP32.** Use a DHCP reservation on the router — same
  outcome with one less place to keep in sync.
