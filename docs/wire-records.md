# UDTV 1.0 型付きレコード

`dtv::wire::records`（`include/dtv/userland/records.h`、`src/records.cpp`、`dtv_userland_wire` ターゲットの一部）は、エンベロープおよび HELLO/ACQUIRE サブセットに続く型付き UDTV 1.0 レコードのために独自実装された、サイズ上限付きのコーデックライブラリである。エンベロープ形式、断片化対応の `FrameReceiver`、および既存の HELLO 要求 / ACQUIRE コーデックを変更せずそのまま再利用する。製品クライアント、デーモン、IPC エンドポイントとは接続していない。

## エンコード規約

| 形式表記 | バイト配置と規則 |
|---|---|
| `u8`/`u16`/`u32`/`u64`/`i32` | 固定長バイナリ、リトルエンディアン配置 |
| `bool` | 1 バイト（`u8`）、0 が false、1 が true |
| `UUID`/epoch/token | 16 バイトの不透明バイナリデータ（opaque bytes） |
| `str[N]` | 2 バイトの長さ（`u16`） + UTF-8 バイト列（最大 `N` バイト、途中に NUL 文字を含めない） |
| `bytes[N]` | 4 バイトの長さ（`u32`） + バイナリデータ列（最大 `N` バイト） |
| `arr<T,N>` | 2 バイトの要素数（`u16`） + 各要素の連続（最大 `N` 個） |
| `opt<T>` | 1 バイトの存在タグ（`u8`、0: 非存在 / 1: 存在） + 存在時のみ後続する値データ |

`Model` レコードは `str[128] name` としてエンコードされる。wire 電文上にモデル `id` フィールドは存在しない。STATUS の enclosure レコードおよび LIST/STATUS の function レコードでは、この文字列値の直前に optional な Model タグが配置される。

独立した LNB 給電操作には opcode `0x0039` を使用する。その要求および応答ボディは、それぞれ 16 バイトの epoch と、それに続く 1 バイトの `bool enabled`（`0`: 給電オフ、`1`: 給電オン）で構成される。これは同一リリース内での内部制御レコードであり、本コーデック自体が接続ディスパッチ、クライアント向けヘルパー、デーモン権限検査、製品ハードウェアの LNB 制御を追加するものではない。

すべてのレコード構造体は明示的な固定サイズ記憶域（`WString`、`BBytes`、`BArray`、`Optional`）に格納される。ヒープの動的割り当ては一切行われず、ホストの構造体をそのまま通信路へコピーすることもない。唯一の例外は `TsData` であり、1 MiB の内部配列を保持する代わりに、入力ビュー内のパケットバイト列を直接参照（借用）する。

## コーデックのエラー契約

デコーダは各レコードで規定された過不足のない正確なペイロード長を要求する。レコード解析処理では次のエラーを返す:

- `PROTOCOL_ERROR`: データ長、要素数、文字列長、存在フラグタグ、UTF-8 妥当性、配列の整序性、重複 ID の各規則違反、および最終フィールド以降に残存する余剰バイト。
- `INVALID_ARGUMENT`: データ長自体は合致しているが、値が許容範囲外である場合。未知の列挙値、予約ビットの指定、仕様上 0 以外が要求されるフィールドでの 0 値（ID/lease/generation 等）、宣言された測定項目集合に含まれない未サポート測定値、または対応する generation を欠いた `usable` 状態のような意味的結合の破綻。
- `RESOURCE_LIMIT`: 正当な形式のレコードであるが、エンコード後の電文サイズが制御ペイロード上限値 `kMaxControlPayload`（65,536 バイト）を超過するレコード。1 バイトも書き出される前に検出されて返される。これは呼び出し側が渡したバッファ不足を表す `BUFFER_TOO_SMALL` とは明確に区別される。現行の容量規定では、正当な全レコードが制御上限内に収まるよう設計されている（最大構成の STATUS レコードであっても、両方の筐体観測 optional が存在し各ストリームカウンタ集合に 16 エントリが含まれる状態で 58,780 バイト）。本ガードは将来の電文拡張に対する安全境界として保持されている。
- エンコーダは、不正な入力値に対しては `INVALID_ARGUMENT` を返し、出力先バッファの不足に対しては `BUFFER_TOO_SMALL` を返す。エンコーダは書き込み前に事前計測パスを実行して電文全体の長さを算出するため、出力先不足時に呼び出し側のバッファを変更することはなく、データが中途半端に途切れて出力されることもない。

## STATUS の筐体（enclosure）観測値

同一リリース内において、`StatusResponse` は `opt(enclosure)`、それに続く `opt<bool> lnb_15v_enabled` および `opt<bool> card_inserted` をこの順序でエンコードしてから `topology` を配置する。各 optional boolean は、1 バイトの存在タグ（0: 非存在、1: 存在）と、存在時のみ後続する 1 バイトの真偽値（0: false、1: true）で構成される。非存在の値は、未知（unknown）、利用不可（unavailable）、または未観測（unobserved）を意味し、観測された結果としての `false` とは厳密に区別される。`card_inserted` は筐体レベルでの内部カード観測値を表し、リーダー状態を集約して算出した値ではない。外部リーダーの観測値は各リーダーレコード内に個別に保持される。

エンベロープバージョンは UDTV 1.0 である。本レコードレイアウトは、同一リリースに属するデーモンおよびクライアントコンポーネント間でのみサポートされる。本リビジョンにおける `Model` レコードは名前のみ（name-only）であり、`id` と `name` の双方をエンコードしていた初期開発系列（Core `af82b7a`）とは電文バイト列が異なる。これは完全なレコードレイアウトがその初期開発系列と互換性を持つことを意味しない。2 つの筐体観測 optional を持たなかった旧レイアウトを含め、異なるリリース間での STATUS レイアウトの混在はサポート外である。デコーダは厳密なデータ長検査および存在フラグの厳格な検証を維持する。

## ストリームカウンタ

`StreamCounters` は昇順に整序された 2〜16 個のエントリを運ぶ。ID 5（`transmitted_packets`）および ID 6（`queue_dropped_packets`）は必須であり、ID 7〜32 は任意（optional）である。ID 0 および ID 33〜255 は拒否される。ID 7〜12 は `stream_bytes`、`sync_errors`、`tei_packets`、`continuity_errors`、`stream_usb_errors`、`empty_intervals` として固定されており、ID 13〜32 は将来の拡張用として予約されている。各エントリは既存の `Counter` 測定状態を使用する。`valid` は測定値 0 を含み、`unknown` はその未測定理由を保持し、任意項目で `unsupported` のエントリは JSON および CLI 表示から除外される。未知の任意エントリは形式検証された上で、外部への投影処理によって安全にスキップされる。既知のフィールド名を誤ってエイリアスすることはない。フレームのエンベロープはバージョン 1.0 を維持する。異なるリリースのピア間通信は未サポートであり、厳格なデータ長検査および意味的妥当性の検証は変更されない。

## 検証処理の分離（Validation split）

バイトコーデック層は、レコード単体に局所的な構文および上限付きコンテナの境界のみを検査する。参照整合性や文脈を検証する独立したバリデータ群が存在し、スナップショットの公開や操作ディスパッチの前に必ず実行されなければならない:

- `validate_error_record`、`validate_scope`
- `validate_capabilities`、`validate_receiver_descriptor`、`validate_reader_descriptor`、`validate_topology`、`validate_limits`
- `validate_measurements`（測定項目 ID 集合は宣言された `measurement_mask` と完全に一致しなければならない。宣言された各測定項目は必ず存在し、`unknown` または `valid` でなければならず、`unsupported` であってはならない）
- `validate_stream_counters`（必須 ID 5, 6、任意 ID 7〜32、最大 16 個の昇順エントリ）、`validate_receiver_counters`（ID 1〜4 のサブセット）。これらには `overflow=true` の場合に `value == UINT64_MAX` を要求する飽和規則が含まれる
- `validate_stream_final`（理由コードとエラー情報の結合整合性）、`validate_receiver_status`、`validate_reader_status`、`validate_card_observation`、`validate_card_session_result`、`validate_card_status_response`
- `validate_binding`（`function_id` は同一スナップショット内に存在しなければならない）
- `validate_list_response`
- `validate_status_response_scope`（形状のみの事前検査）および `validate_status_response`（全体集約検査）

`validate_status_response(value, StatusContext{list})` は、スナップショットを公開する前、または制御要求をディスパッチする前に呼び出し側が実行する完全な検証パスである。コンテキストとして、正規化・検証済みの同一 epoch の `ListResponse` を渡す。これにより、scope が属する epoch と、公開された完全な機能/レシーバ/リーダーの記述子集合が提供される。すべてのバリデータは、まず対象データに対してコーデック局所の厳密な検査を実行するため、型付きインターフェースを直接呼び出す側が `static_cast` を使って列挙値・状態・予約値・上限規則を迂回することはできない。その上で、集約検査として以下を照合する:

- 応答コンテキストの epoch が正規 LIST の epoch と一致していること。
- 重複 ID の排除と所属関係の照合: 応答に含まれるすべての function/receiver/reader ID が正規 LIST に実在し、receiver/reader 記述子の `systems`、`binding`、`capabilities` が正規記述子と完全に一致すること。
- 全体（`enclosure`）スコープの場合: 応答が同一 epoch の完全な機能、レシーバ、リーダーの集合を返し（要素数および ID が正規 LIST と等しい）、各コレクションの存在有無が正規 topology と一致すること。既知の正規 topology では両方のコレクションが存在することを要求し、真に未知の正規 topology では両方が省略されることを要求する。リーダーが存在しないことが判明している場合は、非存在ではなく要素数 0 の配列として表現する。
- 部分スコープの場合: 返された単一の receiver または reader が正規メンバーであり、その記述子が一致すること。内部参照は依然として正規機能集合に対して解決される。
- 正規の公開レシーバ数に対する `Limits.max_concurrent_streams` の照合、およびスナップショット時刻 `snapshot.observed_at_ns` に対するすべての有効な測定値/カウンタ/カードサンプリング時刻の照合。
- 保持されている（前回の）ストリーム対象が存在する場合、そのレシーバが依然として利用権保持状態（`leased`）であることを要求する。

正規コンテキストが提供されない場合（またはその epoch が一致しない場合）、関数は `INVALID_ARGUMENT` を返す。部分レコード自身の申告が、検証済み LIST の代わりになることは決してない。`validate_status_response_scope` は形状のみの事前検査として機能する。

## 本インクリメントに含まれないもの

- ディスパッチャーおよび接続状態の管理: 初回 HELLO、交渉済みバージョン、再利用または過去の request ID の拒否、不足している必須機能に対する `UNSUPPORTED`、STATE_EVENT 購読、要求と応答の突き合わせ。
- フレーム受信および HELLO に対する 5 秒の制限時間管理、ソケット I/O、タイムアウト制御、デーモンや実ハードウェアとの統合。
- PID フィルタ機能の有効化判定、周波数/TSID/スロット番号のプロファイル範囲検査、JSON エクスポータおよび CLI 向け出力変換。
- 各製品の個別エンドポイント、CLI コマンド、JSON 出力の切り替え、バージョン番号の更新や正式リリース。

本節は `dtv::wire` コンポーネント単体について述べている。ビルドオプション `DTV_USERLAND_BUILD_CONTROL=ON` の場合、分離された内部コンポーネント `dtv::control` が [`control-connection.md`](control-connection.md) に記載されたソケット不要な接続ゲートと応答ヘルパーを提供する。サービス実行や製品統合はそのコンポーネントの対象外である。

## 確定事項として扱う規則

本リビジョンに先行する設計レビューにおいて、設計文書群から一意に確定していると判断された事項は以下のとおりである。これらは曖昧な未決事項ではなく、確定した規則として実装されている:

- Capabilities（機能情報）は、`controls` ビットフィールドの直下に存在フラグタグを**伴わずに** LNA/LNB データを直接配置する。したがって、最小構成の capabilities は 4 バイトのゼロ値となる。
- 発行される lease、stream、card handle、generation の各 ID は必ず 0 以外（nonzero）とする。ストリームスコープの対象 ID も同様に 0 以外とする。存在する `Error` 構造体は必ず 0 以外のコードを持つ。
- `overflow=true` の場合は必ず `value == UINT64_MAX` とする。飽和したカウンタ値が周回（ラップアラウンド）することはない。
- `quiesced=false`（ハードウェア未静定）を伴う STOP 完了通知は、必ず `final.error` を保持しなければならない。
- TRANSMIT コマンドのデータ長は 1〜4096 バイトとする。
- ISDB-T では TSID または slot を選択できない（`selection` は常に `automatic` とする）。
- `UsbFunction.model` が存在しない場合は、`none` 以外の識別理由（identification reason）を要求する。既知のコールドモデルは `cold_model` を保持してよい。
- LNA 機能が提供される場合、その既定値は記述子のサポート変調方式と厳密に一致しなければならない（サポートされる方式ごとに 1 エントリ。不足、余剰、重複は認められない）。
- `frequency_hz` および指定時の `programmed_hz` は正数かつ最大 2^53−1 とする。周波数プロファイル範囲の検査はコーデックの外側で行う。
- STATUS の `enclosure.identity` がスキャンの一意性（uniqueness）を独自に主張することは決してない。シリアル状態が `present` または `unknown` の場合、uniqueness は `unknown` となり（unknown は不在の確定ではない）、不在（`absent`）が確認された場合のみ `not_applicable` となる。`unique_in_scan` および `duplicate` は LIST および discovery のみが決定する値である。
- 構成が判明している topology は `receivers` と `readers` の両方を返す。リーダーが存在しないことが判明している場合は、非存在ではなく要素数 0 の配列とする。構成が未知の topology のみ両方を省略する。
- TUNE の `timeout_ms` およびその他の TUNE optional 項目は設計上明示的に定義されている。「要求されないタイムアウトオプションは存在しない」という記述はカード制御固有のものである。
- 詳細な STOP、カード制御、部分スコープの各レコード仕様は、基本草案と矛盾するものではなく、それを精緻化したものである。

## 残された未決事項

既存の設計文書群から一意に導出できない事項のみを未決として記録する:

1. **ゼロパケットの TS_DATA**: 電文レイアウトでは最大パケットデータ長と 188 バイトアラインメントを定めているが、パケット数が 0 件の TS_DATA イベントが正当であるかは明記されていない。本コーデックではこれを受理する。
2. **PID 配列の整序性**: 草案は重複がないことを要求しているが、昇順に整列されていることは要求していない。本コーデックではビットマップにより重複を排除し、送信された順序をそのまま保持する。
3. **ピアから通知された HELLO 上限値**: `max_control_payload` および `max_ts_bytes` は通信相手の申告値であり、そのまま運搬する。自側のローカル上限値との照合判定はコーデック層では行わない。

## 来歴（Provenance）

本実装の要件は、`adoption-status.md` の条件付き採用に基づき、`shared/design/common-wire-draft.md`、`wire-record-layout.md`、`common-api-contract.md`、`json-record-contract.md`、`cli-contract-draft.md` から抽出されたものである。コードは独自のクリーンルーム MIT 実装であり（`independent-code-policy.md`）、既存製品のペイロード実装、カードプロトコルコード、ハードウェアシーケンス、ベンダーのアセットをコピーしていない。
