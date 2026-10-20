# Optional POSIX host runtime adapter

This directory contains GPL-2.0-only POSIX IPC and nonce implementations
extracted from `Khronos31/px4-userland` at source commit
`27236f24de9db1217c5d3bf548d5db11a8e70f27`. The original SPDX notices remain
on the source files. `COPYING` preserves the source repository's GPL-2.0 text.

`dtv_userland_host_runtime` is opt-in, build-tree-only, and is not installed or
exported. Its product namespace and instance-name policy are explicit private
target inputs. The common IPC declaration is in this adapter and is consumed
through product compatibility headers; it is not part of the MIT include tree.
