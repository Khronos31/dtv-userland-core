# Windows stdin EOF monitor

`DTV_USERLAND_BUILD_HOST_FACADE=ON` のWindows targetは、GPL-2.0-onlyの既存P実装を共有する。`<dtv/userland/host_facade/windows_stdin.h>` の `px4::userland::cli::StdinEofMonitor` を使い、`dtv::host_facade` へリンクする。MIT portable targetへの依存追加はない。

`start(input, callback, context)` は借用HANDLEから読込み、EOFまたは読込み失敗でcallbackを一度呼ぶ。callbackは製品側の `request_stop` へ接続する。`stop()` / destructorは読込みをcancelしてjoinする。製品はcontextをcloseした後で `notify_cleanup_complete()` を呼ぶ。CLIや停止意味は製品が既存契約に従って接続する。

`windows_stdin_monitor_tests` は既存Pの4件（起動時EOF、データ後EOF、read失敗、閉じないpipeのcancel/join）を同じ条件で実行する。由来とSHA-256は `adapters/host_facade/windows-stdin-provenance.json` に記録した。PとASICENの旧helperはCore受入後の別小差分で共有header/libraryへ切り替える。このPRは各製品mainを変更しない。
