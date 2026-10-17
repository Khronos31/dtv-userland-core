# DTV userland common core

This repository contains a first, portable C++17 increment of shared DTV userland code. It provides the reviewed error/result, logging, mock transport foundation and a bounded UDTV 1.0 wire subset. It is not a daemon, hardware transport, complete wire protocol, or card implementation.

The static libraries are `dtv::core` and `dtv::wire`. Consumers can use the installed CMake package with `find_package(dtv_userland_core CONFIG REQUIRED)`. The wire library links the core library. Both targets compile without exceptions or RTTI and have no operating-system or USB dependency.

Configure with CMake and enable `BUILD_TESTING` to build the standalone foundation and wire tests. The optional `DTV_LEGACY_SOURCE` cache path enables the behavioral equivalence check against a fixed PX4 source checkout. See `PROVENANCE.json` for the source pin, extracted-file hashes, per-file source/destination SPDX identifiers, moved ranges, and exclusions.

The root MIT license applies to this candidate's approved foundation destinations, independently implemented wire subset, and newly added build, packaging, test, and documentation files. It does not relicense the full PX4 product, unlisted source files, third-party material, or any future GPL adapter. This records the approved license scope and does not assert or infer copyright ownership or authorship. The scope follows `design/independent-code-policy.md`, the user's continuation policy recorded at 2026-10-18 02:44 JST, and the first-increment implementation start at 2026-10-18 03:06 JST.

The typed wire payload API covers HELLO requests and ACQUIRE requests/responses, alongside the bounded UDTV 1.0 envelope and frame handling. It does not implement a typed HELLO response or the full wire protocol. Wire limits are described in [docs/wire-subset.md](docs/wire-subset.md). No current product client or daemon is wired to the new wire API.
