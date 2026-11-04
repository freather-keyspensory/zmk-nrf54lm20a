# Contributing

Feedback, reproducible bug reports, and focused patches are welcome. This is an experimental Minew nRF54LM20A port, not a drop-in nRF52840 bootloader.

Read `AGENTS.md` and `docs/testing.md`. Explain the exact board/module, wiring, host OS, probe, source revisions, image version, and reproduction steps. Separate build success from actual hardware results. Include sanitized logs only; never attach full RRAM backups, pairing keys, signing keys, or unrelated desktop captures.

Run `bash firmware/bootloader/tests/run.sh` for parser/converter changes and build affected firmware targets. Describe hardware tests that were actually run and tests still outstanding. Do not enable irreversible security settings or erase unrelated devices.

Keep firmware and hardware work separate. Existing local EC/HE experiments are not part of the published firmware baseline. Propose hardware additions separately with their own provenance and validation.
