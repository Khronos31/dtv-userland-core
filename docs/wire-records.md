# UDTV 1.0 型付きレコード

`dtv::wire::records`（`include/dtv/userland/records.h`、`src/records.cpp`、`dtv_userland_wire` ターゲットの一部）は、エンベロープおよび HELLO/ACQUIRE サブセットに続く型付き UDTV 1.0 レコードのために独自実装された、上限付きコーデックである。エンベロープ、断片化に対応した `FrameReceiver`、および既存の HELLO 要求 / ACQUIRE コーデックを変更せずそのまま再利用する。製品クライアント、デーモン、IPC エンドポイントとは接続していない。

## エンコード規約

| 形式 | バイト数 |
|---|---|
| `u8`/`u16`/`u32`/`u64`/`i32` | 固定幅、リトルエンディアン |
| `bool` | `u8` の 0 または 1 |
| `UUID`/epoch/token | 16 バイトの不透明データ（opaque bytes） |
| `str[N]` | `u16` バイト長 + UTF-8 バイト列（最大 `N`、NUL 埋め込み不可） |
| `bytes[N]` | `u32` 長さ + バイト列（最大 `N`） |
| `arr<T,N>` | `u16` 要素数 + 各要素（最大 `N`） |
| `opt<T>` | `u8` タグ（0: 非存在 / 1: 存在） + 存在時の値 |

`Model` は `str[128] name` としてエンコードされる。wire 上の `id` フィールドは存在しない。STATUS の enclosure および LIST/STATUS の function レコードでは、この値の直前に optional な Model タグが依然として先行する。

独立した LNB 給電操作は opcode `0x0039` を使用する。その要求および応答ボディは、それぞれ 16 バイトの epoch と、それに続く 1 バイトの `bool enabled`（`0`: オフ、`1`: オン）で構成される。これは同一リリース内の内部制御レコードであり、接続ディスパッチ、クライアントヘルパー、デーモンの権限検査、製品の LNB サポートを追加するものではない。

すべてのレコードは明示的な固定サイズ領域（`WString`、`BBytes`、`BArray`、`Optional`）に格納される。動的メモリ確保は行われず、ホストの構造体をそのまま wire へ/wire からコピーすることもない。唯一の例外は `TsData` であり、1 MiB の配列を保持する代わりに入力ビュー内のパケットバイト列をエイリアス（参照）する。

## コーデックのエラー契約

デコーダは厳密なペイロードサイズを要求する。レコード内では次のエラーを返す:

- `PROTOCOL_ERROR`: サイズ、要素数、長さ、optional タグ、UTF-8、配列順序、重複 ID の違反、および最終フィールド以降の余剰バイト。
- `INVALID_ARGUMENT`: サイズは正しいが範囲外の値。未知の enum、予約ビット、仕様上 0 以外が要求される箇所での 0 の ID/lease/generation、宣言集合に含まれない未サポート測定値、または対応する generation のない `usable` のような意味的結合の不整合。
- `RESOURCE_LIMIT`: 正当な形式であるが、エンコード後サイズが `kMaxControlPayload` (65,536) を超えるレコード。1 バイトも書き込まれる前に返され、呼び出し側自身のバッファ不足を意味する `BUFFER_TOO_SMALL` とは区別される。現行の上限規約では、正当なすべてのレコードが制御上限内に収まる（最大構成の STATUS であっても、両方の enclosure 観測 option が存在し各ストリームカウンタ集合に 16 エントリが含まれる状態で 58,780 バイト）。このガードは将来のレイアウト変更に備えて保持されている。
- エンコーダは、いかなる不正な値に対しても `INVALID_ARGUMENT` を返し、出力バッファ不足には `BUFFER_TOO_SMALL` を返す。エンコーダは最初にカウントパスを実行してレコード全体の長さを測るため、出力バッファ不足時に呼び出し側バッファを変更することはなく、レコードが途中で切断されることもない。

## STATUS の enclosure 観測値

同一リリース内において、`StatusResponse` は `opt(enclosure)`、それに続く `opt<bool> lnb_15v_enabled` および `opt<bool> card_inserted` をこの順序でエンコードしてから `topology` をエンコードする。各 optional boolean は、1 バイトの存在タグ（0: 非存在、1: 存在）と、存在時の 1 バイトの boolean（0: false、1: true）で構成される。非存在の値は、未知（unknown）、利用不可（unavailable）、または未観測（unobserved）を意味し、観測された `false` とは区別される。`card_inserted` は筐体レベルの内部カード観測値を表し、リーダー状態を集約して導出されたものではない。外部リーダーの観測値は各リーダーレコード内に保持される。

エンベロープバージョンは UDTV 1.0 である。このレコードレイアウトは、同一リリースに属するデーモンとクライアントコンポーネント間でのみサポートされる。本リビジョンにおける `Model` レコードは name のみであり、そのバイト列は、`id` と `name` の双方をエンコードしていた Core `af82b7a` 時点の初期開発系列とは異なる。これは完全なレコードレイアウトがその開発系列と一致することを主張するものではない。2 つの enclosure 観測 option を持たなかった旧 main レイアウトを含め、異なるリリース間の STATUS レイアウトはサポート外である。デコーダは厳密なサイズ検査および厳格な optional タグ検査を維持する。

## ストリームカウンタ

`StreamCounters` は昇順に並んだ 2〜16 個のエントリを運ぶ。ID 5 (`transmitted_packets`) および 6 (`queue_dropped_packets`) は必須であり、ID 7〜32 は任意（optional）である。ID 0 および ID 33〜255 は拒否される。ID 7〜12 は `stream_bytes`、`sync_errors`、`tei_packets`、`continuity_errors`、`stream_usb_errors`、`empty_intervals` であり、ID 13〜32 は将来の任意カウンタ用に予約されている。各エントリは既存の `Counter` 測定状態を使用する。`valid` は測定された 0 を含み、`unknown` はその理由を保持し、任意の `unsupported` エントリは JSON および CLI 表示から除外される。未知の任意エントリは完全に検証され、投影処理によってスキップされる。既知のフィールド名をエイリアスすることはない。フレームエンベロープはバージョン 1.0 のままである。異なるリリースのピア間通信は未サポートのままであり、既存の厳密なサイズ検査および意味的検証検査は変更されない。

## 検証の分離（Validation split）

バイトコーデックは、レコードに局所的な内容と上限付きコンテナのみを検査する。参照やコンテキストを検査する独立したバリデータ群が存在し、スナップショットの公開や操作のディスパッチの前に必ず実行されなければならない:

- `validate_error_record`、`validate_scope`
- `validate_capabilities`、`validate_receiver_descriptor`、`validate_reader_descriptor`、`validate_topology`、`validate_limits`
- `validate_measurements`（測定項目 ID 集合は宣言された `measurement_mask` と一致しなければならない。宣言された各測定項目は必ず存在し、`unknown` または `valid` でなければならず、`unsupported` であってはならない）
- `validate_stream_counters`（必須 ID 5 および 6、任意 ID 7〜32、最大 16 個の昇順エントリ）、`validate_receiver_counters`（ID 1〜4 のサブセット）。これらには `overflow=true` の場合に `value == UINT64_MAX` を要求する飽和規則が含まれる
- `validate_stream_final`（理由とエラーの結合）、`validate_receiver_status`、`validate_reader_status`、`validate_card_observation`、`validate_card_session_result`、`validate_card_status_response`
- `validate_binding`（`function_id` は同一スナップショット内に存在しなければならない）
- `validate_list_response`
- `validate_status_response_scope`（形状のみ）および `validate_status_response`（集約検査）

`validate_status_response(value, StatusContext{list})` は、スナップショットの公開または操作のディスパッチ前に呼び出し側が実行する完全な検証パスである。コンテキストは、正規化・検証済みの同一 epoch の `ListResponse` である。これは scope が属する epoch と、公開された完全な function/receiver/reader 記述子を提供する。すべてのバリデータは、まず値に対してコーデック局所の厳密な検査を実行するため、型付きインターフェースを直接呼び出す側が `static_cast` を使って enum/state/reason/reserved/cap 規則を迂回することはできない。集約検査では次に以下を検証する:

- 応答コンテキストの epoch が正規 list の epoch と等しいこと。
- 重複 ID および所属関係: 応答に含まれるすべての function/receiver/reader ID が正規 list に存在し、receiver/reader 記述子の `systems`、`binding`、`capabilities` が正規記述子と一致すること。
- グローバル（`enclosure`）スコープの場合、応答が同一 epoch の完全な function、receiver、reader 集合（要素数および ID が正規 list と等しい）を返し、コレクションの存在有無が正規 topology と一致すること。既知の正規 topology では両方のコレクションが存在することを要求し、真に未知の正規 topology では両方が省略されることを要求する。既知の空 reader 集合は要素数 0 の存在する配列とする。
- 部分スコープの場合、返される単一の receiver/reader が正規メンバーであり記述子が一致すること。参照は依然として正規 function 集合に対して解決される。
- 正規の公開レシーバ数に対する `Limits.max_concurrent_streams` の照合、および `snapshot.observed_at_ns` に対するすべての有効な測定値/カウンタ/カードサンプリング時刻の照合。
- 保持された（前回の）ストリームターゲットでは、レシーバが依然として `leased` 状態であることを要求する。

正規コンテキストが存在しない場合（またはその epoch が一致しない場合）、関数は `INVALID_ARGUMENT` を返す。部分レコード自身の主張が検証済み LIST の代わりになることは決してない。`validate_status_response_scope` は形状のみの事前検査として維持される。

## 本インクリメントに含まれないもの

- ディスパッチャおよび接続状態: 初回 HELLO、交渉済みバージョン、再利用または過去の request ID、不足している必須機能に対する `UNSUPPORTED`、STATE_EVENT 購読、要求と応答の突き合わせ。
- フレームおよび HELLO の 5 秒期限、ソケット、タイムアウト、デーモンやハードウェアとの統合。
- PID フィルタ機能のゲーティング、周波数/tsid/slot のプロファイル範囲検査、JSON エクスポータおよび CLI 変換。
- 製品エンドポイント、CLI、JSON 切り替え、バージョン引き上げやリリース公開。

本節は `dtv::wire` コンポーネント単体について述べている。`DTV_USERLAND_BUILD_CONTROL=ON` の場合、分離された内部コンポーネント `dtv::control` が [`control-connection.md`](control-connection.md) に記載されたソケット不要な接続ゲートと応答ヘルパーを提供する。サービス実行や製品統合はそのコンポーネントの対象外である。

## 確定事項として扱う規則

本リビジョンに先行するレビューで、設計文書によって決定されていると指摘された複数の事項が存在する。これらは曖昧さとしてではなく、確定した規則として実装されている:

- Capabilities は、`controls` ビットの直下に optional な存在タグを**伴わずに** LNA/LNB ボディを直接保持する。したがって最小 capabilities は 4 バイトのゼロである。
- 発行される lease/stream/card handle/generation ID は 0 以外（nonzero）である。ストリームスコープのターゲットも 0 以外である。存在する `Error` は 0 以外のコードを持つ。
- `overflow=true` は `value == UINT64_MAX` を要求する。飽和カウンタがラップ（周回）することはない。
- `quiesced=false` を伴う STOP 完了は、必ず `final.error` を保持しなければならない。
- TRANSMIT コマンドは 1〜4096 バイトである。
- ISDB-T では TSID または slot を選択できない（`selection` は `automatic` のまま）。
- 非存在の `UsbFunction.model` は、`none` 以外の識別理由（identification reason）を要求する。既知のコールドモデルは `cold_model` を保持してよい。
- LNA が提供される場合、そのデフォルト値は記述子のサポートシステムと厳密に一致しなければならない（サポートされるシステムごとに 1 エントリ。不足、過剰、重複は不可）。
- `frequency_hz` および存在する `programmed_hz` は正数かつ最大 2^53−1 である。プロファイル範囲はコーデックの外部で検査される。
- STATUS の `enclosure.identity` がスキャンの一意性（uniqueness）を主張することは決してない。シリアル状態が `present` または `unknown` の場合、uniqueness は `unknown` となり（unknown 状態は不在の確定ではない）、不在（`absent`）が確認された場合のみ `not_applicable` となる。`unique_in_scan` および `duplicate` は LIST/discovery の値として残る。
- 既知の topology は `receivers` と `readers` の両方を返す。既知の空 reader 集合は要素数 0 の存在する配列であり、非存在ではない。未知の topology は両方を省略する。
- TUNE の `timeout_ms` およびその他の TUNE optional 項目は設計によって明示的に定義されている。「要求されないタイムアウトオプションは存在しない」という記述はカード固有のものである。
- 詳細な STOP/カード/部分スコープのレコードは、基本的なドラフトと矛盾するものではなく、それを精緻化したものである。

## 残された未決事項

既存文書から導出できない項目のみが残されている:

1. **ゼロパケットの TS_DATA**: レイアウトでは最大パケットバイト数と 188 アラインメントを定めているが、0 パケットの TS_DATA イベントが正当であるかは規定していない。コーデックはこれを受理する。
2. **PID 配列の順序**: ドラフトは重複がないことを要求しているが、昇順であることは要求していない。コーデックはビットマップにより重複を拒否し、送信された順序を保持する。
3. **ピアが通知する HELLO 上限**: `max_control_payload` および `max_ts_bytes` はピア側の仕様を表し、そのまま運搬される。ローカル上限との相互照合は行わない。

## 来歴（Provenance）

要件は、`adoption-status.md` の条件付き採用に基づき、`shared/design/common-wire-draft.md`、`wire-record-layout.md`、`common-api-contract.md`、`json-record-contract.md`、`cli-contract-draft.md` から得られている。コードは独自の MIT 実装であり（`independent-code-policy.md`）、製品のペイロード、カードプロトコル、ハードウェア手順、ベンダーのアセットをコピーしていない。
