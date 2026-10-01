# PThreadTicker 0.03 (from UnixLib 5.0.3.1)

The module UnixLib 5.0.1 and later programs use for their thread timer
when it is loaded (without it UnixLib runs a copy from the RMA). Every
program here is linked with UnixLib 5.0.3.1, so each app carries the
module and loads it from its !Run (a copy merged into !System is used
first if there is one).

- From `PThreadTicker-0.03.zip`, release v5.0.3.1 of
  github.com/adyoull/riscos-unixlib, an unofficial fork of GCCSDK's
  UnixLib (`libunixlib/module/pthticker.s`, `docs/THREAD-TICKER.md`).
  0.03 keeps the timer running while a program is in Wimp_Poll, so
  threads run in programs that poll often (interface version 2: programs
  built with 5.0.3.1 use 0.03 only, older ones 0.01/0.02 only; either
  falls back to its own copy).
- The !Run files ask for 0.01 on purpose (as the module's ReadMe says):
  an older copy that's loaded and in use can't be replaced, and a
  program that doesn't find a version it uses runs its own copy.
- `PThrTicker` sha256 checked by `tools/check-versions.sh`:
  see `PThrTicker.sha256`.
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
