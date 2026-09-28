# PThreadTicker 0.01 (from UnixLib 5.0.1)

The module UnixLib 5.0.1 programs use for their thread timer when it is
loaded (without it UnixLib runs a copy from the RMA). Every program here is
linked with UnixLib 5.0.1, so each app carries the module and loads it from
its !Run (a copy merged into !System is used first if there is one).

- From `PThreadTicker-0.01.zip`, release v5.0.1 of
  github.com/adyoull/riscos-unixlib (`libunixlib/module/pthticker.s`,
  `docs/THREAD-TICKER.md`).
- `PThrTicker` sha256 checked by `tools/check-versions.sh`:
  see `PThrTicker.sha256`.
- Revised BSD licence: `Licence`. User notes: `ReadMe`.
