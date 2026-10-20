# Optional host runtime adapter

This directory contains GPL-2.0-only IPC and nonce implementations extracted
from `Khronos31/px4-userland`. The POSIX implementation is from source commit
`27236f24de9db1217c5d3bf548d5db11a8e70f27`; the Windows implementation and
security helper are from source commit
`a8b1fc5e109667bf483154d7d20b3f0003b37c78`. Original SPDX notices remain on
the source files. `COPYING` preserves the source repository's GPL-2.0 text.

`dtv_userland_host_runtime` is opt-in, build-tree-only, and is not installed or
exported. CMake selects the Windows or POSIX IPC/nonce source pair for the
target platform. Its product namespace and instance-name policy are explicit
private target inputs. The common IPC declaration and Windows security helper
are in this adapter and are consumed through product compatibility headers or
the build-tree target include path; they are not part of the MIT include tree.
