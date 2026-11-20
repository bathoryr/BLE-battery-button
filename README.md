# Battery-Powered XIAO nRF52840 BTHome Buttons

Firmware for a Seeed Studio XIAO nRF52840 that acts as a low-power, two-button. Each button advertises a BTHome v2 event that Home Assistant can receive through its Bluetooth integration.

## Features

- Two independent inputs on D1 and D2, using internal pull-ups.
- Separate BLE identities for the two buttons:
  - D1: `Button-D1`
  - D2: `Button-D2`
- BTHome v2 button events for single, double, triple, and long presses.
- A persistent per-button BTHome packet ID so repeated identical gestures are
  reported as distinct events.
- Battery-percentage measurement using the XIAO battery-voltage input.
- Returns to nRF52840 System OFF between button events.
- Sends the onboard QSPI flash a Deep Power-Down command before sleeping.
- Disables Bluefruit's automatic status LED to reduce active current.

## Hardware and wiring

The project targets the **Seeed Studio XIAO nRF52840**.

Connect each normally-open button between its GPIO and GND:

| Button | XIAO pin | Connection |
|---|---|---|
| 1 | D1 | D1 to GND |
| 2 | D2 | D2 to GND |

The firmware enables the internal pull-up, so an unpressed button reads HIGH
and a pressed button reads LOW. Only one button should be pressed at a time;
if both wake the board simultaneously, D1 takes priority.


## Button events

The first press wakes the MCU from System OFF. The firmware uses the nRF52840
reset reason and GPIO latch to identify the waking button, including when a
short press has been released before the application starts.

| Gesture | BTHome object | Event value |
|---|---|---:|
| Single press | Button (`0x3A`) | `0x01` |
| Double press | Button (`0x3A`) | `0x02` |
| Triple press | Button (`0x3A`) | `0x03` |
| Long press (at least 800 ms) | Button (`0x3A`) | `0x04` |

The inter-click window is 400 ms and button debounce is 25 ms. After detecting
the gesture, the device advertises for about 1.5 seconds, then returns to
System OFF. It cannot count clicks while in System OFF; the MCU remains awake
for the gesture window after the wake-up press.

## BLE and Home Assistant

Each button uses its own BLE address and advertised local name so Home
Assistant discovers them as separate BTHome devices. Add the **BTHome**
integration and ensure the Bluetooth integration is enabled. After flashing,
allow Home Assistant to discover both names. 

The BTHome v2 Service Data contains:

- UUID `0xFCD2`
- Device information byte `0x44` (unencrypted, trigger-based BTHome v2)
- Packet ID object (`0x00`), persisted in internal flash for each button
- Battery percentage object (`0x01`)
- Button event object (`0x3A`)

Advertisements are **not encrypted**. These events are suitable for ordinary
button push automations, not security-critical actions such as unlocking a door.

## Power behavior

After advertising, the firmware puts the nRF52840 into System OFF, configured
to wake when either button is pressed. System OFF wake-up restarts the
application; it does not resume the previous execution.

Before entering System OFF, the firmware sends the onboard QSPI flash the
`0xB9` Deep Power-Down command using its QSPI-connected pins. It also disables
Bluefruit's automatic BLE status LED. Actual board current depends on the
hardware, battery, measurement setup, and firmware state. A deep-sleep current
of approximately 2.5 µA has been measured on one assembled device; this is not
a guaranteed specification.

## Build and upload

Install PlatformIO Core, connect the XIAO over USB, and run these commands from
the project directory:

```sh
pio run
pio run --target upload
```

The PlatformIO environment is configured in [`platformio.ini`](platformio.ini).
For reference see [Getting Started with Seeed Studio XIAO nRF52840 Series](https://wiki.seeedstudio.com/XIAO_BLE/).

## Implementation

Firmware source: [`src/main.cpp`](src/main.cpp).
