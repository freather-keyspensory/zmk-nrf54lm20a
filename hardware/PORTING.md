# Firmware-facing hardware notes

The tested firmware target is the Minew MX54BE13/ME54BS13 nRF54LM20A development board. Do not infer XIAO header compatibility from SoC compatibility. Verify power, native USB data routing, clock components, SWD, and reset behavior against the exact module/board schematic.

The firmware baseline intentionally uses a debugger-controlled one-key scan path. No physical matrix, LED or unlock-button pin has been guessed. A future board can supply MX, EC or HE scan hardware through a separate board/shield configuration without replacing ZMK's keymap/behavior architecture.

The original Minew USB-C connector was confirmed by the owner to route data to the module, and actual USB High Speed communication was observed. This does not establish identical routing on other Minew board revisions.

Separate local PCB design work is not part of this published firmware baseline and is not certified for fabrication here. Submit hardware designs separately with their own schematic, design-rule checks, BOM, and assembly/test evidence.
