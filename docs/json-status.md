# STATUS JSON 投影

関数 `dtv::userland::wire::status_json_size` および `encode_status_json` は、検証済みの `StatusResponse` レコードを 1 つのコンパクトな UTF-8 JSON オブジェクトへと投影する。呼び出し側は、`validate_status_response` の検証で使用された正規の同一 epoch を持つ `ListResponse` を提供しなければならない。エンコーダは出力の末尾に改行文字（`\n`）を 1 つ付与し、終端 NUL 文字は出力せず、内部での動的メモリ割り当てを行わない。入力データ、コンテキスト情報、出力バッファ領域は互いに重複してはならず、関数実行中に内容を変更してはならない。

## 公開フィールドの対応関係

出力されるルートオブジェクトのフィールドは `schema`、`producer`、`context`、`snapshot`、`scope`、`enclosure`、`topology` である。`usb_functions`、`receivers`、`readers` は、対応するコレクションデータが存在する場合にのみ出力に含まれる。

| 出力 JSON パス | データ型および表現規則 |
| --- | --- |
| `producer.product`, `producer.version`, `producer.core_version` | 文字列。未定義の core バージョンは `null` |
| `context.epoch`, `context.instance`, `context.state`, `context.last_error` | 小文字 16 進数の epoch 文字列、null 許容の instance 文字列、可読状態文字列、null 許容のエラーオブジェクト |
| `snapshot.sequence`, `snapshot.observed_at_ns` | 64 ビット整数値をそのまま保持する 10 進文字列 |
| `scope.kind`, `scope.id` | 可読種別文字列。enclosure の id は `null`、stream の id は 10 進文字列、receiver/reader の id は JSON 数値 |
| `enclosure.id`, `enclosure.model`, `enclosure.serial` | JSON 数値、モデル名文字列または `null`、シリアル番号文字列または `null` |
| `enclosure.lnb_15v_enabled`, `enclosure.card_inserted` | JSON 真偽値（boolean）または `null` |
| `enclosure.limits.max_concurrent_streams` | JSON 数値または `null` |
| `topology.state`, `topology.expected_function_count` | 可読状態文字列、および JSON 数値または `null` |
| `usb_functions[]` | `id`、`usb`、`mode`、`location.usb_path`、`location.bus`、`location.address`、`serial`、`model`、`present`、`last_error` を保持するオブジェクト配列 |
| `receivers[]` | `descriptor`、`present`、`state`、`operation`、`leased`、`tune_revision`、`current_tune`、`last_successful_tune`、`stream`、`last_stream`、`measurements`、`statistics`、`last_error` を保持するオブジェクト配列 |
| `receivers[].descriptor` | `receiver` 番号、`ISDB-T` や `ISDB-S` などの正式名称を格納する `systems` 配列、null 許容の `binding`。存在する binding は数値の `function_id` と `local_id` を持つ |
| `receivers[].current_tune`, `receivers[].last_successful_tune` | 10 進文字列 `revision`、`system`、数値 `frequency_hz`、null 許容の数値 `programmed_hz`、`selection`、`verification` を保持する null 許容の選局オブジェクト。`selection.kind` は `auto`、`tsid`、`slot` のいずれかであり、`auto` の場合 `selection.value` は省略される |
| `receivers[].stream` | 10 進文字列の `id` と `tune_revision`、および `phase` を保持する null 許容のアクティブストリームオブジェクト |
| `receivers[].last_stream` | 10 進文字列の `id` と `tune_revision`、`reason`、null 許容の `error`、`statistics` カウンタマップを保持する null 許容の終端ストリームオブジェクト |
| `receivers[].measurements`, `receivers[].statistics` | サポート対象の測定値オブジェクトおよび名前付きカウンタ。各コレクションは型付き STATUS 値で実際に観測されたエントリのみを反映する |
| `receivers[].statistics.receiver`, `receivers[].statistics.stream` | 受信機カウンタマップおよび null 許容のストリームカウンタマップ。各カウンタオブジェクトは `value`、`sampled_at_ns`、`reason`、`overflow` を保持する |
| `readers[]` | `descriptor`、`present`、`card`、`operation`、`handles_open`、`transaction_active`、`last_error` を保持するオブジェクト配列。`card` は `state`、`sampled_at_ns`、`reason` を含む |
| Core の `Error` enum に定義されたエラー `code` | 定義済みのエラー識別文字列 |
| 上記 enum の定義外のエラー `code` | `code: "UNKNOWN"` および元の数値 `numeric_code` |

`context`、`limits`、`measurements`、`statistics`、`tune.selection.kind`、`stream.phase` などのキー名は、既存実装の投影仕様をそのまま維持している。ここでの定義は外部公開される JSON の形状を規定するものであり、採択済みの STATUS 仕様決定事項や Core により検証された値の範囲を超えて、追加のフィールド意味論を創出するものではない。

STATUS および discovery JSON における公開 `model` 値はモデル**名前のみ（name-only）** であり、文字列または `null` で表現される。モデル識別用の内部 ID は出力しない（形式例: `"PX-Q3U4"`）。これは採択済みの仕様決定（回答 2.A / RC47 決定）に準拠したものである。`receivers[].descriptor.capabilities` は本 STATUS 投影では出力しない。これは能力情報の公開自体を取りやめるものではなく、静的なリーダーおよび LNB 構成は discovery JSON 側で公開される。LNA の設定値および既定値のみが後続の設計検討のために保留されており、その詳細仕様はここでは規定しない。

`usb_functions[]` は、内部状態フィールドである `serial_state` や `identification_reason` を出力しない。これらのフィールドは Core 内部の照会・識別処理の状態を保持するものであり、公開 JSON 投影の語彙には含まれない。

非存在の optional 文字列または未観測の項目は、JSON の `null` として出力する。内部で使用される `IdentitySerialState`、`Uniqueness`、`UsbFunction::serial_state`、`UsbFunction::identification_reason` などの型定義は、公開 status の語彙ではない。`serial_unique` はスキャン全体の機器検出結果であり、単一の STATUS 応答から算出されるものではない。筐体レベルのカード存在フラグは内部での直接観測値であり、リーダー状態から集約されたものではない（外部リーダーの状態は `readers[].card` 配下に個別に維持される）。また、本 STATUS 投影がスキャン全体の検出階層（`enclosures[].devices[].receivers[]`）を構築することはない。

## エンコーダの出力保証

公開されている両方の出力関数は、処理の開始時に正規 LIST に対する完全な `StatusResponse` 検証を実行する。`status_json_size` は末尾の改行文字を含む厳密な出力バイト数を算出する。`encode_status_json` は実際の書き込み前に事前計測を行い、入力値の不正、出力スパンの異常、出力先バッファの不足が検出された場合は、呼び出し側のバッファ内容を一切変更しない。文字列のエスケープ処理では、JSON 規格に従い二重引用符、バックスラッシュ、制御文字を適切にエスケープし、正当な UTF-8 バイト列を維持する。内部のライター実装は上限付きのバイトカウンタを使用し、書き込み専用の非検査 API を外部へ露出しない。

このサイズ計測付き安全ライターの動作は、修正された公開投影仕様のために独自に設計・実装されたものである。以前の Core #13 JSON 実装は、サイズ事前計測、文字エスケープ、書き込み前検証のアプローチに関してのみ参考とされた。その古い STATUS フィールドマッピングおよびテストフィクスチャは公開契約ではない。本仕様のマッピングは、2026-10-19 に採択された JSON 決定および 2026-10-22 の型付き STATUS レイアウト決定に厳格に準拠している。設計の背景および詳細範囲については、agent-workspace の `design/core-json-next-increment-plan-20261022.md` および `DECISIONS-20261021.md` を参照のこと。
