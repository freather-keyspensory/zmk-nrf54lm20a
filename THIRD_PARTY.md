# Upstream projects

Repository-authored code is MIT licensed unless a file states otherwise. Existing upstream notices and licenses remain in force; the root MIT license does not relicense third-party code.

- ZMK v0.3.0, `edf5c0814fd3ea202e43aad2d68fd32e882a518c`: MIT. Source is fetched from https://github.com/zmkfirmware/zmk. Patches retain upstream ownership/notices.
- Zephyr, `0534e997eacb94bae1df6c9a7dd4a9a6cbf1cecf`: principally Apache-2.0, with file-specific licenses. https://github.com/zephyrproject-rtos/zephyr
- MCUboot, `725d63c9d5bc1bb572d802e7a768daf504186daf`: Apache-2.0 and third-party components with their own licenses. https://github.com/mcu-tools/mcuboot
- Additional dependencies are pinned by the west manifests. Consult each fetched project's license.
- The USB prototype reuses Zephyr's sample USB initialization helper at build time; it is not copied or relicensed here.
- UF2 format reference: https://github.com/microsoft/uf2

Upstream source checkouts, SDKs, compiled firmware, and signing keys are not vendored in this source repository.
