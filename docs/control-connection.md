# UDTV 制御プロトコルゲート

`dtv::control` は、既存の `dtv::wire` レコードに対する制御接続ごとのプロトコル検査を必要とする呼び出し側のための、内部 Core コンポーネントである。`DTV_USERLAND_BUILD_CONTROL=ON` でビルドされ、`find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS control)` で検出される。`dtv::wire` のみにリンクし、Threads を必要としない。

本インクリメントは、`ControlConnection`、要求/応答ヘルパー値、および上限付きフレームエンコードヘルパーを提供する。呼び出し側が完全なデコード済みフレームと出力バッファを提供する。このゲートは、初回フレームとして HELLO を要求し、サポートされるバージョン/機能を交渉し、単調増加する要求 ID と 1 つの処理中要求を強制し、デーモンの epoch を検査し、サポートされる制御要求を所有権を持つ型付きの値へとデコードする。`dispatch` は要求が呼び出し側で処理可能になったことのみを意味し、ACQUIRE、TUNE、START、STOP、RELEASE を実行するものではなく、それらの操作が成功したことを示すものでもない。`complete()` は、呼び出し側が応答の書き込みを完了した後に、未完了要求のゲートを解放する。

TUNE について、本ゲートは要求のデコードと分類のみを行う。呼び出し側がレシーバの LNB/LNA 機能を検査し、対応するサポートポリシーを適用する。

CARD_STATUS、CARD_CONNECT、CARD_RECONNECT、CARD_DISCONNECT、CARD_RESET、CARD_TRANSMIT、BEGIN_TRANSACTION、END_TRANSACTION、CARD_HANDLE_STATUS、および LNB_POWER は、対応する型付き要求メンバーへとデコードされる。要求値は呼び出し側所有の `ControlRequest` 領域にコピーされ、入力フレームへの借用ビュー（borrowed view）は保持されない。ただし呼び出し側は、`Frame.payload` をフレーム格納領域によって生存期間が制限された借用入力として扱う必要がある。この領域は `accept()` から復帰した後に再利用されうる。CARD 要求は交渉済みの CARD_SERVICE を要求し、そうでなければ UNSUPPORTED を受け取る。LNB_POWER は CARD_SERVICE から独立している。ディスパッチ結果はプロトコルゲートを通過したことのみを示し、リーダー、レシーバ、物理的な所有権の確立、デーモン権限やハードウェア機能の検査、操作の実行、またはその成功を意味するものではない。

本コンポーネントは、ソケット、リスナー、スレッド、タイマー、クライアント、サービス、レシーバ、キャプチャ格納域、USB ハンドル、製品アダプタを一切所有しない。ATTACH および TS_DATA は分離されたデータパスに属し、ここでは扱われない。本ゲートの制御サブセット外にある正当な形式の opcode は、既存の wire レコードに従って `UNSUPPORTED` を受け取る。

デーモンと ctl 間の UDTV は内部契約である。異なるリリースに属するデーモンと ctl の接続はサポートされない。この CMake コンポーネントのエクスポートは、安定した公開 wire やクロスリリースの API を確立するものではない。name のみの Model、および topology の直前に配置される STATUS の `lnb_15v_enabled` / `card_inserted` optional フィールドを含む現行のレコードレイアウトについては、`docs/wire-records.md` を参照のこと。
