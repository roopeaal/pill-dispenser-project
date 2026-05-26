# Pill Dispenser Project

Embedded Systems Programming course project for a Raspberry Pi Pico based pill dispenser.

The program controls a stepper-driven dispenser wheel, calibrates the wheel with an opto fork sensor, detects dropped pills with a piezo sensor interrupt, stores recovery state in an AT24C256 EEPROM and sends status messages through a LoRa-E5 module.

## Repository Contents

- `project/Pill_dispenser/main.c` - project source code
- `CMakeLists.txt` - Pico SDK build target `pill_dispenser`
- `.run/` - CLion run configurations for flashing and monitoring
- `run-pico.sh` and `flash-pico.sh` - helper scripts for OpenOCD/debug probe flashing
- `tools/serial-monitor.py` - serial monitor used by the CLion monitor configuration
- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.pdf` - final project report
- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.docx` - editable report file

## Hardware Used

- Raspberry Pi Pico / Pico W
- Raspberry Pi Debug Probe
- Course second-year board
- Stepper motor dispenser mechanism
- Opto fork reference sensor
- Piezo pill-drop sensor
- AT24C256 EEPROM
- LoRa-E5 module on UART1

## Main Functionality

- SW0 starts calibration and then starts the dispensing schedule.
- The opto fork sensor is used to find and center the dispenser wheel reference position.
- The stepper motor advances one dispenser compartment at a time.
- The piezo sensor is handled with a GPIO interrupt and is used to detect whether a pill dropped.
- EEPROM stores the operating state so that an interrupted motor turn can be recovered after reset.
- LoRa-E5 sends status messages such as boot, calibration, start, dispense result and empty state.

## Build

Set `PICO_SDK_PATH` to the Pico SDK directory, configure CMake and build:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target pill_dispenser -j 6
```

## Run and Test

The included `.run` configurations can flash the target through OpenOCD and the debug probe:

- `Flash pill_dispenser`
- `Run pill_dispenser + Monitor`

The monitor shows startup, calibration, LoRa joining, schedule start and dispense status output.
If LoRaWAN cannot be joined outside the school gateway coverage, the firmware continues normally and prints `LoRa status` lines to show which AT+MSG messages would be sent.

Normal test flow:

1. Flash `pill_dispenser`.
2. Open the monitor.
3. Press SW0 once to calibrate the dispenser wheel.
4. Press SW0 again to start the dispensing schedule.
5. Confirm that the motor advances one compartment at each interval.
6. Confirm that piezo detection changes the reported dispense result.
7. Reset during a motor turn to test EEPROM recovery.

## LoRa AppKey

The source code contains the AppKey assigned to the course LoRa-E5 module used with this project. The AppKey is used only for configuring the module and is not printed to the debug console.

## Report

The final report is included in both Word and PDF format:

- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.docx`
- `project/Pill_dispenser/Pill_Dispenser_Report_Metropolia_Template.pdf`
