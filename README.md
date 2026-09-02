# Heltec HTCC-AB02A LoRa mailbox sensor

Battery-powered mailbox sensor firmware for the [Heltec CubeCell 1/2AA Node
(HTCC-AB02A)](https://docs.heltec.org/en/node/asr650x/htcc_ab02a/index.html).
Two reed-switch inputs report the hatch and door state in a compact raw LoRa
packet. Between events, the board enters its low-power mode.

This is point-to-point LoRa firmware, not LoRaWAN. It does not join a network,
use gateways, or contain LoRaWAN credentials. A receiver must use matching
radio parameters and decode the five-byte application protocol described
below.

## Hardware

- Heltec CubeCell 1/2AA Node (HTCC-AB02A)
- Suitable antenna for the configured 868 MHz band
- Two dry-contact reed switches
- 1/2 AA Li-SOCl2 cell, matching the firmware's battery model

The sketch uses the board's internal pull-ups:

| Function | Pin | Active state |
| --- | --- | --- |
| Mailbox hatch | `GPIO1` | `LOW` means open |
| Mailbox door | `GPIO2` | `LOW` means open |
| Test button | `USER_KEY`, or `GPIO0` as a fallback | Falling edge |

Connect each reed-switch input to ground through its switch. Confirm the
switch's contact behavior before installation: the firmware reports `HIGH` as
closed and `LOW` as open. If the mechanical arrangement has the opposite
polarity, invert the state conversion in the sketch.

Do not power the radio without an antenna attached.

## Behavior

At startup, the firmware initializes the inputs and radio without sending an
initial status. A change on either reed switch wakes the MCU and sets an
interrupt flag. The main loop then waits 50 ms for the contact to settle,
checks a shared 300 ms debounce interval, and transmits the current state of
both switches.

Pressing the board's user button sends two test packets one second apart:
first both inputs open, then both inputs closed. Both packets reuse the same
battery reading.

After event handling, `lowPowerHandler()` returns the board to low-power mode.
Serial diagnostics are currently enabled by `ENABLE_SERIAL_DEBUG`; comment out
that definition for a production build with lower power consumption.

## Radio configuration

The transmitter is configured directly through the CubeCell `Radio` API:

| Parameter | Value |
| --- | --- |
| Frequency | 868,000,000 Hz |
| Transmit power | 22 dBm |
| Bandwidth | 125 kHz (index `0`) |
| Spreading factor | SF8 |
| Coding rate | 4/8 (index `4`) |
| Preamble | 8 symbols |
| Header | Explicit/variable length |
| CRC | Enabled |
| IQ inversion | Disabled |
| Transmit timeout | 3,000 ms |

The sketch does not explicitly select a public or private LoRa sync word, so a
receiver must match the default used by the installed CubeCell core. It must
also match every parameter above.

The 868 MHz frequency is intended for the local deployment in Italy, but
frequency, output-power, antenna-gain, and duty-cycle rules remain the
operator's responsibility. In particular, do not assume that the configured
22 dBm is permitted in every 868 MHz sub-band or installation.

## Packet format

Every transmission is exactly five bytes:

| Offset | Field | Encoding |
| ---: | --- | --- |
| 0 | Node ID | `0x01` |
| 1 | Message type | `0x01` (mailbox status) |
| 2 | Hatch | `0x00` closed, `0x01` open |
| 3 | Door | `0x00` closed, `0x01` open |
| 4 | Battery | Estimated charge, clamped to 0-100 percent |

For example, `01 01 01 00 57` represents node 1, a mailbox-status message,
an open hatch, a closed door, and an estimated battery level of 87 percent.

The battery estimate discards the first ADC sample, averages the next two,
converts the result to a nominal 0-3.6 V range, and maps 3.0-3.6 V linearly to
0-100 percent. This is an approximate Li-SOCl2 model, not a fuel-gauge
measurement.

## Build and upload

1. Install the Arduino IDE and Heltec's
   [CubeCell Arduino core](https://github.com/HelTecAutomation/CubeCell-Arduino).
   Heltec documents installation through the Arduino Boards Manager in the
   core's installation instructions.
2. Open `heltec_HTCC-AB02A-mailbox.ino` in the Arduino IDE.
3. Select the `CubeCell-1/2AA Node` board and the device's serial port.
4. Review the pin assignments, radio frequency, and transmit power for the
   hardware and jurisdiction.
5. Build and upload the sketch.

### Current source caveat

The current event path calls `sendMailboxPacket(hatch_open, door_open)` with
two arguments, while the function definition requires a third
`batteryPercent` argument. The comment beside that argument shows an intended
default value of `0xFF`, but a comment is not a C++ default argument. This is a
pre-existing compile blocker and must be corrected (for example, with a
visible default argument or a two-argument overload) before the sketch will
build. It is documented here rather than silently changing firmware behavior
as part of this documentation-and-licensing update.

## Operational limitations

- Packets are unacknowledged; there is no retry or periodic state heartbeat.
- Rapid transitions can be coalesced while the blocking transmit delay runs.
- Node ID and message type are fixed constants.
- Receiver compatibility depends on the CubeCell core's implicit sync-word
  default.
- Raw LoRa provides no application authentication, encryption, or replay
  protection. Anyone with compatible radio settings may receive or forge a
  packet; do not use this protocol for access control or other safety-critical
  decisions.
- Debug logging is enabled in the checked-in configuration.

## License

Copyright 2026 Davide Alberani.

Licensed under the Apache License, Version 2.0. See [LICENSE.txt](LICENSE.txt).
