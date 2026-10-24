# Optional Windows WinSCard adapter

Files in this directory are licensed under GPL-2.0-only. The WinSCard client
and proxy are derived from PX4's `userland/src/windows/winscard_*` and
`scripts/px4-winscard.def` in `Khronos31/px4-userland`; the source files retain
their SPDX notices. The portable core and its normal MIT targets remain
separately licensed.

The complete GPL version 2 license text is preserved in `COPYING`, copied
verbatim from the PX4 source repository's `LICENSE`.

`dtv_windows_winscard_client` and any DLL created by
`dtv_add_windows_winscard_dll()` are opt-in, build-tree-only targets. They are
not installed or exported. A product supplies its namespace/version, expected
UDTV product slug, reader prefix, runtime environment variable, host facade
includes, and client-only support target; Core never links back to a product
target. There is no implicit PX4 default, so enabling the adapter without those
values fails configuration.
