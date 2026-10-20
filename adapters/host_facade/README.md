# Optional host facade adapter

This GPL-2.0-only adapter owns generic PX4 IPC record conversion, runtime
instance validation through the MIT identity library, signal handling,
Windows stdin decoding, and platform sleep support extracted from
`Khronos31/px4-userland` source commit
`03d4f1ce0c5e39fb864cd845bfd069f891ab5932`. `PROVENANCE.json` lists the
source paths. `COPYING` preserves the source license text.

`dtv_userland_host_facade` is opt-in, build-tree-only, and is not installed or
exported. Its public headers stay outside the MIT include tree. Product
namespace and instance-case policies are supplied by each runtime target.
Device models, USB discovery, serial trust, transport constants, nonce
selection, and hardware backends remain in product adapters.
