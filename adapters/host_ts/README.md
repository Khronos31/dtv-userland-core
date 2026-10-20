# Host TS consumer adapter

This build-tree-only GPL-2.0-only adapter is derived from PX4 userland GPL sources at accepted source revision `3821ba77c56dc188ac8012fd3c9fb59e640464bb`.

The adapter contains the synchronous control client, shared TS parser/runner, and POSIX/Windows output implementations. Product targets provide their own facade headers through `DTV_USERLAND_HOST_TS_FACADE_INCLUDE_DIRS`. This target is optional and is not installed or exported by the MIT core package.
