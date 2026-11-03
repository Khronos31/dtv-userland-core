# UDTV 1.0 wire サブセット

`dtv::wire`（`include/dtv/userland/wire.h`、`src/wire.cpp`）は、共通 UDTV wire の先頭スライスを独自に実装したコンポーネントである。エンベロープ（envelope）、断片化に対応したフレーム受信機（`FrameReceiver`）、および 3 つの型付きペイロードを扱う。プロトコル全体を網羅するものではなく、既存の製品クライアント、デーモン、IPC ファイルとは接続していない。

## エンベロープ

すべての整数はリトルエンディアンである。各フィールドは 1 バイトずつ読み書きされ、構造体をそのまま wire へ/wire からコピーすることはない。そのため、ホストのエンディアン、パディング、ABI が漏出しない。

| オフセット | サイズ | フィールド | 規則 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `UDTV`。旧方式の `PX4U` および `ASCN` マジックは拒否し、`HeaderStatus::legacy_magic` として個別に報告する。そのペイロードは一切解釈しない。 |
| 4 | 2 | major | 1 でなければならない。 |
| 6 | 2 | minor | 0 でなければならない。それ以外の major や minor は `VERSION_MISMATCH` とする。 |
| 8 | 2 | opcode | raw u16。未知の値も受理する。 |
| 10 | 2 | flags | 0、1、または 3。それ以外はすべて拒否する。 |
| 12 | 4 | request_id | 下記の種別（kind）を参照。 |
| 16 | 4 | payload_length | ペイロードのバイト列を読み込む、または格納する前に、上限と照合して検査する。 |

opcode の bit 15 はイベントを示す（`is_event_opcode`）。この規則は、既知・未知を問わずすべての opcode に適用される。有効な組み合わせは次のものに限られる:

| 種別（Kind） | opcode bit 15 | flags | request_id |
|---|---:|---:|---|
| request | 0 | 0 | 0 以外（nonzero） |
| response | 0 | 1 | 0 以外（nonzero） |
| error_response | 0 | 3 | 0 以外（nonzero） |
| event | 1 | 0 | 0 |

それ以外のすべての組み合わせは不正なエンベロープ（`PROTOCOL_ERROR`）とする。該当 opcode のペイロードディスパッチが対象外であっても同様である:

- flags 2、または予約ビットの指定: `bad_flags`。
- イベント opcode に対する flags 1 または 3 の指定: `bad_kind_for_opcode`。例: STREAM_END `0x80ff` に flags 1 と id 1 を指定。
- 通常 opcode に対する request_id 0 の指定、またはイベント opcode に対する 0 以外の request_id の指定: `bad_request_id`。例: `0x0010` に flags 0 と id 0 を指定。

未知 opcode であっても、エンベロープとして正当であれば（0 以外の id を持つ通常 opcode、または flags 0 かつ id 0 のイベント opcode）、受理されてディスパッチャへそのまま届く。これに対して `UNSUPPORTED` を返すのはディスパッチャの責務である。エンコーダも同じ表の規則を強制する。

### ペイロード上限

- TS_DATA イベント: `kMaxTsDataPayload` = 1,048,476 + 28 = 1,048,504 バイト。1,048,476 は 1 MiB を超えない 188 の最大倍数であり、28 バイトはパケット直前の固定 TS_DATA フィールドである。この上限はペイロード全体に適用され、生のパケットバイト数だけを対象とするものではない。また、エンベロープレベルではパケット数や 188 の倍数であるかの検査は行わない。それは TS_DATA コーデックの責務であり、本サブセットの対象外である。
- それ以外のすべてのフレーム（未知 opcode や他のイベントを含む）: `kMaxControlPayload` = 65,536。

旧 PX4 の TS_DATA 上限（1,048,596）は UDTV の上限を超えているため拒否される。

## API

- `check_header(ByteView, Header*)` は詳細な `HeaderStatus` を返し、`decode_header(ByteView)` は `header_error()` で対応づけたステータスを含む `Result<Header>` を返す。ビュー不正またはサイズが 20 以外なら `INVALID_ARGUMENT`、バージョン不正なら `VERSION_MISMATCH`、それ以外はすべて `PROTOCOL_ERROR` とする。厳密に 20 バイトを要求し、ペイロードには一切触れない。
- `encode_header` および `encode_frame` はデコードと同じ規則を強制する。不正なヘッダは `INVALID_ARGUMENT`、出力先不足は `BUFFER_TOO_SMALL` とし、失敗時には何も書き込まない。`encode_frame` は `payload.size == header.payload_length` であることを要求する。
- `decode_frame(ByteView)` は厳密に `20 + payload_length` バイトを要求する。入力が不足または超過していれば `PROTOCOL_ERROR` とする。返される `Frame::payload` は入力をエイリアス（参照）する。
- `FrameReceiver` は、任意のサイズの断片（連結フレームを含む）からソケットや動的メモリ確保なしでフレームを再構成する。呼び出し側がペイロード格納領域を提供する。ヘッダは 20 バイトが揃った時点で直ちに検査されるため、上限超過や不正な長さのフレームは、ペイロードのバイトが 1 バイトもコピーされる前に拒否される。呼び出し側の提供領域を超えるペイロードは `BUFFER_TOO_SMALL` とする。完成した各フレームは、未知 opcode のフレームも含めて `FrameHandler::on_frame` へそのまま渡される（ここがディスパッチャとの境界となる）。ハンドラでの失敗を含め、何らかのエラーが発生すると、`reset()`（メモリ上で接続切断に相当する処理）が呼ばれるまでレシーバは無効化（poison）される。`header_status()` により、ヘッダが拒否された理由を呼び出し側へ通知する。
- 入力が EOF に達したときは `FrameReceiver::finish()` を呼ぶ。正常なフレーム境界（`pending_size() == 0`）のときのみ成功する。ヘッダやペイロードの途中で EOF に達した場合は、プロトコルがフレームの途中で終了したことを意味する。`finish()` は `PROTOCOL_ERROR` を返し、`truncated()` を設定してレシーバを無効化する。その後にバイト列が投入されても拒否し、新たなフレームへの再同期は試みない。既に失敗状態にあるレシーバに対して `finish()` を呼んだ場合は、その失敗をそのまま返す。`pending_size()` は、バッファされている未完了フレームのバイト数を報告する（上限は 20 + ペイロード上限および呼び出し側格納領域）。
- サイズが 0 以外でポインタが null のビューは、どこであっても `INVALID_ARGUMENT` として拒否する。サイズ 0 の null ビューは、正当な空ビューとして扱う。

フレームごとにメモリを確保したいレシーバは、20 バイトを読んで `decode_header` を呼び、成功した後に `payload_length` バイトだけを確保すればよい。上限値が定められているため、確保サイズの上限は保証される。

## 型付きペイロード

すべての型付きデコーダは、厳密なペイロードサイズを要求し、それ以外は `PROTOCOL_ERROR` を返す。サイズは合っているが意味的に不正な値は `INVALID_ARGUMENT` とする。エンコーダは出力バッファ不足に対して `BUFFER_TOO_SMALL` を返し、中途半端な値を書き込むことはない。

| ペイロード | レイアウト | サイズ |
|---|---|---:|
| `HelloRequest` | `u16 min_major, u16 min_minor, u16 max_major, u16 max_minor, u64 required_features, u64 optional_features` | 24 |
| `AcquireRequest` | `u8 epoch[16], u16 receiver_id` | 18 |
| `AcquireResponse` | `u8 epoch[16], u64 lease_id` | 24 |

### HELLO バージョン範囲

- バージョンは (major, minor) として比較する。min > max の範囲は、エンコーダ・デコーダともに `INVALID_ARGUMENT` として拒否する。
- `negotiate_version` は、min <= 1.0 <= max のときに 1.0 を返す。それ以外は `VERSION_MISMATCH` を返し、逆転した範囲は `INVALID_ARGUMENT` とする。本サブセットが選択できるバージョンは 1.0 のみである。それ以外の major や minor を持つフレームは、厳密なバージョン検査により失敗する。
- `required_features` と `optional_features` は、未知のビットや上位ビットも含めて変更せずそのまま運搬する。コーデックはこれらを判定せず、機能ポリシーの判定はディスパッチャの責務である。

### ACQUIRE

- `epoch` は不透明な値（opaque）であり、双方向へそのままコピーされる。
- `receiver_id` は、0 や 255 を超える値も含め、任意の u16 を取りうる。該当 ID が公開レシーバ集合に含まれているかはデーモンが判断し、コーデックは判断しない。
- `AcquireResponse::lease_id` は 0 以外（nonzero）でなければならない。エンコーダ・デコーダともに、epoch の値にかかわらず 0 を `INVALID_ARGUMENT` として拒否する。それ以外の任意の u64 は有効である。nonce は存在せず、アタッチトークンは後続の wire 仕様における START に属する。

## 本サブセットに含まれないもの

- HELLO 応答、error-response ペイロード（コードと詳細）、RELEASE、TUNE、START/STOP、STATS、LIST/STATUS レコード、カード操作、ATTACH、および 188 バイトパケット境界検査や 28 バイトプレフィックスの意味を含む TS_DATA ペイロードコーデック。これらは後続のコーデックで扱われる。
- 接続状態の管理: 初回フレームとしての HELLO または ATTACH_STREAM、交渉済みバージョンとヘッダバージョンの照合、1 接続あたり 1 同時要求の制限、再利用または過去の request_id の拒否（仕様上の PROTOCOL_ERROR）、要求と応答の突き合わせ、不足している必須機能に対する UNSUPPORTED、STATE_EVENT 購読。これらにはディスパッチャが必要であり、本サブセットには含まれない。
- 期限（時間制限）: 未完了フレームに対する 5 秒の制限、および未完了 HELLO に対する 5 秒の制限。`FrameReceiver` は時計を持たないため、OS アダプタがこれらを強制した上で `finish()` を呼ぶか切断しなければならない。
- ソケット、タイムアウト、デーモンやハードウェアとの統合。
- `FrameReceiver` はシングルスレッド設計であり、再入不可能である。ハンドラが同一レシーバに対して `feed` を呼び出してはならない。

本節は `dtv::wire` 単体について述べている。`DTV_USERLAND_BUILD_CONTROL=ON` の場合、分離された内部コンポーネント `dtv::control` がソケット不要な接続ゲートと応答ヘルパーを提供する（[`control-connection.md`](control-connection.md) 参照）。サービス実行や製品統合は追加しない。
