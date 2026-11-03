# STATUS JSON 投影

`dtv::userland::wire::status_json_size` および `encode_status_json` は、検証済みの `StatusResponse` を 1 つのコンパクトな UTF-8 JSON オブジェクトへと投影する。呼び出し側は、`validate_status_response` で使用される正規の同一 epoch の `ListResponse` を提供する。エンコーダは末尾に改行 1 つを追加し、NUL は含めず、動的メモリ確保を行わない。入力、コンテキスト、出力領域は重複してはならず、呼び出し中は変更されてはならない。

## 公開フィールドマッピング

出力されるルートフィールドは `schema`、`producer`、`context`、`snapshot`、`scope`、`enclosure`、`topology` である。`usb_functions`、`receivers`、`readers` は、対応する optional コレクションが存在する場合にのみ含まれる。

| 出力パス | JSON 表現 |
| --- | --- |
| `producer.product`, `producer.version`, `producer.core_version` | 文字列。非存在の core バージョンは `null` |
| `context.epoch`, `context.instance`, `context.state`, `context.last_error` | 小文字 16 進数の epoch、nullable な instance、可読ステータス文字列、nullable な error |
| `snapshot.sequence`, `snapshot.observed_at_ns` | 64 ビット全体を保持する 10 進文字列 |
| `scope.kind`, `scope.id` | 可読 kind 文字列。enclosure id は `null`、stream id は 10 進文字列、receiver/reader id は JSON 数値 |
| `enclosure.id`, `enclosure.model`, `enclosure.serial` | JSON 数値、モデル名文字列または `null`、シリアル文字列または `null` |
| `enclosure.lnb_15v_enabled`, `enclosure.card_inserted` | JSON boolean または `null` |
| `enclosure.limits.max_concurrent_streams` | JSON 数値または `null` |
| `topology.state`, `topology.expected_function_count` | 可読 state 文字列、および JSON 数値または `null` |
| `usb_functions[]` | `id`、`usb`、`mode`、`location.usb_path`、`location.bus`、`location.address`、`serial`、`model`、`present`、`last_error` を持つオブジェクト |
| `receivers[]` | `descriptor`、`present`、`state`、`operation`、`leased`、`tune_revision`、`current_tune`、`last_successful_tune`、`stream`、`last_stream`、`measurements`、`statistics`、`last_error` を持つオブジェクト |
| `receivers[].descriptor` | `receiver` 番号、`ISDB-T` や `ISDB-S` などの正式名称の `systems` 配列、nullable な `binding`。存在する binding は数値の `function_id` と `local_id` を持つ |
| `receivers[].current_tune`, `receivers[].last_successful_tune` | 10 進文字列 `revision`、`system`、数値 `frequency_hz`、nullable な数値 `programmed_hz`、`selection`、`verification` を持つ nullable な tune オブジェクト。`selection.kind` は `auto`、`tsid`、`slot` のいずれかで、`auto` の場合 `selection.value` は省略される |
| `receivers[].stream` | 10 進文字列の `id` と `tune_revision`、および `phase` を持つ nullable なアクティブストリームオブジェクト |
| `receivers[].last_stream` | 10 進文字列の `id` と `tune_revision`、`reason`、nullable な `error`、`statistics` カウンタマップを持つ nullable な最終ストリームオブジェクト |
| `receivers[].measurements`, `receivers[].statistics` | サポートされる測定値オブジェクトおよび名前付きカウンタ。各コレクションは型付き STATUS 値によって表されるエントリのみを反映する |
| `receivers[].statistics.receiver`, `receivers[].statistics.stream` | レシーバカウンタマップおよび nullable なストリームカウンタマップ。各カウンタ値オブジェクトは `value`、`sampled_at_ns`、`reason`、`overflow` を含む |
| `readers[]` | `descriptor`、`present`、`card`、`operation`、`handles_open`、`transaction_active`、`last_error` を持つオブジェクト。`card` は `state`、`sampled_at_ns`、`reason` を含む |
| Core `Error` enum 内のエラー `code` | 定義されたシンボル |
| その enum 外のエラー `code` | `code: "UNKNOWN"` および元の数値 `numeric_code` |

`context`、`limits`、`measurements`、`statistics`、`tune.selection.kind`、`stream.phase` などの名称は、既存の実装における投影から維持されている。ここでの記述は出力される形状を文書化するものであり、採択された STATUS の決定事項や Core によって検証された値を超える追加のフィールド意味論を確立するものではない。

STATUS および discovery JSON における公開 `model` 値はモデル**名のみ**であり、文字列または `null` として表現される。モデル識別子は出力されない。名前の形式例は `"PX-Q3U4"` である。これは採択された回答 2.A / RC47 決定に従う。`receivers[].descriptor.capabilities` は本 STATUS 投影では出力されない。これは機能公開全体を保留するものではない。discovery JSON は静的なリーダーおよび LNB 構成を公開する。LNA の値およびデフォルトのみが後続の調査のために保留されており、その公開の詳細はここでは確立されない。

`usb_functions[]` は、内部フィールドである `serial_state` や `identification_reason` を出力しない。これらのフィールドは内部の照会/識別状態を記述するものであり、本投影の一部ではない。

非存在の optional 文字列または観測値は JSON の `null` として出力される。内部の筐体 `IdentitySerialState`、`Uniqueness`、`UsbFunction::serial_state`、`UsbFunction::identification_reason` は公開 status の語彙ではない。`serial_unique` はスキャン全体の検出結果であり、単一の STATUS 応答から算出されるものではない。筐体の CARD フィールドは内部の観測値であり、リーダーから導出されるものではなく、リーダーの状態は `readers[].card` の配下に留まる。本投影は、スキャン全体の検出階層である `enclosures[].devices[].receivers[]` を構築しない。

## 出力処理の保証（Writer guarantees）

公開される双方の操作は、まず正規 LIST に対する完全な `StatusResponse` 検証を実行する。`status_json_size` は末尾の改行を含む厳密なバイト数を返す。`encode_status_json` は書き込み前にカウントを行い、不正な値、不正な出力スパン、出力バッファ不足時には呼び出し側バッファを変更しない。文字列は JSON のクォート、バックスラッシュ、制御文字をエスケープし、正当な UTF-8 バイト列を維持する。内部ライターは上限付きバイトカウントを使用し、書き込み専用の公開 API を露出しない。

この上限付きライターの振る舞いは、修正された公開投影のために独自に実装されたものである。以前の Core #13 JSON の成果は、サイズカウント、エスケープ、および書き込み前検証の手法についてのみ参照された。その古い STATUS フィールドマップおよびフィクスチャは公開契約ではない。上記のマッピングは、2026-10-19 に採択された JSON 決定および 2026-10-22 の型付き STATUS レイアウト決定に従っている。元となった決定事項および範囲については、agent-workspace の `design/core-json-next-increment-plan-20261022.md` および `DECISIONS-20261021.md` を参照のこと。
