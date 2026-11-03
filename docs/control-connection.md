# UDTV 制御プロトコルゲート

`dtv::control` は、既存の `dtv::wire` レコードに対する制御接続ごとのプロトコル検査を提供する、Core の内部ライブラリコンポーネントである。CMake オプション `DTV_USERLAND_BUILD_CONTROL=ON` でビルドされ、`find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS control)` により検出・利用される。`dtv::wire` のみにリンクし、OS スレッド（Threads）への依存関係を持たない。

本インクリメントは、`ControlConnection`、各種要求/応答ヘルパー値、およびサイズ上限付きフレームエンコードヘルパーを提供する。呼び出し側が完全なデコード済みフレームおよび出力バッファを供給する。本ゲートは、初回フレームとして HELLO を要求し、サポートされるバージョンおよび機能（features）を交渉し、単調増加する要求 ID と 1 接続あたり 1 同時要求の制約を強制し、デーモンの epoch を照合した上で、サポート対象の制御要求を所有権を持つ型付きデータ構造へデコードする。`dispatch` は要求が呼び出し側で処理可能な状態になったことのみを示し、ACQUIRE、TUNE、START、STOP、RELEASE などのハードウェア操作を自ら実行するものではなく、それらの操作が成功したことを意味するものでもない。`complete()` は、呼び出し側が応答フレームの書き込みを完了した後に、未完了要求ゲートを解放するために呼び出す。

TUNE 要求について、本ゲートは要求電文のデコードおよび構文分類のみを担当する。レシーバハードウェアの LNB/LNA 能力を照合し、対応するサポートポリシーを適用するのは呼び出し側の責務である。

CARD_STATUS、CARD_CONNECT、CARD_RECONNECT、CARD_DISCONNECT、CARD_RESET、CARD_TRANSMIT、BEGIN_TRANSACTION、END_TRANSACTION、CARD_HANDLE_STATUS、および LNB_POWER は、対応する型付き要求メンバーへとデコードされる。要求データは呼び出し側が所有する `ControlRequest` 記憶域へ値としてコピーされ、入力フレームへの借用ビュー（borrowed view）は保持されない。ただし呼び出し側は、`Frame.payload` をフレームバッファ領域によって生存期間が制約された借用入力として扱う必要がある。このバッファ領域は `accept()` から復帰した後に再利用されうる。CARD 関連の要求はネゴシエーション済みの CARD_SERVICE 機能を要求し、未交渉の場合は `UNSUPPORTED` が返される。LNB_POWER は CARD_SERVICE の有無とは独立して処理される。ディスパッチの成功はプロトコルゲートの通過のみを意味し、リーダー、レシーバ、物理ハードウェアの所有権の確立、デーモン権限や実機能力の検証、実際の操作実行、またはその成功を意味するものではない。

本コンポーネントは、ソケット、リスナー、スレッド、タイマー、クライアント、サービス、レシーバ、キャプチャバッファ、USB ハンドル、製品アダプターを一切所有しない。ATTACH および TS_DATA は分離されたデータ接続に属し、本制御ゲートでは扱わない。本ゲートの制御サブセット外にある正当な形式の opcode は、既存の wire レコード仕様に従って `UNSUPPORTED` を受け取る。

UDTV wire プロトコルは、採用済みの層モデルにおいて外部契約（L2 規範）として位置付けられている。本コンポーネントはその wire 規範に適合した接続ゲートを提供する。ただし、本 CMake コンポーネントのエクスポートはリリースをまたぐ無期限の安定 C++ ABI を保証するものではなく、同一メジャー内での具体的な互換範囲は上位の外部契約規定に従う。名前のみの Model、および topology の直前に配置される STATUS の `lnb_15v_enabled` / `card_inserted` 各観測フィールドを含む現行のレコードレイアウトについては、[`wire-records.md`](wire-records.md) を参照のこと。
