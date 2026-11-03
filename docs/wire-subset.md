# UDTV 1.0 wire サブセット

`dtv::wire`（`include/dtv/userland/wire.h`、`src/wire.cpp`）は、共通 UDTV wire 仕様の先頭スライスを独自に実装したライブラリコンポーネントである。電文共通のエンベロープ（ヘッダーと全体枠）、断片化に対応したフレーム受信機（`FrameReceiver`）、および 3 種類の型付きペイロードを扱う。プロトコル全体を網羅するものではなく、既存の製品クライアント、デーモン、IPC ファイルとは接続していない。

## エンベロープ

すべての整数フィールドはリトルエンディアンで配置する。各フィールドは 1 バイトずつ読み書きし、メモリ上の構造体をそのまま通信路（wire）へコピーすることはない。そのため、ホスト CPU のエンディアン、構造体パディング、ABI の差異が電文へ漏れ出すことはない。

| オフセット | サイズ | フィールド | 規則 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII 文字列 `UDTV`。旧方式の `PX4U` および `ASCN` マジックは拒否し、`HeaderStatus::legacy_magic` として個別に報告する。そのペイロードは一切解釈しない。 |
| 4 | 2 | major | 必ず 1 とする。 |
| 6 | 2 | minor | 必ず 0 とする。それ以外の major や minor は `VERSION_MISMATCH` とする。 |
| 8 | 2 | opcode | 符号なし 16 ビット整数（u16）。未知の値であってもエンベロープとしては受理する。 |
| 10 | 2 | flags | 0、1、または 3 のみ有効。それ以外の値はすべて拒否する。 |
| 12 | 4 | request_id | 下記の種別（kind）表に従う。 |
| 16 | 4 | payload_length | ペイロードのバイト列を読み込む、またはバッファへ格納する前に、上限値と照合して検査する。 |

opcode の bit 15 はイベント識別子を表す（`is_event_opcode`）。この規則は、既知・未知を問わずすべての opcode に適用される。有効な組み合わせは次の 4 パターンに限られる:

| 通信種別（Kind） | opcode bit 15 | flags | request_id |
|---|---:|---:|---|
| 要求（request） | 0 | 0 | 0 以外（nonzero） |
| 応答（response） | 0 | 1 | 0 以外（nonzero） |
| エラー応答（error_response） | 0 | 3 | 0 以外（nonzero） |
| イベント（event） | 1 | 0 | 0 |

それ以外の組み合わせはすべて不正なエンベロープ（`PROTOCOL_ERROR`）として扱う。該当 opcode のペイロード処理が未実装であっても同様である:

- flags に 2 または予約ビットが指定された場合: `bad_flags`。
- イベント用 opcode に対して flags 1 または 3 が指定された場合: `bad_kind_for_opcode`（例: STREAM_END `0x80ff` に flags 1 と id 1 を指定）。
- 通常 opcode に対して request_id 0 が指定された場合、またはイベント opcode に対して 0 以外の request_id が指定された場合: `bad_request_id`（例: `0x0010` に flags 0 と id 0 を指定）。

未知の opcode であっても、エンベロープとして正当であれば（0 以外の id を持つ通常 opcode、または flags 0 かつ id 0 のイベント opcode）、形式エラーとせずディスパッチャーへそのまま渡す。未知の操作に対して `UNSUPPORTED` を返すのはディスパッチャーの責務である。エンコーダーも同じ表の組み合わせ規則を強制する。

### ペイロード上限

- TS_DATA イベント: `kMaxTsDataPayload` = 1,048,476 + 28 = 1,048,504 バイト。1,048,476 は 1 MiB を超えない 188 の最大倍数であり、28 バイトはパケット直前に配置される固定 TS_DATA フィールドである。この上限はペイロード全体に適用され、生のパケットデータ長のみを対象とするものではない。なお、エンベロープレベルではパケット数や 188 バイト境界の整序性は検査しない。それは後続の TS_DATA コーデックが担う責務であり、本サブセットの対象外である。
- その他の全フレーム（未知 opcode や他イベントを含む）: `kMaxControlPayload` = 65,536 バイト。

旧 PX4 の TS_DATA 上限（1,048,596）は UDTV の上限を超過しているため拒否される。

## API 契約

- `check_header(ByteView, Header*)` は詳細な `HeaderStatus` を返し、`decode_header(ByteView)` は `header_error()` で対応づけたエラー情報を含む `Result<Header>` を返す。入力ビューの指定不正または長さが 20 バイト以外であれば `INVALID_ARGUMENT`、バージョン不一致なら `VERSION_MISMATCH`、それ以外の形式不正はすべて `PROTOCOL_ERROR` とする。過不足なく正確に 20 バイトであることを要求し、ペイロード領域には一切触れない。
- `encode_header` および `encode_frame` はデコード処理と同じ規則を強制する。不正なヘッダー値には `INVALID_ARGUMENT`、出力先バッファ不足には `BUFFER_TOO_SMALL` を返し、失敗時には出力先を変更しない。`encode_frame` は `payload.size == header.payload_length` であることを要求する。
- `decode_frame(ByteView)` は入力長が過不足なく正確に `20 + payload_length` バイトであることを要求する。入力が不足または超過していれば `PROTOCOL_ERROR` とする。返される `Frame::payload` は入力バッファの該当領域を参照（借用）する。
- `FrameReceiver` は、任意のサイズに断片化されたデータや複数フレームの連結データから、動的メモリ確保やソケット I/O を行わずにフレームを再構成する。呼び出し側がペイロード格納用のバッファ領域を提供する。ヘッダーの 20 バイトが揃った段階で直ちに検証が行われるため、サイズ超過や不正な長さのフレームは、ペイロードのデータが 1 バイトもコピーされる前に早期拒否される。呼び出し側の提供バッファを超えるペイロードは `BUFFER_TOO_SMALL` とする。完成した各フレームは、未知 opcode を含めてそのまま `FrameHandler::on_frame` へ引き渡される。ハンドラ側での失敗を含め、何らかのエラーが発生すると、`reset()`（接続切断に相当するメモリ初期化）が呼ばれるまで `FrameReceiver` は無効化状態（poisoned）に保たれる。拒否理由は `header_status()` で取得できる。
- 入力ストリームが EOF に達した際は `FrameReceiver::finish()` を呼び出す。正常なフレーム境界（`pending_size() == 0`）にある場合のみ成功する。ヘッダーやペイロードの途中で EOF に達した場合は、フレームの中途で通信が途絶したことを意味するため、`finish()` は `PROTOCOL_ERROR` を返し、`truncated()` フラグを設定して `FrameReceiver` を無効化する。その後にデータが投入されても拒否し、新たなフレームへの再同期は試みない。既にエラー状態にある `FrameReceiver` に対して `finish()` を呼んだ場合は、その既存のエラーをそのまま返す。`pending_size()` は、バッファリング中の未完了フレームのバイト数を報告する（上限は 20 + ペイロード上限および呼び出し側バッファサイズ）。
- ポインタが null でありながらサイズが 0 以外に設定されたビューは、すべての API で `INVALID_ARGUMENT` として拒否する。サイズ 0 の null ビューは正当な空ビューとして扱う。

フレームごとに動的にメモリを割り当てたい受信側は、まず 20 バイトを読み込んで `decode_header` を呼び出し、ヘッダー検証に成功した後に `payload_length` バイトの領域を確保すればよい。ペイロード上限が定められているため、確保サイズの上限は常に保証される。

## 型付きペイロード

すべての型付きデコーダは、各定義に応じた過不足のない正確なペイロード長を要求し、長さが合致しない場合は `PROTOCOL_ERROR` を返す。長さは合致しているが値が意味的に不正である場合は `INVALID_ARGUMENT` とする。エンコーダは出力バッファ不足に対して `BUFFER_TOO_SMALL` を返し、不完全なデータを書き込むことはない。

| ペイロード型 | レイアウト | サイズ |
|---|---|---:|
| `HelloRequest` | `u16 min_major, u16 min_minor, u16 max_major, u16 max_minor, u64 required_features, u64 optional_features` | 24 バイト |
| `AcquireRequest` | `u8 epoch[16], u16 receiver_id` | 18 バイト |
| `AcquireResponse` | `u8 epoch[16], u64 lease_id` | 24 バイト |

### HELLO バージョン範囲

- バージョン比較は (major, minor) の順序で行う。min > max となる逆転した範囲指定は、エンコーダおよびデコーダの双方で `INVALID_ARGUMENT` として拒否する。
- `negotiate_version` は、min <= 1.0 <= max の範囲に含まれる場合に 1.0 を返す。それ以外は `VERSION_MISMATCH` を返し、逆転した範囲指定は `INVALID_ARGUMENT` とする。本サブセットが選択できるバージョンは 1.0 のみである。それ以外のメジャーまたはマイナーバージョンを持つフレームは、厳密なバージョン検査により失敗する。
- `required_features` および `optional_features` は、未知のビットや上位ビットも含めてそのまま透過的に運搬する。コーデック層ではこれらを判定せず、機能ポリシーの判断は上位のディスパッチャーが担当する。

### ACQUIRE

- `epoch` は不透明な値（16 バイトのバイナリ識別子）であり、要求・応答の双方向へそのままコピーされる。
- `receiver_id` は、0 や 255 を超える値を含め、任意の u16 値を取りうる。指定された ID が公開受信機集合に含まれているかはデーモン側で判断し、コーデック層は判定しない。
- `AcquireResponse::lease_id` は必ず 0 以外（nonzero）でなければならない。エンコーダおよびデコーダの双方で、epoch の値にかかわらず 0 を `INVALID_ARGUMENT` として拒否する。0 以外の任意の u64 値は有効である。ACQUIRE にはノンス（nonce）は存在せず、アタッチ用トークンは後続の wire 仕様における START 処理で発行される。

## 本サブセットに含まれないもの

- HELLO 応答、エラー応答ペイロード（エラーコードと詳細文字列）、RELEASE、TUNE、START/STOP、STATS、LIST/STATUS レコード、カード操作、ATTACH、および 188 バイトパケット境界検査や 28 バイトプレフィックスの意味論を含む TS_DATA ペイロードコーデック。これらは後続のコーデックで扱われる。
- 接続状態の管理: 初回フレームとしての HELLO または ATTACH_STREAM の強制、交渉済みバージョンとヘッダーバージョンの照合、1 接続あたり 1 同時要求の制限、再利用または過去の request_id の拒否（仕様上の PROTOCOL_ERROR）、要求と応答の突き合わせ、不足している必須機能に対する UNSUPPORTED、STATE_EVENT 購読。これらにはディスパッチャーが必要であり、本サブセットには含まれない。
- 制限時間管理: 未完了フレームに対する 5 秒の制限、および未完了 HELLO に対する 5 秒の制限。`FrameReceiver` 自体は時計を持たないため、OS アダプター側が制限時間を管理した上で `finish()` の呼び出しまたはソケット切断を行わなければならない。
- ソケット通信、タイムアウト制御、デーモンや実ハードウェアとの統合。
- `FrameReceiver` は単一スレッドでの実行を前提としており、再入不可能である。ハンドラ内部から同一 `FrameReceiver` インスタンスに対して `feed` を再帰的に呼び出してはならない。

本節は `dtv::wire` 単体について述べている。ビルドオプション `DTV_USERLAND_BUILD_CONTROL=ON` の場合、分離された内部コンポーネント `dtv::control` がソケット不要な接続ゲートと応答ヘルパーを提供する（[`control-connection.md`](control-connection.md) 参照）。サービス実行や製品統合は追加しない。
