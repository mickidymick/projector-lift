# Projector lift

A motorized porch-projector lift box with on-wall and Alexa control,
networked AVR + projector orchestration over Telnet and PJLink, and an
ESP32 running ESPHome. Built for my dad's covered porch.

## The build doc

Full design — site measurements, throw calculator, projector + AVR
selection, BOM, fabrication, wiring sketches, perfboard layouts, firmware
state machine, AV chain, Alexa integration — is one self-contained HTML
document with hand-drawn SVG diagrams.

Rendered: **<https://mickidymick.github.io/projector-lift/>**

Source: [`index.html`](./index.html)

## Firmware

ESPHome config for the lift's ESP32 lives in [`firmware/`](./firmware/).
Pin map, bench bring-up sequence, calibration notes, and network endpoints
are in [`firmware/README.md`](./firmware/README.md).

## Layout

```
.
├── index.html        # the design + build doc (served by GitHub Pages)
├── firmware/         # ESPHome control firmware
│   ├── projector-lift.yaml
│   ├── includes.h
│   ├── secrets.yaml.example
│   └── README.md
└── README.md
```
