# Pill Dispenser Project

Raspberry Pi Pico course project for an embedded pill dispenser.

The project controls a stepper-driven dispenser wheel, calibrates the wheel with an opto fork sensor, detects dropped pills with a piezo sensor interrupt, stores recovery state in an AT24C256 EEPROM and sends status messages through a LoRa-E5 module.

## Hardware

- Raspberry Pi Pico / Pico W
- Raspberry Pi Debug Probe
- Course second-year board
- Stepper motor dispenser mechanism
- Opto fork reference sensor
- Piezo pill-drop sensor
- AT24C256 EEPROM
- LoRa-E5 module on UART1

## Build

Set `PICO_SDK_PATH` to the Pico SDK directory, configure CMake and build:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target pill_dispenser -j 6
```

## Run From CLion

The included `.run` configurations can flash the target through OpenOCD and the debug probe:

- `Flash pill_dispenser`
- `Run pill_dispenser + Monitor`

The monitor shows calibration, LoRa joining and dispense status output.

## LoRa AppKey

The source file intentionally contains a placeholder AppKey:

```c
#define LORA_APP_KEY "00000000000000000000000000000000"
```

Use the AppKey assigned to the LoRa-E5 module in the course device list before flashing to hardware. Do not commit real course keys to a public repository.

## Report

The report is included in both Word and PDF format:

- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.docx`
- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.pdf`
