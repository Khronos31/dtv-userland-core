# DTV userland 1.0.0 外部契約（案）

状態: **承認前の案。未レビュー・未merge。** 本書は 1.0.0 の外部契約を一本にまとめた候補であり、採用済みの仕様ではない。本文中の「採用済み」はユーザー決定記録に明示の根拠がある項目だけを指し、「未決」は判断が残る項目を指す。実装・ライセンス表示・版・タグ・リリースは本書で変更しない。

- 対象: PX4 / ASICEN / Siano / PIX / Torne の 1.0.0
- 規範材料: ユーザー決定記録、Core の `docs/`、全5製品 CLI 共通契約の §0（確定対象表）に限る。
- 規範にしないもの: 現行製品実装、旧草案の数値、既存利用者の呼び出し方。これらは移行メモと点検にだけ使う。
- 製品ごとの能力差は各製品 SPEC の付録案として分離する。本書は共通規則だけを持つ。
- L3（埋め込み）は別途のアーキテクチャー設計案を §6 に要約して統合した。§6.1 の構成は採用済みだが、個別の関数名・ABI・例外型・数値は未決であり、確定扱いにしない。

## 0. 読み方: 6つの観点との対応

| 依頼観点 | 本書の節 |
|---|---|
| 引数（argv）の共通項と製品別追加・除外 | §3 L1、製品 SPEC 付録 |
| ライブラリとして静的リンクする際の API | §6 L3（構成は採用済み、個別 API は未決） |
| JSON の書式 | §4.8、[json-discovery](json-discovery.md)、[json-status](json-status.md) |
| wire IPC | §4、[wire-subset](wire-subset.md)、[wire-records](wire-records.md) |
| runtime / FD の受け渡し | §5、[control-client-runtime](control-client-runtime.md)、[identity](identity.md) |
| その他（終了・シグナル・所有・キャンセル/クローズ・ライセンス） | §3.7–3.8、§4.6、§6.4、§7 |

## 1. 層モデル（採用済み）

利用者の層ごとに契約を一つだけ割り当てる。上位の層は下位の層と同じ不変条件を同じコードで継承する。

| 層 | 対象 | 主な利用者 | 約束 |
|---|---|---|---|
| L1 プロセス | 引数（argv）、標準出力の TS、終了コード、シグナル・EOF、FD の受け渡し、runtime ディレクトリ | mirakc、dtv-android、ランチャー | 1.0.0 の規範。メジャーで固定 |
| L2 wire と JSON | IPC レコード・opcode、discovery・status JSON | C# 実装の外部利用者、ctl | 規範。バージョン交渉付き（§4.5 の未決を含む） |
| L3 埋め込み | 製品間で同じ形の C ヘッダー、各関数の意味（受付判定、ゲート、Error、所有、キャンセル・クローズの期限） | WebTS、in-process 利用者 | リリースごとに固定するソース契約。安定 ABI は約束しない |
| L4 内部 | Family、TunerServiceBackend、Core host アダプター、`detail/`、製品内部クラス | 製品自身のみ | 契約ではない。予告なく変えてよい |

- 層分けは採用済み。L3 の構成は採用済みだが、具体契約は設計対象であり、この採用だけで個別 API を確定しない。
- L3 の約束は **リリースごとの固定** とする。リリースをまたぐ無期限の安定 ABI を新たに約束しない。
- L4 から L3 への昇格はユーザー判断とする。WebTS が現在使っている Family 等は L4 にあたる。
- `include/dtv/userland/detail/` は L4 の印として扱う。
- 目標構成（ユーザー 6603670549）: Core の共通ライブラリと、同じ共通仕様に適合する各製品ライブラリを持ち、実行バイナリはそれらと疎結合にする。入口だけを共通化してライブラリの責務・再利用性が整わない状態や、CLI 側が選局・ゲートを再実装する状態を完成としない。
- 契約テストは層ごとに作り、どの層の規範を検査しているかを明記する。

## 2. 共通の前提

- 製品は 5 つ。公開コマンドは製品ごとにデーモン（`*d`）、通常 TS クライアント（`*-ts`）、ctl（`*ctl`）、および配布ランチャー/ラッパー。
- 通常 TS クライアントは **すべてデーモン IPC 経由**とする。Torne の `--device` による直接 USB 経路は廃止し、共通のデーモン接続・物理セレクター/アサーションとして扱う（採用済み）。
- 同じオプション名に製品ごとの別の意味を割り当てない。ハードウェアにない機能は黙って無視せず、明示的な未対応（unsupported）として返す。
- すべての入口が同じ規則に到達することを目的とする。同じコードを使うことはその手段の一つにすぎない。

---

## 3. L1: プロセス契約

### 3.1 コマンドと役割

| 役割 | コマンド | 責務 |
|---|---|---|
| デーモン | `px4d` / `asicend` / `sianod` / `pixd` / `torned` | USB 筐体の所有、ファームウェア、選局、キャプチャー、カード、IPC の公開 |
| TS クライアント | `px4-ts` / `asicen-ts` / `siano-ts` / `pix-ts` / `torne-ts` | デーモンへ接続し、選局・受信し、生の TS を標準出力または `--output` へ書く |
| ctl | `px4ctl` / `asicenctl` / `sianoctl` / `pixctl` / `tornectl` | `list` / `status` / CARD / LNB など、能力に応じた制御と状態参照 |
| ランチャー | 各配布ラッパー（例 `px4-termux`、`pix-termux`、`torne-termux`） | 権限・FD・runtime の準備、デーモン起動、終了時の後始末 |

デーモンは筐体ごとに 1 プロセスを基本とする。ctl の `list` はデーモンの `--list` とは別物であり、廃止対象ではない。

### 3.2 共通 argv

以下は全製品で同じ名前・同じ意味を持つ対象。`D`=デーモン、`T`=通常 TS クライアント、`C`=ctl の既存ルートオプション。

| option / 短名 | 役割 | 値・単位 | 状態 |
|---|---|---|---|
| `--help` / `-h` | D/T/C/ランチャー | なし | 採用済み。他の引数にかかわらずヘルプを表示し正常終了（§3.3） |
| `--version` | D/T/C/ランチャー | なし | 採用済み。ヘルプがなければバージョンを表示し正常終了。`-V` は廃止（§3.5） |
| `--firmware` | D | ファイルパス | 採用済み。全デーモンで明示パス必須。`-F` は廃止 |
| `--device` | D/T/C | `serial:TEXT` または `usb:BUS-PORT[,BUS-PORT...]` | 採用済みの共通セレクター。デーモンの `-d` は個別判断（未決） |
| `--instance` | D/T/C | トークン | 採用済み。入力の大文字小文字を保持し、`RoomA` と `rooma` は別 |
| `--runtime-dir` | D/T/C | パス | 採用済み。明示値優先、不正なら無言のフォールバックをしない。省略時の共通解決は別設計（未決） |
| `--group` | D/C とエンドポイントを開く T | なし | 採用済み。POSIX グループと Windows ACL を区別。OS 非対応時の応答は未決 |
| `--fd` | D、ランチャー内部の受け渡し | 10 進 FD | 採用済み。借用した USB 記述子。数・値範囲は能力差（§5） |
| `--exit-on-stdin-eof` | D | なし | 採用済み。Windows 限定・オプトイン・既定オフ。POSIX は現行どおり未知引数/2 |
| `--models` | D | なし | 採用済み。接続を試さず、対応機種名＋VID:PID を静的プレーンテキストで出す |
| `--list-json` | D | なし | 採用済み。接続中の discovery の JSON。`--list` は廃止 |
| `--freq` / `-f` | T | Hz の符号なし整数 | 採用済み。`--channel` と排他 |
| `--channel` / `-c` | T | チャネル文字列 | 採用済み。mirakc のテンプレートへ渡せる文字列。`--freq` と排他 |
| `--system` / `-s` | T | `isdb-t` / `isdb-s` | 採用済み。ASCII の大文字小文字を区別しない。チャネルから導出できる場合は省略可 |
| `--receiver` | T | 符号なしの受信機インデックス | 採用済み。全 TS クライアントで明示必須。省略時に 0 へ暗黙選択しない |
| `--slot` | T | 符号なしの衛星スロット | 採用済み。`--tsid` と排他 |
| `--tsid` | T | 符号なしの衛星 TSID | 採用済み。`--stream-id` の共通正規名。`--slot` と排他 |
| `--time` / `-t` | T | 秒の整数、または `-` | 採用済み。`-` または省略は無期限 |
| `--packet-count` | T | 正のパケット数 | 条件付き方針。無理なく共通化できる場合だけ提供（未決を含む） |
| `--tune-timeout-ms` | T | ミリ秒 | 採用済み。単位 ms を長名に明記。範囲・既定は実装前に凍結 |
| `--output` / `-o` | T | パスまたは `-` | 採用済み。`-` は全製品の標準出力 |
| `--allow-lnb-power` | D | なし | 採用済み。PX4/ASICEN などの ISDB-S 15V 要求の許可。PIX の `--lna` と混同しない |
| `--verbose` / `-v` | D | なし | 一部製品の既存オプション。共通必須かは未決 |
| `--pid` / `-p` | D | PID フィルター | 条件付き方針。全製品必須ではない |
| `--control` | T（Siano 等） | なし | 製品固有の常駐制御。共通の再選局 API へ統合する設計対象（未決） |
| `--lna` | D（PIX） | なし | 当面 PIX のみ。他製品の扱いは未決 |

製品ごとの追加・除外は各製品 SPEC の付録案に記す。ここにないオプションを新設して同じ意味を持たせない。

### 3.3 ヘルプ / バージョン / メタデータの優先

採用済みの規則:

1. ヘルプ/バージョンは、他の引数にどんな不正があってもメタデータを表示して **正常終了**する。
2. ヘルプとバージョンが併存したら **ヘルプ** を表示して正常終了する。
3. メタデータの解決は、通常解析・接続・USB discovery・権限取得より前に行う。
4. 廃止短名 `-F`/`-V` は、メタデータを要求していない通常解析では未知引数/2 のまま。メタデータ優先は通常解析の引数エラー判定に先行する。

`--models` / `--list-json` をメタデータと同じ「単独要求」として扱うかは製品差があり、§7 の未決に置く。

### 3.4 必須入力

- 全デーモン: `--firmware` の明示パスが必須。
- 全 TS クライアント: `--receiver` の明示が必須。
- 通常 TS クライアント: `--channel` か `--freq` のどちらか一方が必須。
- `--freq` 経路では `--system` が必須。`--channel` から system を導出できる場合は省略できる。
- Torne の通常のネイティブデーモン: `--device` を必須とし、最初の筐体を暗黙選択しない。`--fd` を使う場合とメタデータ（ヘルプ/バージョン/models）は device を要求しない。models / ネイティブ device / Windows 限定 EOF / 外部ファームウェアは採用済みの対象であり、実装・検証の未完と混同しない（付録 E）。
- 明示必須を、製品固有の既定値や「接続機器が 1 台なら省略可」で置き換えない。

### 3.5 短名・重複・排他

- 廃止短名: `-F`（`--firmware`）と `-V`（`--version`）は全製品で `UNKNOWN ARGUMENT`/2。ヘルプに掲示しない。従来の互換維持方針はこの後続決定で上書きされた。
- 維持する短名: `-h`、`-f`、`-c`、`-o`、`-t`、`-s`（採用済みのもの）。それ以外の短名は個別決定がなければ発明しない。
- 単一値オプションの重複は引数エラー/2。長短名の混在も同じ。例: `--firmware FILE_A --firmware FILE_B`、`-f 1 --freq 2`。
- 繰返しが仕様の入力（`--fd`、PID 等）は単一値オプションと区別する。重複判定は繰返し仕様の内側で行う。
- 排他: `--freq` と `--channel`、`--slot` と `--tsid`、`--time` と `--packet-count`（後者は条件付き）、地上選局と `--slot`/`--tsid`。
- instance の大文字小文字は保持する。`RoomA` と `rooma` は別 instance。旧小文字正規化案を現在の規範へ再採用しない。

### 3.6 stdout / stderr

- TS 出力は生の TS のみ。`--output -` は標準出力。診断・進捗は標準エラー出力。
- `--list-json` は 1 つの JSON 文書のみを標準出力へ出す。前後に説明文を混ぜない。
- ヘルプ/バージョンは標準出力。
- 成功時のカウンター診断や失敗理由は標準エラー出力。標準出力に混ぜない。
- `--models` はプレーンテキスト（非 JSON・非 CSV）。

### 3.7 終了コード

共通分類 `0, 2..10, 70` を採用済み。数値を IPC エラー enum と混同しない。

| exit | 分類 | 主な対応 |
|---:|---|---|
| 0 | 成功 / 要求された停止 | 正常完了、time/packet 上限、オプトインの stdin EOF、SIGINT/SIGTERM の通常のクリーンアップ完了 |
| 2 | 入力不正 | 引数構文・範囲・相互排他・単一値重複、`INVALID_ARGUMENT` |
| 3 | 対象/機能なし、状態不整合 | `NOT_FOUND` / `NOT_READY` / `UNSUPPORTED` / `INVALID_STATE`。曖昧な device 選択も 3 |
| 4 | 競合 | `BUSY`（別の所有者、回収中、同時ストリーム制限） |
| 5 | 期限切れ | `TIMEOUT`（選局、IPC 応答待ち、停止回収） |
| 6 | IPC 不整合 | `VERSION_MISMATCH` / `PROTOCOL_ERROR`、壊れたフレーム、不整合な final |
| 7 | USB 喪失 / 入出力 | `USB_IO` / `DISCONNECTED` |
| 8 | TS 継続不能 | `SLOW_CONSUMER`、バックエンドが終端エラーとして報告した TS 破損。単なる CC/drop 増加は含めない |
| 9 | カード操作失敗 | `NO_CARD` / `CARD_REMOVED` / card API の `BUFFER_TOO_SMALL`。カード非対応製品の CARD 要求は `UNSUPPORTED`→3 |
| 10 | 起動用データ不正 | `FIRMWARE_REJECTED`、必要なファームウェア/アセットの欠落・不整合 |
| 70 | ローカル / 内部失敗 | 出力の open/write/EPIPE/ENOSPC、`ACCESS_DENIED`、`RESOURCE_LIMIT`、`INTERNAL`、未知の非 0 エラー |

- 運転中に最初に確定した非回復エラーを主エラーに固定する。要求停止・少量の drop は主エラーを作らない。
- 要求停止・time/packet 到達後でも、既に確認した USB/出力/端末エラーを消さない。
- 信頼できる正常な final が取れず、STOP も静定確認できない場合は成功にしない。
- 少量の drop だけで受信を失敗させない。drop 後の正常終了は 0 とする。
- 個々の Error → exit の細部は採用済み記録・実装と照合し、本書の表を新たな数値決定として扱わない（§7）。

### 3.8 シグナル / EOF

- SIGINT/SIGTERM は通常のクリーンアップを経て 0。クリーンアップ失敗時は分類に従う。
- `--exit-on-stdin-eof` は **Windows 限定**、オプトイン、既定オフ。指定時は stdin パイプの EOF から通常のクリーンアップへ進む。POSIX では現行どおり未知引数/2 を維持し、Core の汎用設計を理由に全 OS 対応へ変更しない。
- 全体終了が有限の予算を超えて静定しない場合は異常終了。秒数・OS プリミティブは実装/試験事項。

### 3.9 チャネル / Hz

- `--freq` は符号なし 10 進 Hz、1..2^53−1（JSON 整数との共通上限）。入力 Hz を保持し、バックエンドだけが必要な単位へ量子化する。無言で別周波数へ丸めない。
- `--channel` は mirakc のチャネル文字列をそのまま渡せる文字列。共通で受理する範囲:
  - 地上: `13`..`62`、`T13`..`T62`
  - BS: 奇数 `BS01`..`BS23`、任意で `_0`..`_11`（ゼロ埋めなし）
  - CS: 偶数 `CS2`..`CS24`（`CS02`..`CS08` も受理）
- 地上チャネルは自動選択で slot/TSID を拒否。接尾辞なし BS は slot か TSID が必須。接尾辞あり BS は接尾辞の slot を使い、追加の slot/TSID を拒否。CS は既定 slot 0 で、明示 slot/TSID が置換する。衛星周波数は明示 slot/TSID が必須。slot 0..11、TSID 1..65534。
- system は `isdb-t` / `isdb-s` の ASCII 大文字小文字を区別しない。チャネルとの併記は一致時のみ受理。
- パーサーが受理する範囲と機器能力を分ける。未対応の帯域・方式・slot は `UNSUPPORTED`/3 とし、USB 操作前に拒否する。

---

## 4. L2: wire と JSON

L2 の規範は Core の次の文書に置く。本書は入口と横断規則だけをまとめ、レコードの全フィールドを複製しない。

- エンベロープ・フレーム・HELLO/ACQUIRE の先頭サブセット: [`wire-subset.md`](wire-subset.md)
- 型付きレコードと STATUS/LIST: [`wire-records.md`](wire-records.md)
- 接続ゲート: [`control-connection.md`](control-connection.md)
- オーソリティ / セッション: [`control-authority.md`](control-authority.md)
- 読み取り専用 / 録画クライアント: [`control-client.md`](control-client.md)
- ストリームライフサイクル: [`receiver-v1-stream.md`](receiver-v1-stream.md)
- discovery JSON: [`json-discovery.md`](json-discovery.md)
- status JSON: [`json-status.md`](json-status.md)

### 4.1 転送方式とエンドポイント

- POSIX/Android は Unix ドメインソケット。Windows は名前付きパイプ/ACL 相当（Core の host runtime が吸収）。
- エンドポイントのパスは `<runtime>/<product-slug>/v1/<instance>/<endpoint>`。`endpoint` は `control.sock` または `stream.sock`。product slug は `px4-userland` 等。
- 接続の所有は呼び出し側。Core ライブラリはソケットを生成しない（host runtime がアダプターとして提供）。
- エンドポイントの所有者/mode・ピア UID（POSIX）、所有者 SID・保護 DACL（Windows）を検証する。共有グループは POSIX のみ。

### 4.2 エンベロープ

- magic は ASCII `UDTV`。legacy `PX4U` / `ASCN` は拒否し、ペイロードを解釈しない。
- 20 バイト固定ヘッダー。すべてリトルエンディアン。
- major=1、minor=0。他の版は `VERSION_MISMATCH`。
- opcode は raw u16。未知の opcode もエンベロープとしては受理し、ディスパッチャーが `UNSUPPORTED` を返す。
- flags は 0/1/3 のみ。それ以外は不正形式。
- kind の組合せ: request(bit15=0, flags=0, id≠0)、response(0,1,id≠0)、error_response(0,3,id≠0)、event(1,0,id=0)。他は `PROTOCOL_ERROR`。
- `payload_length` はペイロードを読む前に上限検査する。
- 上限: TS_DATA event は 1,048,504 バイト（1,048,476 + 28）。その他は 65,536 バイト。
- `FrameReceiver` は断片・連結フレームを再構成し、不完全なフレームは 5 秒で破棄する（OS アダプターが期限を強制）。EOF がフレーム途中なら `PROTOCOL_ERROR` で再接続しない。

### 4.3 接続状態

- 最初のフレームは HELLO。HELLO で版と機能（feature）を交渉する。
- 1 接続で同時に 1 要求。request_id は単調増加し、再利用・過去 ID の再使用を拒否する。
- 応答は opcode と request_id が一致する response/error_response のみ受理する。
- HELLO/LIST/STATUS の後、ACQUIRE→TUNE→START→ATTACH→TS_DATA→STOP→RELEASE の順。
- epoch は同一リリース内の識別子。LIST と STATUS の epoch を照合する。
- 制御接続と data 接続を分ける。ATTACH と TS_DATA は data 接続で行い、制御接続は要求/応答だけを運ぶ。
- 不足する機能は `UNSUPPORTED`。未知の機能ビットは運搬し、コーデックは判定しない。

### 4.4 ペイロード

- 型付きペイロードは正確なサイズ。サイズ不一致は `PROTOCOL_ERROR`、サイズが正当で意味が不正なら `INVALID_ARGUMENT`。
- 文字列は `u16 length + UTF-8`、バイト列は `u32 length + bytes`、配列は `u16 count`。上限はレコードごと。
- 整数は固定幅リトルエンディアン。JSON では 64 ビットを 10 進文字列で保持する。
- Model は wire/JSON とも **name のみ**（`str[128]` / string or null）。id は持たない。
- lease/stream/card ハンドル/世代の ID は非 0。`overflow=true` は `value==UINT64_MAX`。
- `frequency_hz` と `programmed_hz` は正で 2^53−1 以下。プロファイル範囲はコーデックの外で検査する。
- STATUS のエンクロージャー観測（`lnb_15v_enabled` / `card_inserted`）は「不明」と「観測 false」を区別する。`card_inserted` はエンクロージャー内部の観測で、リーダー集計ではない。
- 未知の任意カウンターは検証して投影では飛ばし、既知のフィールド名へエイリアスしない。

### 4.5 版・互換

- エンベロープバージョンは UDTV 1.0。HELLO の min/max で 1.0 を選ぶ。それ以外は `VERSION_MISMATCH`。
- **L2 は外部契約（採用済み）。** 層モデル採用（Issue23 6603147282 の直前表 6602947137）で、wire のレコード・opcode と discovery/status JSON は C# 実装や ctl を含む第三者向け境界とし、バージョン交渉付きの規範とする。外部化そのものを未決へ戻さない。
- Core の既存 docs（`control-client.md` / `control-connection.md`）は「同一リリース内の内部契約。リリースをまたぐ安定 wire API ではない」と記す。これは草案前の記述であり、本 1.0.0 案の外部境界と整合させる更新対象とする。旧 README/docs の全面改訂を本 PR の追加ゲートにしない。
- **狭い未決:** リリースをまたぐ具体的な互換範囲。同一メジャー内で許す差（レイアウト変更・機能ビット・マイナーの扱い）と、同一リリース前提を残す範囲を決める。推奨は「マイナーは機能でゲートし、レイアウト変更は明示的な機能または版で区別する。L3 と同様に安定 ABI は約束しない」。
- レイアウト変更を機能ビットでゲートするか、同一リリース前提の非互換変更として扱うかは、上記の狭い未決に含める。

### 4.6 所有・キャンセル・クローズ

- ACQUIRE はリースを返す。リースは接続が閉じるとデーモンが回収する。
- START は tune revision と attach token を返す。token は data 接続の ATTACH にだけ使う。
- **wire の STOP 応答は `epoch` / `final` / `quiesced` を返す。** 停止の終結果と静定の有無を外部クライアントに示す。物理クリーンアップの実行責任と所有者の割り当てはデーモン内部（engine/ワーカー）の契約であり、wire のフィールドではない。`cleanup_owner` や `complete_stream_cleanup_v1()` は内部実装の説明であって、外部クライアントが読む値ではない。
- `quiesced=false` は未静定である。final の受信を物理静定と同一視しない。
- 停止・クローズは有限期限を持つ。孤立した pending を成功として扱わない。全体クローズの秒数は候補値であり、実機認定ではない。
- エラーが付いた final は分類に従う。要求停止に結び付かない `owner_closed`/`CANCELLED` は 70。
- drop は計数するが失敗ではない。

### 4.7 エラー

- error_response は非 0 code と detail を持つ。code は Core の `Error` enum の意味に従う。
- Core enum 外の code は JSON で `code:"UNKNOWN"` + `numeric_code`。
- 全入口で同じ Error 分類に到達することを目標とする。アダプターが BUSY/NOT_FOUND/ACCESS 等の区別を潰さない。
- 具体的な wire error payload のバイト配置は [`wire-records.md`](wire-records.md) を正とする。

### 4.8 JSON

公開 JSON は 2 種。

- `userland.discovery`: 接続中の scan。`schema` / `producer` / `scan` / `enclosures` / `ungrouped_usb_devices`。物理階層 `enclosures[].devices[].receivers[]` を保つ。ID は JSON number、VID:PID は小文字 `VID:PID`。
- `userland.status`: 検証済み `StatusResponse` の投影。`schema` / `producer` / `context` / `snapshot` / `scope` / `enclosure` / `topology` と、存在する collection だけを出す。

横断規則:

- 64 ビット値（epoch の hex 以外の sequence/observed_at_ns/stream id/revision）は 10 進文字列。
- 欠測・不明は `null`。観測 false と区別する。
- enum は独自語彙を作らず、簡潔で読みやすい文字列（`ready`、`ISDB-T`、`isdb-t` 等）を使う。
- `model` は name のみ。`id` は出さない。
- ID の寿命: epoch は同一リリースの接続識別子、function/receiver/reader ID はスナップショット内、lease/stream/card ハンドルは操作単位。scan の index を永続 ID として転用しない。
- writer は validate→measure→write。失敗時に出力バッファを変更しない。文字列は JSON エスケープし、有効 UTF-8 を保つ。
- 1 回の出力は 1 JSON 文書。末尾改行の有無は各既存 API の契約に従う。

---

## 5. runtime と FD の受け渡し

### 5.1 runtime ディレクトリ

- `--runtime-dir` は全製品で提供する。明示値が不正なら無言のフォールバックをしない。
- 省略時の共通解決は別 PR の設計対象であり、本書では確定しない（未決）。
- 解決規則（Core の host runtime の既存設計）:
  - POSIX: 絶対 `XDG_RUNTIME_DIR` → `$TMPDIR/dtv-userland-<euid>` → `/tmp/dtv-userland-<euid>`。相対・空・長すぎる・ドット要素は除外。明示ルートは単独使用。
  - 既存候補は所有者と mode を検証し、安全でないものは変更せず次へ進む。
  - Windows: `LOCALAPPDATA` 配下。所有者 SID と DACL を検証。
- エンドポイントは `<runtime>/<product>/v1/<instance>/<endpoint>`。instance の大文字小文字は保持する。
- デーモンは runtime 配下の自分が必要とする階層だけを作る。呼び出し側がルートを用意する契約の製品では、その境界を付録に記す。

### 5.2 FD

- `--fd` は POSIX/Android のデーモン起動用 USB 記述子。TS 出力先の FD/PID とは別。
- 10 進 3..INT_MAX。符号・空白・16 進・8 進解釈・positional FD は廃止。`03` は値 3 として扱い 8 進にしない。
- 繰返し可。同じ数値の重複は 2。上限 8（wire の function 上限）。
- fd 集合が open 対象を限定する。USB 名前空間を勝手に再列挙して不足する function を開かない。`--device serial:...`/`usb:...` は渡された集合へのアサーションとしてだけ使う。
- 不一致・別筐体混在・同じ function を異なる fd で二重指定は 3。閉じた fd・USB でない fd は 2。検査 I/O 失敗は 7。
- 引数順を receiver/function 番号と同一視しない。USB メタデータとプロファイルで役割を照合する。
- ドライバーは内部複製を作り CLOEXEC で保持し、その複製だけを wrap/close する。部分失敗時は作成済みの複製を回収し、元 fd は close しない。
- 元 FD の所有はホスト。ホストは context の `quiesced=true` 完了、または USB FD を保持し得る全プロセスの終了/回収確認まで元の接続を close しない。
- Windows では数値 POSIX fd を HANDLE へキャストせず `UNSUPPORTED`/3。
- cold→warm の再列挙で fd は自動で新 device の fd にならない。権限/新 fd はホストとの明示的な受け渡しが必要。

### 5.3 instance と identity

- `serial:<string>` は非空 UTF-8、最大 512 バイト、NUL/制御文字なし。trim・percent-decode・正規化・大文字小文字の折りたたみをしない。
- `usb:<path>[,<path>...]` は 1..8 個の `bus-port[.port...]`。正の ASCII 10 進、先頭ゼロなし、各 64 バイト以下。重複は構文段階で拒否し、黙って重複除去しない。順序は path 順で、入力の function 役割番号ではない。
- `canonicalize_instance` は 1..64 ASCII、先頭英数字、残り英数字/`_`/`.`/`-`。大文字小文字を保持。末尾ドットと Windows 予約語幹（`con`/`prn`/`aux`/`nul`/`com1..9`/`lpt1..9`、大文字小文字を区別しない）を拒否。
- 構文検査は筐体のグループ化や OS 上の存在を証明しない。
- セレクターと instance を混同しない。device は物理筐体、instance は接続先。

---

## 6. L3: 埋め込み契約（構成は採用済み、個別 API は未決）

**この節の構成は採用済みだが、個別 API は確定していない。** 別途のアーキテクチャー設計案を公開契約案へ統合した要約であり、関数名・型・数値・ライセンスは未決。設計案そのものは実装済み API・許諾済みライセンス・受入済み数値を示さない。

### 6.1 採用済み

- 層モデルを採用し、L3 を埋め込み境界とする。
- **案 C の構成を採用する。** 共通 C 面と `submit`/`completion`/`cancel` の非同期 USB 核を持ち、同期コールバックはワーカー上の橋で接続する。そのうえで、Core の共通ライブラリと、同じ共通仕様に適合する各製品ライブラリを持ち、実行バイナリはそれらと疎結合にする（ユーザー 6603670549）。この採用は構成の採用であり、個別 API の symbol・型・enum 採番・構造体レイアウト・数値・期限・容量を確定しない。
- L3 の約束はリリースごとに固定する。リリースをまたぐ安定 ABI を新たに約束しない。
- 新規 L3 ヘッダーのライセンスは未決であり、MIT を提案する。これはユーザーの選好（GPL-2.0-or-later 希望、合理的なら MIT も可）と新規設計の推奨であり、採用済みのライセンス決定ではない。
- L3 の関数の意味として、少なくとも次を共有する: 入力の単位・範囲の受付判定、identity の文法と大文字小文字、Error enum の意味、資源所有（FD/バッファ/リース）と排他、状態遷移（open→tune→キャプチャー→stop→close）、キャンセル/クローズの有限期限、drop の非失敗扱い。
- サポートする埋め込み境界を「Family＋検査付き入口」に限るか、生 `tuner_backend()` まで含めるかは **未決**（設計案の推奨は、既存 Family を L3 と呼ばず、新しい C 面とリンク境界を分けること）。
- 具体的な関数名・型・ABI・例外は §6.2 の設計案に留まり、未決である。

### 6.2 設計案の要約（採用済みの構成: 共通 C 面＋非同期 USB 核）

設計案は 3 案を比較し、案 C を採用した。以下の詳細は設計案であり、個別 API は未決である。

- **A**: 既存 `Family`/バックエンドを薄く C で包む。5 製品の I/O・所有・選局順が揃わず、GPL-2.0-only のアダプターに依存する。
- **B**: 共通 C 面＋同期 USB コールバックだけ。PIX の `dt295_io` と P/Invoke 利用者には近いが、連続 TS とブラウザーの取消し・有限停止を共通規則にしにくい。
- **C（採用）**: 共通 C 面＋ `submit`/`completion`/`cancel` の USB 核。同期コールバックはワーカー上の橋で接続する。呼び出し側の USB 所有と WebTS のイベントループ非阻塞を両立し、libusb/WebUSB/WinUSB の所有をホストに残す。

責務の分離:

| 構成要素 | 責務 | 持たない責務 |
|---|---|---|
| Core L3 契約ヘッダー（MIT 提案） | 共通の型、Error、リリース照合、受付/完了/取消し、所有・状態、能力照会の語彙 | USB 列挙、デーモン、OS ソケット、カード復号器、製品固有の選局順 |
| Core の純粋な共通部分（MIT 候補） | channel/Hz 解決、受付判定、lease/revision/stream 世代と結果意味 | 未整理の `detail/receiver_engine` を流用しない。L4 クラスの公開 |
| 製品 L3 アダプター | 型変換、能力と実機観測、ファームウェア検証、cold/warm、電源/選局/ロック/セトル/取得ゲート、TS、CARD/LNB、USB 切断の影響範囲 | 呼出側 UI、OS 権限取得、デーモン管理 |
| 呼出側 USB プロバイダー | 許可済み device token の列挙、記述子取得、open/claim/release、control/bulk/interrupt の submit/completion/cancel、単調時計と起床 | 機種の選局・ファームウェア可否判断。無許可の USB permission 要求を L3 へ隠さない |
| host エグゼキューター/同期橋 | 製品処理をワーカーへ実行し、同期 I/O しかない利用者の呼出しを submit/completion へ変換 | UI/main thread の同期 join。無制限のワーカー増加 |
| L1/L2 アダプター | 同じ製品 L3 意味を CLI/wire へ写し、runtime ディレクトリ、引数、認証、FD 受渡しを所有 | L3 と別の選局順・ファームウェア検証の再実装 |

データの流れ（設計案）: ホストが許可済み USB トークン/記述子をプロバイダーへ登録 → `catalog`（静的）または `discover`（登録済みトークンだけを観測、claim しない）→ 明示候補で `open` → アダプターが同じ device を再照合し、必要な function を claim し、ファームウェアを検証して初期化 → `acquire` → `tune` が給電トランザクション、slot/TSID の前後順、ロック/セトル、取得ゲートを一つの期限で実行 → `start`/`attach` で TS プレーンを有効化 → `read` で 188 バイトのパケットを渡す → `stop`/`release`/`close` で完了回収。

API の形（**名称・採番・構造体レイアウトはすべて未決**。以下は設計案であり、個別 API の採用ではない）:

- 製品ごとに同じ `dtv_l3_api` を返すファクトリー（例 `px4_l3_get_api_rX`）。複数製品の静的リンクで裸の symbol を競合させず、異なるリリースのヘッダー/実装を混ぜたらリンクまたはパッケージ照合で失敗させる。
- `release_id`、`catalog`、`discover`、`submit_open`、`capabilities`、`acquire`、`submit_tune`、`submit_start`、`submit_attach`、`read`、`submit_stop`、`submit_release`、`submit_close`、`close_status`、`context_release`、`poll`、`cancel`、`op_release`、CARD 系、LNB 系。`close_status` と `context_release` は設計案の名称であり、採用済み API ではない。
- `dtv_l3_result`（受付/照会）と `dtv_l3_completion`（操作の終結果）を分ける。`submit_*` 成功は受付のみ。`poll` の `PENDING` は未終了。終結果 `TIMEOUT` は期限超過。受付だけで物理完了や USB 静定を推定しない。
- 完了は操作種別のタグ付き union。open→成功・失敗を問わず所有者 context、tune→`{revision, requested_hz, programmed_hz?, verification, lock_measurement}`、start→armed stream、attach→active stream、card reset→ATR、APDU→応答バイト列、close→`{quiesced, retained_scope}`。受付前の検証失敗は副作用なしで op/context を作らない。一度受理した open は部分的な claim/I/O を残す失敗でも回収可能な context を返し、ホストが close と最終解放を行う。
- `op_release` は終完了の取得後だけ許す。解放するのは操作結果と op ハンドルであり、内部 I/O 参照・context・プロバイダー・バッファの最終解放とは別である。結果バッファとハンドルは取得側が明示解放する。
- `read` は `DATA(n)` / `WAIT` / `EOF` / `TERMINAL(error)` を別タグで返す。バッファは呼出し中だけ借り、内部保持しない。`drop_total_packets`（キューで正確に捨てた数）と `upstream_loss_unknown`（USB/復元段で個数不明）を区別し、前者の増加だけで stream を失敗させない。
- 状態: context `opening/ready/closing/closed/failed/disconnected`、receiver `free/leased/tuned/streaming/error`、stream `none/armed/attaching/active/closing/ended`、card `closed/open/reset/transaction/closed`。所有者ハンドルは context 世代を持ち、閉鎖後の旧ハンドルを古いものとして拒否する。
- `tune` は system、`uint64_t` Hz、`auto|slot|tsid` の一択、LNB 電圧/許可、有限の期限を持つ。チャネル文字列の解決は Core の純粋関数で 1 回行い、要求 Hz と実際に設定した Hz を分ける。ロックを測れない製品に架空の true を作らない。
- ファームウェアの供給元は `path`、呼出側 `bytes`、上限付き `read(offset, buffer)` コールバックの排他的な三形。三形ともライブラリ管理領域へコピーしてから検証・USB 書込みへ進む。各製品の manifest/format/model/SHA-256 を USB 書込み前に検証し、呼出側の自己申告ハッシュだけで任意の image を承認しない。cold で必要なら `FIRMWARE_REJECTED`、warm で不要なら無転送。L1 のデーモン `--firmware` 明示必須は維持する。
- プロバイダーはホスト所有。`submit` は受付だけ、転送には正常/失敗/取消しを問わず一つの最終完了を返す。`cancel` 成功は取消し要求の受付であり、バッファ解放の許可ではない。timeout・部分転送の副作用は不明な場合があり、自動再送しない。ライブラリは別の libusb イベント所有者を起こさない。
- `close` は新規受付停止→全 op/stream のキャンセル要求→コールバック回収→card/receiver/USB claim 解放、の有限のエンドツーエンド期限。ここで **操作の期限結果** と **資源の最終静定** を別に観測する。期限内に静定したときだけ `quiesced=true`。期限切れの close op は `TIMEOUT/quiesced=false` と未解放 scope を返して終結し、ホストはその op を release できるが、context・プロバイダー・バッファ・元 FD/USBDevice と内部 I/O 参照は保持する。既に close 中の context への重複 `submit_close` は新しい停止/release を行わず既存の closing 状態を返す。
- `close_status` は同じ所有者ハンドルで非ブロッキングに再照会し、`{phase, quiesced, retained_scope, cleanup_error}` のスナップショットを返す。`quiesced=true` は全受理済み I/O の最終完了回収と card/receiver/claim の後始末が終わったときだけ単調に成立する。保持中の scope がなければ `context_release` を一回だけ許す。`context_release` は静定前なら拒否し、成功後のポインターは無効になる。内部は一回限りの release 遷移で二重コールバック/二重解放を防ぐ。
- 進行担当を分ける。driver 実行レーンは製品の同期手順で待機し得る。USB 完了/イベントポンプ、キャンセル受付、期限通知、`close_status` の読み出しを、その占有レーンに積まない。driver と独立したイベントポンプが最終完了を進め、control/期限担当が `TIMEOUT/quiesced=false` を公開する。クリーンアップは最終転送完了後に進み、完了後だけ `quiesced=true` へ遷移する。専用スレッドを一律必須にしない。
- 例: 受理した open が claim 後に失敗しても、終結果の所有者 context で close を依頼する。クローズ期限中に同期プロバイダーが戻らなければ control/期限担当が `TIMEOUT/quiesced=false` を返し、main は join せず応答を続ける。ホストが close op を release しても資源は残る。遅延した USB の最終完了と driver 復帰の後にクリーンアップが一回完了し、`close_status` が初めて `quiesced=true` を返す。ホストは全 op ハンドルを回収したうえで `context_release` を一回だけ実行し、プロバイダーと元 device/FD を解放できる。遅延完了だけで後始末が終わらない場合、またはプロバイダーが最終完了を失った場合は静定を偽らず保持を続ける。
- 旧 context を閉じず同じ token/FD を新 context へ再貸出ししない。USB 断は関係する function の generation を無効にし、依存する receiver/card を終端状態にする。再接続は新 token/新 context で明示的に行い、旧ハンドルを復活させない。
- リリース固定: 各製品リリースのヘッダー、ソースマニフェスト、Core pin、製品アダプターの組を固定する。ビルド/パッケージ時に照合し、製品名・リリース識別付きファクトリーで異版の静的リンクを失敗させる。`release_id` は一致した組の診断表示であり、異なるリリースのバイナリとの安全な runtime 交渉には使わない。
- 検証は**提案であり未実行**: 疑似 IO で L1/L2/L3 の受付判定が同じ不正値をハードウェアへ一度も届けないこと、遅延完了後のバッファ保持、キャンセル受付と静定完了の分離、各製品の非対応機能と共有資源の BUSY、TS drop と終端。WASM リンク/ブラウザーと P/Invoke 利用者、対象 OS の USB・受信品質は別の実装後検証。

### 6.3 利用者から出ている要件（参考であり、採用決定ではない）

利用者から出ている要件は、正本ではなく設計の参考として扱う。個々の関数名・数値・エラー enum を自動採用しない。次は現案との対応状況であり、未対応を採用済みと書かない。

- WebTS: PX4 のみを現在使用。Family ではなく部品を自前で組み立てている。非同期の submit/completion/cancel を基本にしたいが、ワーカー上の同期ファサードでも受け入れる。main とワーカーの相互待ちを作らないこと、バッファを借りる期間、キャンセル後に遅れて来る完了、クローズ期限切れ時の資源扱いを明記してほしい。内部でスレッドを作ることは要求しない。card+B25 と PIX の機器内復元を同じ API に押し込まないでほしい。
- in-process 利用者: プロセス内（AGPL-3.0-or-later）での利用を希望。USB 転送は呼び出し側が渡す形を想定し、ライブラリは libusb を必須にしないことを希望する。P/Invoke から呼べる C 面を求め、安定 ABI は要求しない。Siano と PX4 を同一バイナリへ静的リンクし、外部 symbol を衝突させないこと、共通の型・関数表を持つことを希望する。同期の読み書きで足り、連続 IN は出しっぱなしの読みで扱うことを想定する。スレッドを作らないことを希望し、必要なら数と停止方法の明示を求める。
- 既存の Family/`EnclosureFamily` は GPL-2.0-only のアダプターに依存するため、全利用者の共通境界にはできない。L3 は新しい C 面とリンク境界を分ける設計を採る。

| 参考要望 | 現案での扱い | 状態 |
|---|---|---|
| 呼び出し側が USB 転送を渡し、ライブラリは libusb を必須にしない | §6.2 の呼出側 USB プロバイダーと組込境界 | 方向は反映 |
| P/Invoke で呼べる C 面。安定 ABI は要求しない | §6 冒頭、§6.1 のリリース固定 | 反映 |
| Siano と PX4 を同一バイナリへ静的リンクし、外部 symbol を衝突させない | §6.2 の製品名・リリース識別付きファクトリー | 方向は反映。具体的な symbol と関数表は未決 |
| 共通の型・関数表 | §6.2 の API 一覧 | 名称・レイアウトは未決 |
| 同期の読み書き＋連続 IN の出しっぱなし読み | §6.2 は `submit`/`completion`/`cancel` を基本とし、同期橋を許容 | 差異あり。同期コールバックと `stream_*` 相当の要望は未決 |
| スレッドを作らない。必要なら数と停止方法を明記 | §6.2「専用スレッドを一律必須にしない」 | 方向は反映。保証範囲は未決 |
| 機材ごとの LNB on/off、非対応は `UNSUPPORTED` | §6.2 の LNB 系、§6.1 | 反映 |
| tuner と同じライブラリで CARD を扱う | §6.2 の CARD 系 | 反映 |
| 複数 USB 筐体/receiver、筐体単位の grouping | §6.2 の catalog/discover/open | 方向は反映。`enclosure_key`/`part_count` 等の具体は未決 |
| 抜け時は GONE、close はその後も呼べる | §6.2 の状態・切断の扱い | 反映 |
| ファームウェアをバイト列で渡し、形式・機種・SHA-256 をライブラリで検証 | §6.2 のファームウェア | 反映 |
| 同一 EP で `bulk_read` と `stream_*` を同時に使わない | 現案に明記なし | 未対応（提案） |
| `usb_string` による文字列記述子の読み出し | 現案に明記なし | 未対応（提案） |
| 戻り値は 0 以上が成功、負がエラー | §6.2 の結果・Error | 具体 enum は未決 |

これらは公開契約の採用決定ではなく、次回の設計で扱う入力である。

### 6.4 設計上の未決

- 案 C の構成は採用済み。個別 API の symbol、型、enum 採番、構造体レイアウト、期限・容量・最大同時数の実測値は未決。
- L3 を最初に対象とする製品と順序（設計案は PIX DT295 を先発候補とする）。
- バッファ借用期間、キャンセル後の遅延完了、クローズ期限切れ時の資源保持/解放の詳細。
- **B1（設計補足を統合済み。採用未決）**: クローズ期限切れまたは失敗 open の後、遅延完了を回収してから残留資源を観測・最終解放する手順。`close_status` の再照会と `context_release` の一回限り解放を提案している。
- **B2（設計補足を統合済み。採用未決）**: 同期 USB を待つワーカーが塞がっても、キャンセルとクローズ期限通知を独立して進める担当関係。driver レーンと USB ポンプ / control-期限 / クリーンアップを分ける。
- B1/B2 の関数名・スナップショット形式・進行担当の配置は設計案であり、採用済み API ではない。
- L3 の契約テストをどの層で回すか。
- リリースごとの互換保証の表現（ソース固定の範囲）。

### 6.5 ライセンス境界

本節は、どの部分がどのライセンス表記に属するかを示す。来歴の調査経過、交渉、採否の経緯、法的判断は本書の対象外とする。

| 部分 | 現状の表記 | 状態 |
|---|---|---|
| Core `src/`、`include/dtv/userland/` | MIT | 現行表記 |
| Core `adapters/` の host 系 | GPL-2.0-only | 現行表記 |
| 新規 L3 契約ヘッダー | MIT 提案 | 未決。採用・許諾済みではない |
| 製品 driver（PX4/ASICEN/Torne） | GPL-2.0-only | 現行表記 |
| Siano 製品本体 | GPL-2.0-or-later | 現行表記 |
| PIX C 層・DT295 アダプター | GPL-2.0-or-later | 現行表記 |
| Core host `Family`、デーモン、CARD T=1 移植 | L4/L1 側 | L3 の必須リンクから外す |

- リリースごとに、各対象がリンクするソースとライセンス表記の組を列挙する。
- ライセンス表示の変更、権利者への照会、法的判断は本書では行わない。未確認の許諾を許諾済みとして扱わない。
- PX4/Torne/ASICEN の組み込みは、当面プロセス境界（デーモン IPC）を現実的な既定とする。

---

## 7. 未決一覧と推奨

| # | 未決 | 選択肢 | 推奨 |
|---:|---|---|---|
| 1 | L2 wire のリリース間互換の具体範囲 | (a) マイナーは機能でゲートし、レイアウト変更は明示的な機能/版で区別 (b) 同一メジャー内はレイアウト固定 | L2 の外部化は採用済み。互換範囲だけを狭く決める。(a) を推奨 |
| 2 | L3 のサポート境界 | (a) 新しい C 面＋検査付き入口 (b) 既存 Family を薄く包む (c) 生バックエンドまで | (a)。設計案の推奨。生バックエンドは L4 内部 |
| 3 | L3 の I/O モデル | 案 A/B/C（§6.2） | 案 C の構成を採用済み。個別 API の詳細は未決 |
| 4 | L3 の対応製品と順序 | PIX DT295 先発 / PX4 先発 | PIX DT295（C 層の現行表記が GPL-2.0-or-later）を先発候補 |
| 4b | L3 の API 詳細 | symbol・型・enum 採番・レイアウト・期限・容量 | 設計案のまま。実測後にリリースごとに凍結 |
| 5 | リリースごとのリンク境界の確定 | 各対象のソースとライセンス表記を列挙 | 新規 L3 ヘッダーは独立に扱う。個別対象の確認結果は本書に書かず、リリース時の記録で扱う |
| 6 | `--models`/`--list-json` のメタデータ優先 | ヘルプ/バージョンと同じ単独要求の優先 / 通常解析 | 単独要求として同じ優先を与える |
| 7 | `--verbose`/`-v` の共通必須化 | 全製品必須 / 既存のみ | 全製品必須にしない。既存を維持し、必要なら別決定 |
| 8 | `--pid`/`-p` の共通化 | 全製品 / 必要製品のみ | 必要製品のみ。PID フィルターは能力 |
| 9 | OS 非対応オプションの共通応答 | unknown/2 / unsupported/3 | 既存応答を保持し、新たに 3 へ変えない |
| 10 | `--tune-timeout-ms` の範囲・既定 | 草案候補 100..120000、既定 30000 | 実装前に凍結。実機で妥当性を確認 |
| 11 | `--packet-count` の共通提供 | 全製品 / 実装可能な製品のみ | 全製品で無理なく実装できる場合だけ提供 |
| 12 | 全体クローズの期限 | 10 秒案 / 15 秒（Siano 既存） | 有限期限を採用済み。秒数は実機で決定 |
| 13 | runtime 省略時の共通解決 | 別 PR 設計 | 明示 `--runtime-dir` を先に確定し、既定解決は別設計 |
| 14 | （採用済み）Torne の models/ネイティブ device/Windows EOF/外部ファームウェア | — | 既決の対象。実装・検証の未完とは分ける。詳細は付録 E |
| 15 | JSON `userland.status` のスキーマ名の文書化 | Core docs に明記 / 実装のみ | Core docs に明記する（現状 docs に literal がない） |
| 16 | exit の Error→code 細部 | 採用済み分類 / 個別表 | 分類は採用済み。細部は実装と照合して別途確定 |
| 17 | B1/B2 の設計補足 | 提案どおり採用 / 修正 | 設計補足（46c6ec2b）を統合済み。`close_status`/`context_release` と進行担当の分離は採用未決 |

---

## 8. 参照

- エンベロープ・フレーム: [`wire-subset.md`](wire-subset.md)
- 型付きレコード・STATUS/LIST・validation: [`wire-records.md`](wire-records.md)
- 接続ゲート: [`control-connection.md`](control-connection.md)
- オーソリティ: [`control-authority.md`](control-authority.md)
- クライアント: [`control-client.md`](control-client.md)
- runtime コネクター: [`control-client-runtime.md`](control-client-runtime.md)
- ストリームライフサイクル: [`receiver-v1-stream.md`](receiver-v1-stream.md)
- discovery JSON: [`json-discovery.md`](json-discovery.md)
- status JSON: [`json-status.md`](json-status.md)
- identity コーデック: [`identity.md`](identity.md)
- 選局リゾルバー: [`tuning.md`](tuning.md)
- Windows の stdin 監視: [`windows-stdin-monitor.md`](windows-stdin-monitor.md)
- host アダプターの境界: [`host-tuner.md`](host-tuner.md)、[`host-installed-headers.md`](host-installed-headers.md)
