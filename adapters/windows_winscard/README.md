# Optional Windows WinSCard adapter

Files in this directory are licensed under GPL-2.0-only. The WinSCard client
and proxy are derived from PX4's `userland/src/windows/winscard_*` and
`scripts/px4-winscard.def` in `Khronos31/px4-userland`; the source files retain
their SPDX notices. The portable core and its normal MIT targets remain
separately licensed.

The complete GPL version 2 license text is preserved in `COPYING`, copied
verbatim from the PX4 source repository's `LICENSE`.

`dtv_windows_winscard_client` and `dtv_windows_winscard` are opt-in,
build-tree-only targets. The latter produces the single product-independent
`WinSCard.dll` artifact used by PX4, ASICEN and torne; it is not installed or
exported by Core. The DLL scans the PX4, ASICEN and torne v1 namespaces,
includes the product, instance and reader ID in each local reader name, and uses the shared
`DTV_USERLAND_RUNTIME_DIR` override. Product builds must stage the same Core
DLL bytes and must not link a product-specific support target into the DLL.
