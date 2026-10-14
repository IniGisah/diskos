# agents/

Contributor instructions for working on diskOS.

`AGENTS.md` in this folder is a ready-made project brief: the hardware facts that are easy to
get wrong, the repo layout, the device-safety rules, and the "verify, do not guess" rule.

## How to use it

- Read [AGENTS.md](AGENTS.md) before changing device or installer code.
- Use it as the project brief for a contributor starting work.

The brief covers the project rules: check `docs/HARDWARE.md` before
claiming anything about the hardware, never assert device behavior from memory, keep the
fail-closed boot contract intact, and follow [RECOVERY.txt](../RECOVERY.txt) after an interrupted flash.

## Good first tasks

- Read `docs/HARDWARE.md` and the installer's `README.md`, then trace the
  install flow in the source.
- Add explicit command mappings and installer checks for a new stock firmware version.
- Improve error messages / add error codes for failure modes not yet covered.
