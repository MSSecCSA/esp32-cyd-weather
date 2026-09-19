# Contributing

Issues and forks are both very welcome. This started as a personal project and
became something with findings worth sharing, so if it is useful to you, take it
and run.

## Fork freely

Cheap Yellow Displays vary between production batches, and your needs are almost
certainly not mine. Fork it, rip out the weather, put your own cities in, drive
different hardware. The Apache 2.0 licence asks that you retain the `NOTICE` file
and note significant changes — beyond that, do what you like.

If your fork does something interesting, open an issue and say so. I would like
to link to it.

## Issues are useful even when they are not bugs

Genuinely useful things to open an issue about:

- **Your board behaves differently.** CYD revisions differ in real ways — the
  amplifier part number, whether the LDR divider is usable, which USB connectors
  are fitted. A report like "my board does X where the README says Y" is valuable
  data, not noise.
- **A documented fact is wrong.** Every hardware claim in `CLAUDE.md` is labelled
  VERIFIED, ASSUMPTION, or UNVERIFIED. If you can disprove one, please do.
- **A question about why something is the way it is.** If the reasoning is not
  written down clearly enough, that is a documentation bug.
- **Bugs**, obviously. Serial output helps enormously.

## The one convention worth knowing

This project tries hard not to state hardware facts it has not checked. Claims are
labelled by how they are actually known:

| Label | Meaning |
|---|---|
| **VERIFIED** | Measured on a physical board, with the method recorded |
| **ASSUMPTION** | Configured and working, or corroborated by sources, but not independently probed |
| **UNVERIFIED** | Believed, not tested |

That distinction earned its place. Several defects here compiled cleanly, ran, and
were only caught by putting an instrument on the board — including a touch
controller that had never worked once while appearing merely unreliable. If you
contribute a hardware claim, please say how you know it.

`tools/hwtest/` is a serial-driven rig for exactly this. If you are testing
something on your own board, it is probably the fastest way in.

## Practical notes

- Build with `pio run`; flash with `pio run --target upload`.
- There is no test suite — this is single-target firmware, verified by flashing it
  and reading serial output.
- Dependencies are pinned exactly, deliberately. Please do not loosen them to
  caret ranges.
- Keep credentials out of the repository. They live in NVS at runtime and there is
  nothing compiled in; please keep it that way.
