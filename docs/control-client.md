# 制御クライアント

`dtv::control` はソケットなしゲートコンポーネントとして、status API、期限付き recording API、独立 LNB 給電クライアントを提供する。呼び出し側が `ClientTransport` または `TimedClientTransport` を実装して注入する。この Core ライブラリはソケットやファイル記述子アダプタを提供しない。`fetch_status` は HELLO/LIST/STATUS だけを要求し、ACQUIRE/TUNE/START/STOP/RELEASE は行わない。recording API の `begin_recording`、`stop_recording`、`release_recording` はそれぞれ明示的にこれらの制御要求を行う。

`fetch_status` は同じトランスポートで HELLO、筐体スコープの LIST、STATUS を順に要求し、request ID 1、2、3 を使う。optional feature なしで UDTV 1.0 を要求する。HELLO で選択されたバージョン、系列内の epoch とプロデューサ製品/バージョン、正規 LIST、および `validate_status_response` による LIST 基準の STATUS を検証する。LIST と STATUS が公開する筐体/機能の identity フィールドも照合する。Model は現行の name のみのフィールドで比較する。呼び出し前に未使用の新しい接続を渡すこと。

フレームと一時ペイロードには 1 つの上限付きワークスペースを使い、各応答でペイロード領域を再利用する。ワークスペースは呼び出し中だけ有効な約 135 kB（約 132 KiB）のヒープ領域で、最大制御ペイロードの要求フレームと応答ペイロードを別々に保持し、確保には例外を使わない。確保できない場合は `RESOURCE_LIMIT` と既定結果を返し、トランスポートを閉じる。デコードしたフレームビューはレシーバが進む前に処理し、返却するスナップショット/セッションは型付きレコードを所有してトランスポート/ワークスペースのバッファを参照しない。`ClientTransport` は呼び出し側の所有物で、呼び出し中に有効でなければならない。`write_all` はビュー全体を書き終えて成功するか、エラーを返す。`read_some` は読んだバイト数、ストリーム終端の 0、またはエラーを返す。成功した読み取りにはフレームの断片や複数フレームが含まれ得るが、クライアントは要求に一致しないフレームを受け付けない。opcode と request ID が一致する response/error-response だけを処理する。

スタック使用量は呼び出し側のスレッドにも依存する。Linux x86_64 の Clang 19.1.7 Release では、測定時に CMake の Release 既定値（`-O3 -DNDEBUG`）を `-O2 -DNDEBUG -fstack-usage` で置き換え、`fetch_status` フレームが 64,888 バイト、共有 records デコーダの `decode_status_response` フレームが 54,248 バイト、返却する `StatusFetchResult` が 64,400 バイトだった。単純合算は 183,536 バイト（179.2 KiB、約 184 kB）で、呼び出し側自身のフレームやランタイムオーバーヘッドは別に加わる。結果を呼び出しスレッド上のローカル変数に置く場合、これらすべてを考慮する。通常の小スタックテストでは、結果をプローブスレッド内に置いた正常・EOF 経路を最適化済み構成で 256/512/768 KiB、無最適化構成で 512/768 KiB のスタックにより確認する。256 KiB は最適化済み構成での参考実測であり、全ビルド種別や呼び出し側への保証ではない。Clang 19.1.7 ではビルド種別未指定の無最適化構成で 256 KiB プローブがスタックオーバーフローしたため、この構成では 512 KiB から確認する。GCC 14.2 Release では CMake 既定の `-O3 -DNDEBUG` に `-fstack-usage` を追加して計測し、デコーダフレームは 108,464 バイト、`StatusFetchResult` を同じスレッドに置く正常経路は約 237 KiB だった。GCC 14.2 Debug の ASan+UBSan（`-g -fsanitize=address,undefined -fno-sanitize=vptr`）では、独立したプローブ計測で正常経路に約 295 KiB を要した。サニタイザランタイムと計装（instrumentation）による増加分は構成・コンパイラ依存であり、通常時のフレーム値から一律には算出できない。このためサニタイザ時は小スタックテストのプローブを 512/768 KiB、通常時は最適化済み構成で 256/512/768 KiB、無最適化構成で 512/768 KiB とし、双方で EOF 早期失敗と正常経路を検査する。

ここでプローブの KiB は 1024 バイト、kB は 1000 バイトを表す。CMake の通常 Release 既定は `-O3 -DNDEBUG`、Debug 既定は `-g` である。`dtv_userland_control` と依存ターゲットには `-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic` が加わる。前記の最適化/サニタイザフラグは各測定で実際に使った追加または置換フラグで、全ビルドへ適用される設定ではない。read-only スタックプローブは Linux x86_64 で実行し、これらは限定環境での測定であり、全コンパイラ/OS へのスタック保証値ではない。呼び出し側は結果の配置場所とデコーダを含む余裕を確保する必要がある。

失敗時は既定スナップショットとローカルエラーを返し、リモートのエラー応答をデコードできた場合は raw コードも返す。その際 `close()` を呼ぶ。未知のリモートコードは raw コードを保持したまま `PROTOCOL_ERROR` に対応づける。成功時はトランスポートを開いたまま呼び出し側へ返す。要求の自動再試行はしない。この API はタイムアウト値やタイムアウトポリシーを定めず、ブロッキングとキャンセルの挙動は注入トランスポートの責任とする。

read-only の `fetch_status` は ACQUIRE/TUNE/START/STOP/RELEASE を行わない。recording API は ACQUIRE/TUNE/START/STOP/RELEASE のみを行い、サービス呼び出し、ATTACH、TS_DATA、レシーバ操作、キャプチャ、USB アクセス、製品接続は行わない。UDTV は同一リリース内の内部契約であり、リリースをまたぐ安定 wire API ではない。

## 期限付き status/recording クライアント

期限付き操作には別の `TimedClientTransport` を注入する。これは呼び出し側が所有し、`write_all`、`read_some`、`monotonic_ms`、`close` を実装する。既存の `ClientTransport` の純粋仮想関数と `fetch_status(ClientTransport&)` のシグネチャ・ブロッキング挙動は変わらない。Timed クライアントは同じトランスポートオブジェクトを借り、ソケットやファイル記述子を生成・所有しない。各 read/write 実装は渡された有限タイムアウトを守り、呼び出しがタイムアウト後もブロックし続けないことが前提になる。

`fetch_status(TimedClientTransport&, Timeout)` は呼び出し開始時に単調時計の絶対期限（absolute deadline）を 1 つ作り、HELLO/LIST/STATUS と断片読み取りを通じて同じ残時間を渡す。部分的な進捗で期限は延びない。期限値 0 は `INVALID_ARGUMENT`、期限算出が時計の uint64 範囲を超える場合は `TIMEOUT` とし、どちらもトランスポートを閉じて既定スナップショットを返す。I/O タイムアウト、EOF、デコード/検証の失敗もトランスポートを閉じ、スナップショットを公開しない。成功時はスナップショットを返し、接続は呼び出し側へ開いたまま返す。

`fetch_status(TimedClientTransport&, Timeout, required_features)` は同じ HELLO/LIST/STATUS シーケンスをひとつの期限内で実行し、HELLO の required feature マスクを送る。required ビットを提供しないサーバは HELLO で拒否する。応答が要求マスクを満たさない場合も `UNSUPPORTED`、要求していない機能を返すなど応答マスクが一致しない場合は `PROTOCOL_ERROR` とし、既定スナップショットを返してトランスポートを閉じる。`required_features == 0` は既存の期限付きオーバーロードと同じ status-only 交渉である。非期限付き `ClientTransport` と既存の仮想関数の形式は変わらない。

`begin_recording` はその `fetch_status` を最初に使用し、必要なら `RecordingStatusValidator` を ACQUIRE より前に呼ぶ。バリデータによる拒否は既定セッションを返し、トランスポートを閉じる。status 段の期限が終了した後、ACQUIRE/TUNE/START 段には同じ長さの新しい期限を作る。この 2 つの予算（2 budget）の動作は旧 recording ヘルパーとの互換実装であり、begin 全体を単一予算にはしない。オプションの数値既定値/範囲、`TuneRequest.timeout_ms` への製品側対応、OS ソケットタイムアウトとの換算は定義しない。

この 2 予算（2 budget）の分割は旧 recording ヘルパーとの互換を保つための現実装上の選択であり、将来の製品向けタイムアウト契約や恒久的なユーザー仕様を定めるものではない。新しいタイムアウト数値、既定値、製品オプションとの換算は定義しない。

begin は 1 つの上限付きワークスペースを例外なしのヒープ確保で使い、既存 status 実装の応答デコーダ/検証処理を呼び出す。ACQUIRE/TUNE/START の失敗は既定セッションとリモートの raw エラーコードを返し、トランスポートを閉じる。開始成功後にのみ epoch、lease、tune 結果/リビジョン、stream ID、attach トークン、expiry、次 request ID を含む呼び出し側所有のセッションを返す。attach トークンは別データ接続の ATTACH にだけ使い、クライアントはデータソケットや ATTACH を行わない。START 成功は armed 受理を示すだけであり、TS アクティブを示すものではない。

`stop_recording` はセッションのストリームへ 1 回の型付き STOP を送り、lease を解放しない。`release_recording` はセッションの lease へ別の型付き RELEASE を 1 回送る。両 API は失敗・応答不明の要求を再送せず、成功時はトランスポートを開いたまま返す。応答が届く前にデーモンが START/STOP/RELEASE を適用した可能性はクライアントから判定できないため、タイムアウト後に物理クリーンアップ完了や `quiesced=true` を推定しない。切断後の回収はデーモン所有者契約の責任である。

## CARD セッションクライアント

`begin_card(TimedClientTransport&, CardClientOptions, Timeout)` は CARD_SERVICE を required feature として HELLO を行い、同じ接続で LIST/STATUS、リーダーの存在確認、CARD_STATUS、CARD_CONNECT を行う。HELLO/LIST/STATUS にはひとつの期限、CARD_STATUS/CARD_CONNECT にはその後に始める同じ長さの期限を使い、各区間内で期限を延長しない。request ID は 1〜5 を使う。リーダーがスナップショットにない場合や identity 制約が一致しない場合は `NOT_FOUND` で接続を閉じ、リーダーや CARD 機能を推定しない。成功時はデーモン epoch、リーダー、ハンドル、generation、CARD_CONNECT が選択したプロトコル、次 request ID 6 を `CardClientSession` に保持し、接続を開いたまま返す。

`card_status` はセッションの epoch/リーダーを使って現在 generation と挿入・初期化状態を取得する。`card_reset` はセッションの epoch/リーダー/ハンドル/束縛 generation を送る。応答が同じ epoch/リーダー/ハンドルを示し、型付きセッション結果の検証に成功した場合だけ generation とプロトコルを更新し、ATR を含む応答を返す。`card_transmit` は usable セッションに保存されたプロトコルと一致する T=1 プロトコル、および 1〜4096 バイトの APDU を受け付け、同じ epoch/リーダー/ハンドル/generation を持つ応答だけを公開する。TRANSMIT 応答も最大 4096 バイトで、応答キーの不一致やデコード失敗では既定応答を返す。`card_disconnect` はセッションの epoch/リーダー/ハンドル/束縛 generation を使う。応答が同じリーダー/ハンドルを示して `consumed=true` なら、`quiesced` やクリーンアップエラーの有無にかかわらずセッションのハンドル/generation/プロトコルを消費し、型付き応答を結果に保持する。クリーンアップエラーがあればリモートエラーコードを保持して対応する Error を返し、エラーがなく `quiesced=false` ならプロトコルエラーを返す。`consumed=false` かつエラーなしもプロトコルエラーであり、セッションは消費しない。status/reset/transmit/disconnect は各呼び出し内で単一の期限を使い、失敗時は接続を閉じて要求を再送しない。リモートエラーの wire コードを保持する。CARD 操作は再実行しない。

## 独立 LNB 給電クライアント

`set_lnb_power(TimedClientTransport&, enabled, Timeout)` は同じ接続で HELLO/LIST/STATUS を行い、STATUS の epoch を使って opcode `0x0039` の `LnbPowerRequest` を request ID 4 で一度送る。HELLO から LNB 応答までひとつの単調時計期限を共有し、タイムアウト後に期限を更新しない。wire バージョンや feature ビットは追加しない。LNB 機能、デーモンの `--allow-lnb-power`、物理操作はサーバ所有者側の責任であり、本クライアントはそれらを代行しない。

成功時は応答の epoch と `enabled` が要求に一致した `LnbPowerResponse` を返し、接続を開いたままにする。この応答は制御要求に対するサーバの応答であり、15V 端子の実測・実機給電成功を意味しない。リモートエラーの raw コードを保持し、タイムアウト、EOF、不正な形式の応答、epoch/状態の不一致では既定応答を返して接続を閉じる。要求は再送しない。タイムアウト 0 は `INVALID_ARGUMENT`、期限算出が単調時計の uint64 範囲を超える場合は `TIMEOUT` となり、どちらも接続を閉じる。

期限付き recording 呼び出しでも、結果・デコードレコード・クライアントフレームは呼び出しスレッドのスタックを使い、上限付き交換ワークスペースはヒープ確保する。Linux x86_64 のフォーカスプローブは fake トランスポートと記録応答をプローブスレッド外のヒープに置き、GCC 14.2 Release と Clang 19.1.7 Debug ASan+UBSan の各 512 KiB スタックで正常な begin→stop→release と期限付き status EOF を確認する。これはそのコンパイラ/ビルド構成での実測に限り、小スタックを一般に保証するものではない。呼び出し側は実際のスタック予算、結果の配置場所、デコーダ、トランスポート実装の使用量を含めて余裕を確保する。PID 要求用スクラッチは上限付きヒープワークスペースを再利用し、最大の正当な PID リストを扱う。これは read-only 要求スクラッチの小さな上限とは別である。
