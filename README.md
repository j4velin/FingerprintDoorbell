# FingerprintDoorbell PoE

Fork of [frickelzeugs](https://github.com/frickelzeugs)' awesome [FingerprintDoorbell](https://github.com/frickelzeugs/FingerprintDoorbell) with some modifications so that it runs on an [Olimax ESP32-POE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/open-source-hardware) board.

For more information, take a look at the [original README](https://github.com/frickelzeugs/FingerprintDoorbell/blob/master/README.md).

## Changes

- removed all WiFi related code
- enabled ethernet connection (DHCP)
- disabled breathing LED when idle
- disabled external ringer signal (the pin is used in the ETH connection)
- use GPIO14 as input for the doorbell ring event (for a dedicated button to just ring)
- use GPIO15 as output for a buzzer for an acoustic feedback while the doorbell button is pressed

## Wiring

![Wiring](images/wiring.png)