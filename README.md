# DTV userland common core

本リポジトリは、DTV ユーザーランド製品群で共有する C++17 の移植可能な共通基盤を提供する。レビュー済みの error/result、ロギング、モック転送基盤、および境界付き UDTV 1.0 ワイヤサブセットを備える。デーモン、ハードウェア転送、製品接続、カードの実装そのものではない。

## 提供ライブラリと構成

静的ライブラリとして `dtv::core`、`dtv::wire`、`dtv::identity`、`dtv::tuning`、`dtv::json` を提供する。また、オプショナルな内部コンポーネントとして `dtv::receiver`（CMake オプション `DTV_USERLAND_BUILD_RECEIVER`、既定値 OFF）、`dtv::capture`（`DTV_USERLAND_BUILD_CAPTURE`、既定値 OFF）、`dtv::control`（`DTV_USERLAND_BUILD_CONTROL`、既定値 OFF）、`dtv::status_projection`（`DTV_USERLAND_BUILD_STATUS_PROJECTION`、既定値 OFF）、`dtv::authority`（`DTV_USERLAND_BUILD_AUTHORITY`、既定値 OFF）、`dtv::card`（`DTV_USERLAND_BUILD_CARD`、既定値 OFF）、GPL-2.0-onlyの互換owner adapter `dtv::host_tuner`（`DTV_USERLAND_BUILD_HOST_TUNER`、既定値 OFF）、およびそのEngineをAuthorityと共有するGPL-2.0-onlyの `dtv::host_authority_owner`（`DTV_USERLAND_BUILD_HOST_AUTHORITY_OWNER`、既定値 OFF）を備える。host adapterの境界と利用条件は[host tuner](docs/host-tuner.md)を参照のこと。

### 依存関係

- `dtv::wire`: `dtv::core` にリンク
- `dtv::identity`: `dtv::core` にリンク
- `dtv::tuning`: `dtv::wire` にリンク
- `dtv::json`: `dtv::wire` にリンク
- `dtv::control`: `dtv::wire` にリンク。socketなしprotocol gateと、transport注入のHELLO/LIST/STATUS read-only clientを提供
- `dtv::status_projection`: `dtv::control` と `dtv::receiver` にリンク。canonical LIST由来のSTATUS shellへengine状態を投影
- `dtv::authority`: `dtv::status_projection` にリンク。socketなしcontrol authority/session serviceを提供
- `dtv::receiver`: `dtv::core` および `Threads` にリンク
- `dtv::host_tuner`: `dtv::receiver` にリンク。既存TunerServiceのowner adapterのみを提供
- `dtv::host_authority_owner`: `dtv::authority` と `dtv::host_tuner` にリンク。既存TunerServiceのprivate Engineをfriend seamで借用し、Authority用ID domainを設定
- `dtv::capture`: `dtv::core` および `Threads` にリンク。receiver エンジンには依存しない
- `dtv::card`（INTERFACE ターゲット）: `dtv::core` にリンク

実装ターゲットである `dtv::core`、`dtv::wire`、`dtv::identity`、`dtv::tuning`、`dtv::json`、およびオプショナルな `dtv::receiver` / `dtv::capture` / `dtv::control` / `dtv::status_projection` / `dtv::authority` は、例外および RTTI を無効にしてコンパイルされる。オプショナルな `dtv::card` はヘッダーのみの INTERFACE ターゲットであり、テンプレートの実体化は利用側のコンパイル設定に従う。これらは OS や USB に依存しない。スレッド（Threads）を使うのはオプショナルな `dtv::receiver`、`dtv::capture`、`dtv::status_projection` である。

外部向けの公開契約は ctl CLI および JSON であり、ワイヤプロトコルは同一リリース内の内部通信に限定される。

## ビルドとテスト

CMake で設定し、`BUILD_TESTING`（または `DTV_USERLAND_BUILD_TESTING`）を有効にすると、有効化されたターゲットの単体テストをビルドできる。

- `foundation_tests`（`dtv::core`）
- `wire_tests`、`records_tests`（`dtv::wire`）
- `identity_tests`（`dtv::identity`）
- `tuning_tests`（`dtv::tuning`）
- `status_json_tests`、`status_json_consumer`（`dtv::json`）
- `discovery_json_tests`、`discovery_json_consumer`（`dtv::json`）
- `card_ownership_engine_tests`（`DTV_USERLAND_BUILD_CARD` 有効時、`dtv::card`）
- `receiver_engine_tests`、`receiver_r2_tests`、`receiver_hz_tests`、`stream_v1_authority_tests`、`stream_v1_owner_cleanup_tests`、`stream_v1_late_prepare_tests`（`DTV_USERLAND_BUILD_RECEIVER` 有効時、`dtv::receiver`）
- `host_tuner_tests`（`DTV_USERLAND_BUILD_HOST_TUNER` 有効時、`dtv::host_tuner`）
- `host_authority_owner_tests`（`DTV_USERLAND_BUILD_HOST_AUTHORITY_OWNER` 有効時、同一Engine上のlegacy/Authority leaseとID guard）
- `packet_stream_session_tests`、`stream_v1_store_tests`（`DTV_USERLAND_BUILD_CAPTURE` 有効時、`dtv::capture`）
- `control_connection_tests`（`DTV_USERLAND_BUILD_CONTROL` 有効時、`dtv::control`）
- `control_client_tests`（`DTV_USERLAND_BUILD_CONTROL` 有効時、`dtv::control`）
- `status_projection_tests`（`DTV_USERLAND_BUILD_STATUS_PROJECTION` 有効時、`dtv::status_projection`）

### ビルド・テスト手順

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

オプショナルコンポーネントを有効にする場合:

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DDTV_USERLAND_BUILD_RECEIVER=ON \
  -DDTV_USERLAND_BUILD_CAPTURE=ON \
  -DDTV_USERLAND_BUILD_CARD=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

オプションの `DTV_LEGACY_SOURCE` キャッシュパスを指定すると、固定された PX4 ソースツリーとの振る舞い等価性検証（`legacy_equivalence`）を実行できる。
ソースの固定位置、抽出ファイルのハッシュ、ファイル単位の SPDX 識別子、移動範囲、除外項目は `PROVENANCE.json` を参照のこと。

## インストールと利用 (CMake Consumer)

### インストール

```sh
cmake --install build --prefix /path/to/install
```

### 利用側の設定 (CMakeLists.txt)

利用側プロジェクトでは `find_package` でパッケージを読み込み、必要なターゲットをリンクする。

```cmake
find_package(dtv_userland_core CONFIG REQUIRED)

# 基本ライブラリの利用例
add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE
    dtv::core
    dtv::wire
    dtv::identity
    dtv::tuning
    dtv::json
)
```

オプショナルな内部エンジンを利用する場合は `COMPONENTS` を指定する。なお、consumer が `COMPONENTS receiver` や `card` を要求する場合、インストール元（ライブラリ側）のビルド時にもそれぞれ `DTV_USERLAND_BUILD_RECEIVER=ON`、`DTV_USERLAND_BUILD_CARD=ON` が指定されてインストールされている必要がある。

```cmake
# receiver エンジンを利用する場合（Threads が自動検出される）
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS receiver)
target_link_libraries(my_consumer PRIVATE dtv::receiver)

# card エンジンを利用する場合
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS card)
target_link_libraries(my_consumer PRIVATE dtv::card)

# capture store を利用する場合（Threads が自動検出される）
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS capture)
target_link_libraries(my_consumer PRIVATE dtv::capture)

# 内部 control protocol gate を利用する場合（dtv::wire のみへ依存）
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS control)
target_link_libraries(my_consumer PRIVATE dtv::control)

# canonical LIST と ReceiverEngine の STATUS 投影を利用する場合
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS status_projection)
target_link_libraries(my_consumer PRIVATE dtv::status_projection)

# legacy TunerService owner adapter（GPL-2.0-only）を利用する場合
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_tuner)
target_link_libraries(my_consumer PRIVATE dtv::host_tuner)

# 同一TunerService Engineをsocketless Authorityへ貸し出すowner seamを利用する場合
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_authority_owner)
target_link_libraries(my_consumer PRIVATE dtv::host_authority_owner)
```

### 利用側のビルド手順

任意の prefix へインストールしたパッケージを利用する場合、configure 時に `CMAKE_PREFIX_PATH` でインストール先を指定する。

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build
```

## 各コンポーネントの詳細

### 型付きワイヤペイロード (`dtv::wire`)

HELLO、ACQUIRE、LIST/STATUS、選局およびストリーム操作、イベント、カード／トランザクションレコード、ならびに境界付き UDTV 1.0 エンベロープとフレーム処理を提供する。[型付きレコードの仕様](docs/wire-records.md) ではバイト構文と文脈検証を分離しており、呼び出し側はスナップショットを公開する前に検証を行わなければならない。初期のフレーム処理サブセットは [docs/wire-subset.md](docs/wire-subset.md) に記載されている。
接続状態、ディスパッチ、タイマー、ソケット、JSON/CLI 変換、製品エンドポイントの移行は `dtv::wire` の対象外である。オプショナルな `dtv::control` はsocketなしの接続単位protocol gateと、transportを注入するHELLO/LIST/STATUS read-only clientを提供する。実socket・service・engine・capture data pathは含まない。wireは同一release内の内部契約であり、package component exportは安定した公開wire APIを意味しない。現行の製品クライアントやデーモンは新しいcontrol APIに接続されていない。詳細は [control connection](docs/control-connection.md) と [read-only control client](docs/control-client.md) を参照のこと。

### STATUS JSON 投影 (`dtv::json`)

`StatusResponse` と canonical LIST を含む context を受け取り、API 内で完全検証したうえで、コンパクトな単一の UTF-8 JSON オブジェクト（末尾改行あり、NUL なし）へ投影する境界付き API（`status_json_size`、`encode_status_json`）を提供する。動的メモリ確保は行わず、書き込み前に容量と値の検証を行うため、バッファ不足や不正な入力があっても出力バッファを破壊しない。
- 公開される `model` はモデル名文字列（例: `"PX-Q3U4"`）または `null` のみであり、数値や内部のモデル識別子は出力しない（採択済みの response 2.A / RC47 決定に基づく）。
- STATUS JSON では `receivers[].descriptor.capabilities` を出力しない。カードリーダーと LNB の静的構成は discovery JSON で公開する。LNA の値および既定値のみが今後の調査対象として保留されている。

### Discovery JSON 投影 (`dtv::json`)

全スキャン結果を `enclosures[].devices[].receivers[]` の階層と `serial_unique` を含む JSON へ投影する。[Discovery JSON の仕様](docs/json-discovery.md) を参照のこと。物理的な探索、CLI、デーモン、製品接続は本ライブラリでは行わない。STATUS JSON の詳細は [docs/json-status.md](docs/json-status.md) を参照のこと。

### デバイス識別コーデック (`dtv::identity`)

採択されたポータブル識別コーデックを実装する。`serial:` / `usb:` デバイスセレクタ文法、大文字小文字を保持する接続インスタンス検証と予約名拒絶、正確なスパンによる LANGID/USB 文字列記述子デコーダ（厳格な UTF-16LE から可逆 UTF-8 への変換）、および `auto-` SHA-256 接頭辞付きインスタンスによるバージョン付き `DTVI` 自動識別エンコーディングを提供する。純粋かつ有界で、動的メモリ確保を行わず、OS や USB の状態を変更しない。物理的な探索、シリアルの一意性・信頼性判定、権限／ACL、USB ファイルディスクリプタの所有権は OS アダプタ側に留まる。機械的に移植された部分は `dtv/userland/detail/sha256.h` 内の中立な SHA-256 クラスのみであり、他は新規実装である。詳細は [docs/identity.md](docs/identity.md) を参照のこと。

### 選局リゾルバ (`dtv::tuning`)

採択されたチャンネル表記または型付き周波数（Hz）入力を、ワイヤシステム、要求周波数（Hz）、ワイヤ選局パラメータへと解決するポータブルヘルパーである。要求周波数をそのまま保持し、オプション解析、デバイス能力検査、バックエンドでの量子化、製品 CLI の切り替えは本インクリメントでは扱わない。文法と境界は [docs/tuning.md](docs/tuning.md) を参照のこと。

### 受信機ライフサイクル互換エンジン (`dtv::receiver`)

従来の直接選局パスは受信機と電源の準備完了後から計時を開始し、エンドツーエンドのデッドラインを持たない。一方、厳格モード（`configure_strict_capabilities`）では受付時からのエンドツーエンドの予算を強制する。従来の選局入口はkHz callbackを維持し、厳格Hz入口はnative Hzを宣言したbackend callbackだけを使い、kHzへの共通fallbackを行わない。Hz入口とbackendのprogrammed値は製品には接続されていない。受信機 ID は8スロットテーブルから独立したu16識別子である。受信機セットはエンジン構築時に固定される。サポート対象のコンシューマ（PX4 Q3U4、W3U4、MLT、単一受信機バックエンド、およびプロファイル選択後の ASICEN）はサービスの存続期間中に固定の受信機数を持つ。後から出現または消失する動的受信機は本インクリメントではサポートしない。本ターゲットのみスレッド（Threads）にリンクする。

### カード排他制御互換エンジン (`dtv::card`)

`card_ownership_engine.h` およびテンプレート実装 `card_ownership_engine.tpp` において、内部の `detail::CardOwnershipEngine<AtrSnapshot>` を公開する（CMake オプション `DTV_USERLAND_BUILD_CARD`、既定値 OFF）。既存のシリアライズされた排他制御、共有、存在／世代管理、トランザクション、解放処理を、中立なバイト／セッション／バックエンド境界の背後で提供する。ATR スナップショットは不透明な値として扱い、製品固有の `CardAtr`、ネイティブプロトコルセッション、ATR パース、T=1、IPC 列挙型、ハードウェアコードは別ライセンスの製品ファサード側に保持する。本エンジンはスレッド、USB、OS への依存を持たず、公開された v1 カード／デッドライン／複数リーダー契約ではない。

### TS packet store (`dtv::capture`)

`DTV_USERLAND_BUILD_CAPTURE`（既定値 OFF）で有効にする独立した内部コンポーネントで、188-byte 完全packetのbounded保持、read、detach後のdrain/final観測、およびincarnation付きread ticketを提供する。legacy policyは満杯時に到着packetを拒否してslow-consumerを記録し、v1 policyは最古の未読packetをevictして継続する。これはproducer、socket、USB、producer quiescence、製品接続を提供せず、receiver-only構成から独立してbuild/installできる。共有session型は `detail/stream_session_types.h` に分離され、receiverとcaptureの有効時だけinstallされる。

## ライセンス範囲

ルートの MIT ライセンスは、本候補で承認された基盤の移行先、独自実装されたワイヤサブセット、ならびに新規に追加されたビルド、パッケージング、テスト、ドキュメントの各ファイルに適用される。PX4 製品全体、リストされていないソースファイル、サードパーティの成果物、および将来の GPL アダプタを再ライセンスするものではない。これは承認されたライセンス範囲を記録するものであり、著作権の所有権や著作者を主張または推定するものではない。適用範囲は、外部ワークスペースの設計方針資料 `design/independent-code-policy.md`（本リポジトリには非同梱）、2026-10-18 02:44 JST に記録されたユーザーの継続方針、および 2026-10-18 03:06 JST の初期インクリメント実装開始に従う。本リポジトリにおける詳細なソースピン、抽出ハッシュ、SPDX 識別子、除外範囲の記録は `PROVENANCE.json` を参照のこと。
