# Optional host control frontend

Files in this directory are licensed under GPL-2.0-only. The frontend is
derived from PX4's `userland/tools/px4ctl.cpp` in
`Khronos31/px4-userland`; the source files retain their SPDX notices. The
portable core and its normal MIT targets remain separately licensed.

The complete GPL version 2 license text is preserved in `COPYING`, copied
verbatim from the PX4 source repository's `LICENSE`.

`dtv_userland_host_ctl` is an opt-in, build-tree-only static target. It is not
installed or exported. Its host facade include directories are private target
inputs, while the final product executable supplies the host socket symbols
through its existing platform library.
