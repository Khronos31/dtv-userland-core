# DTV userland 1.0.0 外部契約（案）

状態: **承認前の案。未レビュー・未merge。** 本書は 1.0.0 の外部契約を一本にまとめた候補であり、採用済みの仕様ではない。本文中の「採用済み」はユーザー決定記録に明示の根拠がある項目だけを指し、「未決」は判断が残る項目を指す。実装・ライセンス表示・版・tag・Release は本書で変更しない。

- 対象: PX4 / ASICEN / Siano / PIX / Torne の 1.0.0
- 規範材料: ユーザー決定記録、Core の `docs/`、全5製品 CLI 共通契約の §0（確定 target 表）に限る。
- 規範にしないもの: 現行製品実装、旧 draft の数値、既存利用者の呼び出し方。これらは移行メモと点検にだけ使う。
- 製品ごとの能力差は各製品 SPEC の付録案として分離する。本書は共通規則だけを持つ。
- L3（埋め込み）の具体 API は別途 architecture 設計中であり、本書は確定済みの約束と未決だけを記す。関数名・ABI・例外型を発明して確定扱いにしない。

## 0. 読み方: 6つの観点との対応

| 依頼観点 | 本書の節 |
|---|---|
| argv の共通項と製品別追加・除外 | §3 L1、製品 SPEC 付録 |
| ライブラリとして静的リンクする際の API | §6 L3（設計中） |
| JSON の書式 | §4.8、[json-discovery](json-discovery.md)、[json-status](json-status.md) |
| wire IPC | §4、[wire-subset](wire-subset.md)、[wire-records](wire-records.md) |
| runtime / FD の受け渡し | §5、[control-client-runtime](control-client-runtime.md)、[identity](identity.md) |
| その他（終了・signal・所有・cancel/close・ライセンス） | §3.7–3.8、§4.6、§6.4、§7 |

## 1. 層モデル（採用済み）

利用者の層ごとに契約を一つだけ割り当てる。同じ不変条件を、上の層は下の層と同じコードで継承する。

| 層 | 対象 | 主な利用者 | 約束 |
|---|---|---|---|
| L1 プロセス | argv、stdout の TS、終了コード、signal・EOF、FD の受け渡し、runtime dir | mirakc、dtv-android、launcher | 1.0.0 の規範。major で固定 |
| L2 wire と JSON | IPC record・opcode、discovery・status JSON | denpa の C# 実装、ctl | 規範。version 交渉付き（§4.5 の未決を含む） |
| L3 埋め込み | 製品間で同じ形の C header、各関数の意味（受付判定、gate、Error、所有、cancel・close の期限） | WebTS、denpa（in-process） | release ごとに固定する source 契約。安定 ABI は約束しない |
| L4 内部 | Family、TunerServiceBackend、Core host adapter、`detail/`、製品内部 class | 製品自身のみ | 契約ではない。予告なく変えてよい |

- 層分けは採用済み。L3 の具体契約は設計対象であり、この採用だけで API を確定しない。
- L3 の約束は **release ごとの固定** とする。release をまたぐ無期限の安定 ABI を新たに約束しない。
- L4 から L3 への昇格はユーザー判断とする。WebTS が現在使っている Family 等は L4 にあたる。
- `include/dtv/userland/detail/` は L4 の印として扱う。
- 契約テストは層ごとに作り、どの層の規範を検査しているかを明記する。

## 2. 共通の前提

- 製品は 5 つ。公開コマンドは製品ごとに daemon（`*d`）、通常 TS client（`*-ts`）、ctl（`*ctl`）、および配布 launcher/wrapper。
- 通常 TS client は **すべて daemon IPC 経由**とする。Torne の `--device` による直接 USB 経路は廃止し、共通の daemon 接続・物理 selector/assertion として扱う（採用済み）。
- 同じ option 名に製品ごとの別の意味を割り当てない。ハードにない機能は黙って無視せず、明示的な unsupported として返す。
- すべての入口が同じ規則に到達することを目的とする。同じコードを使うことはその手段の一つにすぎない。

---

## 3. L1: プロセス契約

### 3.1 コマンドと役割

| 役割 | コマンド | 責務 |
|---|---|---|
| daemon | `px4d` / `asicend` / `sianod` / `pixd` / `torned` | USB 筐体の所有、firmware、選局、capture、card、IPC の公開 |
| TS client | `px4-ts` / `asicen-ts` / `siano-ts` / `pix-ts` / `torne-ts` | daemon へ接続し、選局・受信し、raw TS を stdout または `--output` へ書く |
| ctl | `px4ctl` / `asicenctl` / `sianoctl` / `pixctl` / `tornectl` | `list` / `status` / CARD / LNB など、能力に応じた制御と状態参照 |
| launcher | 各配布 wrapper（例 `px4-termux`、`pix-termux`、`torne-termux`） | 権限・FD・runtime の準備、daemon 起動、終了時の後始末 |

daemon は筐体ごとに 1 プロセスを基本とする。ctl の `list` は daemon の `--list` とは別物であり、廃止対象ではない。

### 3.2 共通 argv

以下は全製品で同じ名前・同じ意味を持つ target。`D`=daemon、`T`=通常 TS client、`C`=ctl の既存 root option。

| option / 短名 | 役割 | 値・単位 | 状態 |
|---|---|---|---|
| `--help` / `-h` | D/T/C/launcher | なし | 採用済み。他の引数にかかわらず help を表示し正常終了（§3.3） |
| `--version` | D/T/C/launcher | なし | 採用済み。help がなければ version を表示し正常終了。`-V` は廃止（§3.5） |
| `--firmware` | D | FILE path | 採用済み。全 daemon で明示 path 必須。`-F` は廃止 |
| `--device` | D/T/C | `serial:TEXT` または `usb:BUS-PORT[,BUS-PORT...]` | 採用済みの共通 selector。daemon の `-d` は個別判断（未決） |
| `--instance` | D/T/C | token | 採用済み。入力 case を保持し、`RoomA` と `rooma` は別 |
| `--runtime-dir` | D/T/C | path | 採用済み。明示値優先、不正なら無言 fallback しない。省略時の共通解決は別設計（未決） |
| `--group` | D/C と endpoint を開く T | なし | 採用済み。POSIX group と Windows ACL を区別。OS 非対応時の応答は未決 |
| `--fd` | D、launcher 内部 handoff | decimal FD | 採用済み。borrowed USB descriptor。数・値範囲は能力差（§5） |
| `--exit-on-stdin-eof` | D | なし | 採用済み。Windows 限定・opt-in・既定 off。POSIX は現行どおり未知引数/2 |
| `--models` | D | なし | 採用済み。接続を試さず、対応機種名＋VID:PID の静的 plain text |
| `--list-json` | D | なし | 採用済み。接続中 discovery の JSON。`--list` は廃止 |
| `--freq` / `-f` | T | Hz unsigned integer | 採用済み。`--channel` と排他 |
| `--channel` / `-c` | T | channel text | 採用済み。mirakc template へ渡せる text。`--freq` と排他 |
| `--system` / `-s` | T | `isdb-t` / `isdb-s` | 採用済み。ASCII case-insensitive。channel から導出できる場合は省略可 |
| `--receiver` | T | unsigned receiver index | 採用済み。全 TS client で明示必須。省略時 0 へ暗黙選択しない |
| `--slot` | T | unsigned satellite slot | 採用済み。`--tsid` と排他 |
| `--tsid` | T | unsigned satellite TSID | 採用済み。`--stream-id` の共通正規名。`--slot` と排他 |
| `--time` / `-t` | T | seconds integer、または `-` | 採用済み。`-` または省略は無期限 |
| `--packet-count` | T | positive packet count | 条件付き方針。無理なく共通化できる場合だけ提供（未決を含む） |
| `--tune-timeout-ms` | T | milliseconds | 採用済み。単位 ms を長名に明記。範囲・既定は実装前に凍結 |
| `--output` / `-o` | T | path または `-` | 採用済み。`-` は全製品 stdout |
| `--allow-lnb-power` | D | なし | 採用済み。P/A などの ISDB-S 15V 要求の許可。PIX `--lna` と混同しない |
| `--verbose` / `-v` | D | なし | 一部製品の既存 option。共通必須かは未決 |
| `--pid` / `-p` | D | PID filter | 条件付き方針。全製品必須ではない |
| `--control` | T（Siano 等） | なし | 製品固有の常駐制御。共通再選局 API へ統合する設計対象（未決） |
| `--lna` | D（PIX） | なし | 当面 PIX のみ。他製品の扱いは未決 |

製品ごとの追加・除外は各製品 SPEC の付録案に記す。ここにない option を新設して同じ意味を持たせない。

### 3.3 help / version / metadata の優先

採用済みの規則:

1. help/version は、他の引数にどんな不正があっても metadata を表示して **正常終了**する。
2. help と version が併存したら **help** を表示して正常終了する。
3. metadata の解決は、通常解析・接続・USB discovery・permission 取得より前に行う。
4. 廃止短名 `-F`/`-V` は、metadata を要求していない通常解析では未知引数/2 のまま。metadata 優先は通常解析の引数エラー判定に先行する。

`--models` / `--list-json` を metadata と同じ「単独要求」として扱うかは製品差があり、§7 の未決に置く。

### 3.4 必須入力

- 全 daemon: `--firmware` の明示 path が必須。
- 全 TS client: `--receiver` の明示が必須。
- 通常 TS client: `--channel` か `--freq` のどちらか一方が必須。
- `--freq` 経路では `--system` が必須。`--channel` から system を導出できる場合は省略できる。
- 明示必須を、製品固有の既定値や「接続機器が 1 台なら省略可」で置き換えない。

### 3.5 短名・重複・排他

- 廃止短名: `-F`（`--firmware`）と `-V`（`--version`）は全製品で `UNKNOWN ARGUMENT`/2。help に掲示しない。従来の互換維持方針はこの後続決定で上書きされた。
- 維持する短名: `-h`、`-f`、`-c`、`-o`、`-t`、`-s`（採用済みのもの）。それ以外の短名は個別決定がなければ発明しない。
- 単一値 option の重複は引数エラー/2。長短名の混在も同じ。例: `--firmware FILE_A --firmware FILE_B`、`-f 1 --freq 2`。
- 繰返しが仕様の入力（`--fd`、PID 等）は単一値 option と区別する。重複判定は繰返し仕様の内側で行う。
- 排他: `--freq` と `--channel`、`--slot` と `--tsid`、`--time` と `--packet-count`（後者は条件付き）、地上選局と `--slot`/`--tsid`。
- instance の case は保持する。`RoomA` と `rooma` は別 instance。旧 lowercase 正規化案を現在の規範へ再採用しない。

### 3.6 stdout / stderr

- TS 出力は raw TS のみ。`--output -` は stdout。診断・進捗は stderr。
- `--list-json` は 1 つの JSON document のみを stdout へ出す。前後に説明文を混ぜない。
- help/version は stdout。
- 成功時の counter 診断や失敗理由は stderr。stdout に混ぜない。
- `--models` は plain text（非 JSON・非 CSV）。

### 3.7 終了コード

共通分類 `0, 2..10, 70` を採用済み。数値を IPC error enum と混同しない。

| exit | 分類 | 主な対応 |
|---:|---|---|
| 0 | 成功 / 要求された停止 | 正常完了、time/packet 上限、opt-in stdin EOF、SIGINT/SIGTERM の通常 cleanup 完了 |
| 2 | 入力不正 | argv 構文・範囲・相互排他・単一値重複、`INVALID_ARGUMENT` |
| 3 | 対象/機能なし、状態不整合 | `NOT_FOUND` / `NOT_READY` / `UNSUPPORTED` / `INVALID_STATE`。曖昧な device 選択も 3 |
| 4 | 競合 | `BUSY`（別 owner、回収中、同時 stream 制限） |
| 5 | 期限切れ | `TIMEOUT`（tune、IPC 応答待ち、停止回収） |
| 6 | IPC 不整合 | `VERSION_MISMATCH` / `PROTOCOL_ERROR`、壊れた frame、不整合な final |
| 7 | USB 喪失 / 入出力 | `USB_IO` / `DISCONNECTED` |
| 8 | TS 継続不能 | `SLOW_CONSUMER`、backend が終端エラーとして報告した TS 破損。単なる CC/drop 増加は含めない |
| 9 | カード操作失敗 | `NO_CARD` / `CARD_REMOVED` / card API の `BUFFER_TOO_SMALL`。カード非対応製品の CARD 要求は `UNSUPPORTED`→3 |
| 10 | 起動用データ不正 | `FIRMWARE_REJECTED`、必要な firmware/assets の欠落・不整合 |
| 70 | ローカル / 内部失敗 | 出力 open/write/EPIPE/ENOSPC、`ACCESS_DENIED`、`RESOURCE_LIMIT`、`INTERNAL`、未知の非 0 error |

- 運転中に最初に確定した非回復エラーを primary に固定する。要求停止・少量 drop は primary を作らない。
- 要求停止・time/packet 到達後でも、既に確認した USB/output/terminal error を消さない。
- 信頼できる正常 final が取れず、STOP も静定確認できない場合は成功にしない。
- 少量 drop だけで受信を失敗させない。drop 後の正常終了は 0 とする。
- 個々の Error → exit の細部は採用済み記録・実装と照合し、本書の表を新たな数値決定として扱わない（§7）。

### 3.8 signal / EOF

- SIGINT/SIGTERM は通常 cleanup を経て 0。cleanup 失敗時は分類に従う。
- `--exit-on-stdin-eof` は **Windows 限定**、opt-in、既定 off。指定時は stdin pipe の EOF から通常 cleanup へ進む。POSIX では現行どおり未知引数/2 を維持し、Core の汎用設計を理由に全 OS 対応へ変更しない。
- 全体終了が有限予算を超えて静定しない場合は異常終了。秒数・OS primitive は実装/試験事項。

### 3.9 channel / Hz

- `--freq` は符号なし 10 進 Hz、1..2^53−1（JSON 整数との共通上限）。入力 Hz を保持し、backend だけが必要な単位へ量子化する。無言で別周波数へ丸めない。
- `--channel` は mirakc の channel text をそのまま渡せる文字列。共通で受理する範囲:
  - 地上: `13`..`62`、`T13`..`T62`
  - BS: 奇数 `BS01`..`BS23`、任意で `_0`..`_11`（zero padding なし）
  - CS: 偶数 `CS2`..`CS24`（`CS02`..`CS08` も受理）
- 地上 channel は自動選択で slot/TSID を拒否。suffix なし BS は slot か TSID が必須。suffix あり BS は suffix slot を使い、追加の slot/TSID を拒否。CS は既定 slot 0 で、明示 slot/TSID が置換する。衛星 freq は明示 slot/TSID が必須。slot 0..11、TSID 1..65534。
- system は `isdb-t` / `isdb-s` の ASCII case-insensitive。channel との併記は一致時のみ受理。
- parser が受理する範囲と機器能力を分ける。未対応帯域・方式・slot は `UNSUPPORTED`/3 とし、USB 操作前に拒否する。

---

## 4. L2: wire と JSON

L2 の規範は Core の次の文書に置く。本書は入口と横断規則だけをまとめ、record の全 field を複製しない。

- 封筒・frame・HELLO/ACQUIRE の先頭 subset: [`wire-subset.md`](wire-subset.md)
- typed records と STATUS/LIST: [`wire-records.md`](wire-records.md)
- connection gate: [`control-connection.md`](control-connection.md)
- authority / session: [`control-authority.md`](control-authority.md)
- read-only / recording client: [`control-client.md`](control-client.md)
- stream lifecycle: [`receiver-v1-stream.md`](receiver-v1-stream.md)
- discovery JSON: [`json-discovery.md`](json-discovery.md)
- status JSON: [`json-status.md`](json-status.md)

### 4.1 transport と endpoint

- POSIX/Android は Unix domain socket。Windows は named pipe/ACL 相当（Core host runtime が吸収）。
- endpoint path は `<runtime>/<product-slug>/v1/<instance>/<endpoint>`。`endpoint` は `control.sock` または `stream.sock`。product slug は `px4-userland` 等。
- 接続の所有は呼び出し側。Core library は socket を生成しない（host runtime が adapter として提供）。
- endpoint の owner/mode・peer UID（POSIX）、owner SID・protected DACL（Windows）を検証する。shared group は POSIX のみ。

### 4.2 封筒

- magic は ASCII `UDTV`。legacy `PX4U` / `ASCN` は拒否し、payload を解釈しない。
- 20 byte 固定 header。すべて little-endian。
- major=1、minor=0。他の版は `VERSION_MISMATCH`。
- opcode は raw u16。未知 opcode も封筒としては受理し、dispatcher が `UNSUPPORTED` を返す。
- flags は 0/1/3 のみ。それ以外は malformed。
- kind の組合せ: request(bit15=0, flags=0, id≠0)、response(0,1,id≠0)、error_response(0,3,id≠0)、event(1,0,id=0)。他は `PROTOCOL_ERROR`。
- payload_length は payload を読む前に上限検査する。
- 上限: TS_DATA event は 1,048,504 byte（1,048,476 + 28）。その他は 65,536 byte。
- `FrameReceiver` は断片・連結 frame を再構成し、不完全 frame は 5 秒で破棄する（OS adapter が期限を強制）。EOF が frame 途中なら `PROTOCOL_ERROR` で再接続しない。

### 4.3 接続状態

- 最初の frame は HELLO。HELLO で版と feature を交渉する。
- 1 接続で同時に 1 要求。request_id は単調増加し、再利用・過去 ID の再使用を拒否する。
- 応答は opcode と request_id が一致する response/error_response のみ受理する。
- HELLO/LIST/STATUS の後、ACQUIRE→TUNE→START→ATTACH→TS_DATA→STOP→RELEASE の順。
- epoch は同一 release 内の識別子。LIST と STATUS の epoch を照合する。
- 不足 feature は `UNSUPPORTED`。未知 feature bit は運搬し、codec は判定しない。

### 4.4 payload

- typed payload は exact size。size 不一致は `PROTOCOL_ERROR`、size 正当で意味不正は `INVALID_ARGUMENT`。
- 文字列は `u16 length + UTF-8`、bytes は `u32 length + bytes`、配列は `u16 count`。上限は record ごと。
- 整数は固定幅 little-endian。JSON では 64 bit を decimal string で保持する。
- Model は wire/JSON とも **name のみ**（`str[128]` / string or null）。id は持たない。
- lease/stream/card handle/generation の ID は非 0。`overflow=true` は `value==UINT64_MAX`。
- `frequency_hz` と `programmed_hz` は正で 2^53−1 以下。profile 範囲は codec の外で検査する。
- STATUS の enclosure 観測（`lnb_15v_enabled` / `card_inserted`）は「不明」と「観測 false」を区別する。`card_inserted` は enclosure 内部観測で、reader 集計ではない。
- 未知の optional counter は検証して投影では飛ばし、既知 field 名へ alias しない。

### 4.5 版・互換

- 封筒版は UDTV 1.0。HELLO の min/max で 1.0 を選ぶ。それ以外は `VERSION_MISMATCH`。
- **未決:** Core の現行文書は「daemon と ctl の UDTV は同一 release 内の内部契約。release をまたぐ安定 wire API ではない」と記す。一方、本 1.0.0 案は denpa の C# 実装のような第三者 client を含む L2 外部契約として wire を位置づける。両者は両立しない。
  - 推奨: L2 を「同一 major 内は version 交渉で接続可、minor 差は feature で判定」と外部化し、release をまたぐ互換を minor の範囲で約束する。ただし「安定 ABI を約束しない」L3 と混同しない。
  - 代替: wire を内部のまま維持し、第三者 client には daemon プロセス境界だけを公開する。この場合 denpa の C# 実装はサポート外となる。
- STATUS の enclosure 観測 field 追加のような layout 変更は、minor ではなく同一 release 前提の非互換変更として扱うか、feature bit で gate するかを決める必要がある（未決）。

### 4.6 所有・cancel・close

- ACQUIRE は lease を返す。lease は接続が閉じると daemon が回収する。
- START は tune revision と attach token を返す。token は data 接続の ATTACH にだけ使う。
- STOP は terminal result と cleanup owner を返す。`cleanup_owner=true` のときだけ物理 cleanup を実行する。`complete_stream_cleanup_v1()` は metadata のみで、物理 cleanup を代行しない。
- 停止・close は有限期限を持つ。孤立 pending を成功として扱わない。全体 close の秒数は候補値であり、実機認定ではない。
- error が付いた final は分類に従う。要求停止に結び付かない `owner_closed`/`CANCELLED` は 70。
- drop は計数するが失敗ではない。

### 4.7 error

- error_response は非 0 code と detail を持つ。code は Core `Error` enum の意味に従う。
- Core enum 外の code は JSON で `code:"UNKNOWN"` + `numeric_code`。
- 全入口で同じ Error 分類に到達することを目標とする。adapter が BUSY/NOT_FOUND/ACCESS 等の区別を潰さない。
- 具体的な wire error payload の byte layout は [`wire-records.md`](wire-records.md) を正とする。

### 4.8 JSON

公開 JSON は 2 種。

- `userland.discovery`: 接続中 scan。`schema` / `producer` / `scan` / `enclosures` / `ungrouped_usb_devices`。物理階層 `enclosures[].devices[].receivers[]` を保つ。ID は JSON number、VID:PID は小文字 `VID:PID`。
- `userland.status`: 検証済み `StatusResponse` の投影。`schema` / `producer` / `context` / `snapshot` / `scope` / `enclosure` / `topology` と、存在する collection だけを出す。

横断規則:

- 64 bit 値（epoch の hex 以外の sequence/observed_at_ns/stream id/revision）は decimal string。
- 欠測・不明は `null`。観測 false と区別する。
- enum は独自語彙を作らず、既約の読みやすい文字列（`ready`、`ISDB-T`、`isdb-t` 等）を使う。
- `model` は name のみ。`id` は出さない。
- ID の寿命: epoch は同一 release の接続識別子、function/receiver/reader ID は snapshot 内、lease/stream/card handle は操作単位。scan の index を永続 ID として転用しない。
- writer は validate→measure→write。失敗時に出力 buffer を変更しない。文字列は JSON escape し、有効 UTF-8 を保つ。
- 1 回の出力は 1 JSON document。末尾 newline の有無は各既存 API の契約に従う。

---

## 5. runtime と FD の受け渡し

### 5.1 runtime

- `--runtime-dir` は全製品で提供する。明示値が不正なら無言 fallback しない。
- 省略時の共通解決は別 PR の設計対象であり、本書では確定しない（未決）。
- 解決規則（Core host runtime の既存設計）:
  - POSIX: 絶対 `XDG_RUNTIME_DIR` → `$TMPDIR/dtv-userland-<euid>` → `/tmp/dtv-userland-<euid>`。相対・空・長すぎる・dot component は除外。明示 root は単独使用。
  - 既存候補は owner と mode を検証し、安全でないものは変更せず次へ進む。
  - Windows: `LOCALAPPDATA` 配下。owner SID と DACL を検証。
- endpoint は `<runtime>/<product>/v1/<instance>/<endpoint>`。instance の case は保持する。
- daemon は runtime 配下の自分が必要とする階層だけを作る。呼び出し側が root を用意する契約の製品では、その境界を付録に記す。

### 5.2 FD

- `--fd` は POSIX/Android の daemon 起動用 USB descriptor。TS 出力先 FD/PID とは別。
- 10 進 3..INT_MAX。符号・空白・16 進・8 進解釈・positional FD は廃止。`03` は値 3 として扱い 8 進にしない。
- 繰返し可。同じ数値の重複は 2。上限 8（wire の function 上限）。
- fd 集合が open 対象を限定する。USB namespace を勝手に再列挙して不足 function を開かない。`--device serial:...`/`usb:...` は渡された集合への assertion としてだけ使う。
- 不一致・別筐体混在・同じ function を異なる fd で二重指定は 3。閉じた fd・USB でない fd は 2。検査 I/O 失敗は 7。
- 引数順を receiver/function 番号と同一視しない。USB metadata と profile で role を照合する。
- driver は内部 duplicate を作り CLOEXEC で保持し、その duplicate だけを wrap/close する。部分失敗時は作成済み duplicate を回収し、元 fd は close しない。
- 元 FD の所有は host。host は context の `quiesced=true` 完了、または USB FD を保持し得る全 process の終了/回収確認まで元 connection を close しない。
- Windows では数値 POSIX fd を HANDLE へキャストせず `UNSUPPORTED`/3。
- cold→warm の再列挙で fd は自動で新 device の fd にならない。権限/新 fd は host との明示 handoff が必要。

### 5.3 instance / identity

- `serial:<string>` は非空 UTF-8、最大 512 byte、NUL/制御文字なし。trim・percent-decode・正規化・case fold をしない。
- `usb:<path>[,<path>...]` は 1..8 個の `bus-port[.port...]`。正の ASCII 10 進、leading zero なし、各 64 byte 以下。重複は構文段階で拒否し、黙って dedupe しない。順序は path 順で、入力 function role 番号ではない。
- `canonicalize_instance` は 1..64 ASCII、先頭 alphanumeric、残り alphanumeric/`_`/`.`/`-`。case を保持。末尾 dot と Windows 予約 stem（`con`/`prn`/`aux`/`nul`/`com1..9`/`lpt1..9`、case-insensitive）を拒否。
- 構文検査は筐体 grouping や OS 上の存在を証明しない。
- selector と instance を混同しない。device は物理筐体、instance は接続先。

---

## 6. L3: 埋め込み契約（設計中）

**この節は確定 API ではない。** 採用済みの約束と、architecture 設計へ渡す境界だけを記す。

### 6.1 採用済み

- 層モデルを採用し、L3 を埋め込み境界とする。
- L3 の約束は release ごとに固定する。release をまたぐ安定 ABI を新たに約束しない。
- L3 の header は MIT を目標とする（ユーザーのライセンス選好: GPL-2.0-or-later 希望、合理的なら MIT も可）。ただし §6.4 の来歴未解決があり、MIT と表示できる状態ではない。
- L3 の関数の意味として、少なくとも次を共有する: 入力の単位・範囲の受付判定、identity の文法と case、Error enum の意味、資源所有（FD/buffer/lease）と排他、状態遷移（open→tune→capture→stop→close）、cancel/close の有限期限、drop の非失敗扱い。
- サポートする埋め込み境界を「Family＋検査付き入口」に限るか、生 `tuner_backend()` まで含めるかは **未決**（推奨: 前者。生 backend は L4 内部扱い）。
- 具体的な関数名・型・ABI・例外は architecture 設計の成果を待つ。本書で発明しない。

### 6.2 利用者から出ている要件（入力であり、採用決定ではない）

- WebTS: PX4 のみを現在使用。Family ではなく部品を自前で組み立てている。非同期の submit/completion/cancel を基本にしたいが、worker 上の同期 facade でも受け入れる。main と worker の相互待ちを作らないこと、buffer を借りる期間、cancel 後に遅れて来る completion、close 期限切れ時の資源扱いを明記してほしい。内部で thread を作ることは要求しない。card+B25 と PIX の機器内復元を同じ API に押し込まないでほしい。
- denpa: in-process 利用を希望（AGPL-3.0-or-later のプロセス内）。USB 転送は呼び出し側が渡す形を想定。
- 既存の Family/`EnclosureFamily` は GPL-2.0-only の adapter に依存するため、全利用者の共通境界にはできない。L3 は MIT 側の受付判定・Error・engine と、製品 adapter（呼び出し側が渡す I/O）＋製品間で同じ形の C header で構成する案が有力（未決）。

### 6.3 設計上の未決

- I/O モデル: 非同期 submit/completion/cancel を第一とするか、同期 facade を第一とするか。
- 対応製品と順序（PIX DT295 を最初とする案）。
- buffer 借用期間、cancel 後の遅延 completion、close 期限切れ時の resource 保持/解放。
- L3 の契約テストをどの層で回すか。
- 安定性の表現: 「release ごとの source 固定」の具体的な互換保証範囲。

### 6.4 ライセンス来歴（未決、法的判断ではない）

- Core の `adapters/` の host 系は GPL-2.0-only、`src/` と `include/dtv/userland/` は MIT。
- ただし MIT 側の `src/receiver_engine.cpp` に、第三者（siketyan）が px4-userland へ出した GPL-2.0-only の PR とほぼ同じ文面が約 15 行ある。本人から MIT で出す許可を得た記録は見当たらない。
- そのため「Core の MIT 側なら AGPL の利用者と組める」という前提は、この行を処理するまで成り立たない。L3 の header を MIT と表示することも、この確認まで確定しない。
- 選択肢:
  1. 本人の許可を得る。
  2. 約 15 行を独立に書き直し、由来を記録する。
  3. L3 をユーザーしか書いていない Core MIT 部品と PIX C 層（すでに or-later）だけで構成する。
  4. PX4/Torne/ASICEN の組み込みはプロセス境界（daemon IPC）に限る。
- 推奨: 1 または 2 を先に片付け、3 を進め、4 を現実的な既定とする。ライセンス変更を許諾済みとして扱わない。
- 関連する未確認: GPLv3/AGPLv3 本文の該当条項、第三者の同意範囲、firmware の再配布権。

---

## 7. 未決一覧と推奨

| # | 未決 | 選択肢 | 推奨 |
|---:|---|---|---|
| 1 | L2 wire を外部契約にするか内部のままにするか | (a) 外部化し version 交渉で minor 互換を約束 (b) 内部維持し daemon 境界だけ公開 | (a)。denpa の C# client を明示的にサポートするなら必要 |
| 2 | L3 のサポート境界 | (a) Family＋検査付き入口 (b) 生 backend まで | (a)。生 backend は L4 内部 |
| 3 | L3 の I/O モデル | 非同期 submit/completion/cancel / 同期 facade | 非同期を第一、同期 facade を許容。WebTS 要件に合わせる |
| 4 | L3 の対応製品と順序 | PIX DT295 先発 / PX4 先発 | PIX DT295（C 層がすでに or-later） |
| 5 | Core MIT の siketyan 行 | 許可 / 書き直し / L3 を user-only MIT で構成 | 許可または書き直しを先に。並行して user-only 構成 |
| 6 | `--models`/`--list-json` の metadata 優先 | help/version と同じ単独特権 / 通常解析 | 単独要求として同じ優先を与える |
| 7 | `--verbose`/`-v` の共通必須化 | 全製品必須 / 既存のみ | 全製品必須にしない。既存を維持し、必要なら別決定 |
| 8 | `--pid`/`-p` の共通化 | 全製品 / 必要製品のみ | 必要製品のみ。PID filter は capability |
| 9 | OS 非対応 option の共通応答 | unknown/2 / unsupported/3 | 既存応答を保持し、新たに 3 へ変えない |
| 10 | `--tune-timeout-ms` の範囲・既定 | draft 候補 100..120000、既定 30000 | 実装前に凍結。実機で妥当性を確認 |
| 11 | `--packet-count` の共通提供 | 全製品 / 実装可能な製品のみ | 全製品で無理なく実装できる場合だけ提供 |
| 12 | 全体 close の期限 | 10 秒案 / 15 秒（Siano 既存） | 有限期限を採用済み。秒数は実機で決定 |
| 13 | runtime 省略時の共通解決 | 別 PR 設計 | 明示 `--runtime-dir` を先に確定し、既定解決は別設計 |
| 14 | Torne の `--models`/`--device`/EOF | 先方担当 PR / 本書で確定 | 先方担当の設計に委ね、本書は共通規則だけ持つ |
| 15 | JSON `userland.status` の schema 名の文書化 | Core docs に明記 / 実装のみ | Core docs に明記する（現状 docs に literal がない） |
| 16 | exit の Error→code 細部 | 採用済み分類 / 個別表 | 分類は採用済み。細部は実装と照合して別途確定 |

---

## 8. 参照

- 封筒・frame: [`wire-subset.md`](wire-subset.md)
- typed records・STATUS/LIST・validation: [`wire-records.md`](wire-records.md)
- connection gate: [`control-connection.md`](control-connection.md)
- authority: [`control-authority.md`](control-authority.md)
- client: [`control-client.md`](control-client.md)
- runtime connector: [`control-client-runtime.md`](control-client-runtime.md)
- stream lifecycle: [`receiver-v1-stream.md`](receiver-v1-stream.md)
- discovery JSON: [`json-discovery.md`](json-discovery.md)
- status JSON: [`json-status.md`](json-status.md)
- identity codec: [`identity.md`](identity.md)
- tuning resolver: [`tuning.md`](tuning.md)
- Windows stdin monitor: [`windows-stdin-monitor.md`](windows-stdin-monitor.md)
- host adapter の境界とライセンス: [`host-tuner.md`](host-tuner.md)、[`host-installed-headers.md`](host-installed-headers.md)
