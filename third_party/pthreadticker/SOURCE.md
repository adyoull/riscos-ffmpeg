# PThreadTicker 0.02 (from UnixLib 5.0.3.1-rc8)

The module UnixLib 5.0.1 and later programs use for their thread timer
when it is loaded (without it UnixLib runs a copy from the RMA). Every
program here is linked with UnixLib 5.0.3.1-rc8, so each app carries the
module and loads it from its !Run (a copy merged into !System is used
first if there is one).

- From `PThreadTicker-0.02.zip`, pre-release v5.0.3.1-rc8 of
  github.com/adyoull/riscos-unixlib (`libunixlib/module/pthticker.s`,
  `docs/THREAD-TICKER.md`). 0.02 updates its count of programs with
  interrupts off.
- The !Run files ask for 0.01 on purpose (as the module's ReadMe says):
  an older copy that's loaded and in use can't be replaced, and either
  version works with every UnixLib that uses it.
- `PThrTicker` sha256 checked by `tools/check-versions.sh`:
  see `PThrTicker.sha256`.
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
