<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# L3 owner prototype

This isolated target exercises a candidate owner/provider lifetime contract. It
is not part of the top-level Core build by default and does not implement a
release API. The header's symbols, enum values, and layouts are candidates, not
a stable ABI.

## Product integration candidate

New integrations use two structures instead of the legacy combined
`dtv_l3_provider`:

- `dtv_l3_usb_provider`: host-owned USB transport only (claim, release_claim,
  submit, cancel, claim_cancel, cancel_scope, logger).
- `dtv_l3_product_adapter`: product-owned semantics (capabilities, firmware,
  scope, tune, bridge, stream). CARD/LNB remain UNSUPPORTED in this candidate.

`dtv_l3_open_ex` accepts both. `dtv_l3_open_factory` runs a product factory with
`dtv_l3_open_config` (model, opaque selector, firmware PATH/BYTES/read callback)
and calls the factory release once after the owner is destroyed. Capabilities
stay in one common shape and missing adapter callbacks report
`DTV_L3_UNSUPPORTED`. CARD/LNB stay UNSUPPORTED until the separate CARD/LNB
contract work. The legacy `dtv_l3_open` path remains for the existing
fixtures and forwards the combined provider to the same engine.

The host provider has no tune/stream/firmware policy. Product adapters and the
firmware view they retain must stay alive until `dtv_l3_context_release`
succeeds. Core calls adapter callbacks outside its large lock.

The newer integration candidate (`dtv_l3_open_with_args` + `dtv_l3_factory`)
adds per-receiver-system/reader/LNB/shared-scope capability values with
SUPPORTED/UNSUPPORTED/UNKNOWN, host grant descriptors, a firmware manifest, and
factory-chosen `claim_count`. The factory must validate the manifest before any
claim side effect. `dtv_l3_capabilities_get_catalog` returns the borrowed
catalog. The older `dtv_l3_open_factory` remains as the initial candidate.

B2 before-close cancellation runs on the requesting thread; the control lane
only expires operation deadlines before and after each queued item. A
MAY_BLOCK submit provider rejects `resubmit_on_provider_abort` before any side
effect.

The prototype source, header, tests, build definition, and this README are
licensed under GPL-2.0-or-later. The repository root `LICENSE` remains MIT for
the existing Core components. This target does not link or copy those
components. Its dependency closure is the C standard headers `stddef.h` and
`stdint.h`, the C++ standard library, and the platform thread runtime requested
by CMake `Threads`; it has no USB, device, product, or external service
dependency.

Run the standalone checks with:

```sh
cmake -S prototype/l3_owner -B /tmp/l3-owner-normal -DBUILD_TESTING=ON
cmake --build /tmp/l3-owner-normal -j2
ctest --test-dir /tmp/l3-owner-normal --output-on-failure

cmake -S prototype/l3_owner -B /tmp/l3-owner-tsan \
  -DBUILD_TESTING=ON -DL3_OWNER_ENABLE_TSAN=ON
cmake --build /tmp/l3-owner-tsan -j2
ctest --test-dir /tmp/l3-owner-tsan --output-on-failure
```
