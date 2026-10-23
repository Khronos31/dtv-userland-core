# Host TS client adapter

This GPL-2.0-only optional target provides the shared wire TS client, channel parser, and POSIX/Windows output adapters. It uses the current wire control client and protected endpoint connector. It has no listener or daemon `Context`; product listener integration is a later target.

The source is adapted from the PX4 userland tree at `90b08dfe79b0b40e393f015975c761863830866a` and the prior Core client implementation at `af82b7a9329bfe2202e07c960f368469612e28f4`. The legacy internal-IPC `PosixControlClient` path is excluded. Enable with `DTV_USERLAND_BUILD_HOST_TS=ON`; installation exports `dtv::host_ts` and GPL license/provenance material.
