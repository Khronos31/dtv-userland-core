# control client

`dtv::control` はsocketなしgate componentとして、status API、期限付きrecording API、独立LNB power clientを提供する。呼び出し側が `ClientTransport` または `TimedClientTransport` を実装して注入する。このCore libraryはsocketやfile descriptor adapterを提供しない。`fetch_status` はHELLO/LIST/STATUSだけを要求し、ACQUIRE/TUNE/START/STOP/RELEASEは行わない。recording APIの `begin_recording`、`stop_recording`、`release_recording` はそれぞれ明示的にこれらの制御要求を行う。

`fetch_status` は同じtransportでHELLO、筐体scopeのLIST、STATUSを順に要求し、request ID 1、2、3を使う。optional featureなしでUDTV 1.0を要求する。HELLOで選択されたversion、系列内のepochとproducer product/version、canonical LIST、および `validate_status_response` によるLIST基準のSTATUSを検証する。LISTとSTATUSが公開する筐体/functionのidentity fieldも照合する。Modelは現行のname-only fieldで比較する。呼び出し前に未使用の新しいconnectionを渡すこと。

frameと一時payloadには1つの上限付きworkspaceを使い、各応答でpayload領域を再利用する。workspaceは呼び出し中だけ有効な約135 kB（約132 KiB）のheap領域で、最大control payloadのrequest frameとreply payloadを別々に保持し、確保には例外を使わない。確保できない場合は `RESOURCE_LIMIT` とdefault resultを返し、transportをcloseする。decodeしたframe viewはreceiverが進む前に処理し、返却するsnapshot/sessionはtyped recordを所有してtransport/workspace bufferを参照しない。`ClientTransport` は呼び出し側の所有物で、呼び出し中に有効でなければならない。`write_all` はview全体を書き終えて成功するか、errorを返す。`read_some` は読んだbyte数、stream終端の0、またはerrorを返す。成功readにはframeのfragmentや複数frameが含まれ得るが、clientは要求に一致しないframeを受け付けない。opcodeとrequest IDが一致するresponse/error-responseだけを処理する。

stack使用量は呼び出し側のthreadにも依存する。Linux x86_64のClang 19.1.7 Releaseでは、測定時にCMakeのRelease既定値 (`-O3 -DNDEBUG`) を `-O2 -DNDEBUG -fstack-usage` で置き換え、`fetch_status` frameが64,888 byte、共有records decoderの `decode_status_response` frameが54,248 byte、返却する `StatusFetchResult` が64,400 byteだった。単純合算は183,536 byte（179.2 KiB、約184 kB）で、caller自身のframeやruntime overheadは別に加わる。結果を呼び出しthread上のlocal変数に置く場合、これら全てを考慮する。通常の小stack testでは、結果をprobe thread内に置いた正常・EOF経路を最適化済み構成で256/512/768 KiB、無最適化構成で512/768 KiBのstackにより確認する。256 KiBは最適化済み構成での参考実測であり、全build typeやcallerへの保証ではない。Clang 19.1.7ではbuild type未指定の無最適化構成で256 KiB probeがstack overflowしたため、この構成では512 KiBから確認する。GCC 14.2 ReleaseではCMake既定の `-O3 -DNDEBUG` に `-fstack-usage` を追加して計測し、decoder frameは108,464 byte、`StatusFetchResult` を同じthreadに置く正常経路は約237 KiBだった。GCC 14.2 DebugのASan+UBSan (`-g -fsanitize=address,undefined -fno-sanitize=vptr`) では、独立したprobe計測で正常経路に約295 KiBを要した。sanitizer runtimeとinstrumentationによる増加分は構成・compiler依存であり、通常時のframe値から一律には算出できない。このためsanitizer時は小stack testのprobeを512/768 KiB、通常時は最適化済み構成で256/512/768 KiB、無最適化構成で512/768 KiBとし、両方でEOF早期失敗と正常経路を検査する。

ここでprobeのKiBは1024 byte、kBは1000 byteを表す。CMakeの通常Release既定は `-O3 -DNDEBUG`、Debug既定は `-g` である。`dtv_userland_control` と依存targetには `-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic` が加わる。前記のoptimization/sanitizer flagsは各測定で実際に使った追加または置換flagsで、全ビルドへ適用される設定ではない。read-only stack probeはLinux x86_64で実行し、これらは限定環境での測定であり、全compiler/OSへのstack保証値ではない。callerは結果の置き場所とdecoderを含む余裕を確保する必要がある。

失敗時はdefault snapshotとlocal errorを返し、remote error responseをdecodeできた場合はraw codeも返す。その際 `close()` を呼ぶ。未知のremote codeはraw codeを保持したまま `PROTOCOL_ERROR` に対応づける。成功時はtransportを開いたまま呼び出し側へ返す。requestの自動再試行はしない。このAPIはtimeout値やtimeout policyを定めず、blockingとcancelの挙動は注入transportの責任とする。

read-onlyの `fetch_status` はACQUIRE/TUNE/START/STOP/RELEASEを行わない。recording APIはACQUIRE/TUNE/START/STOP/RELEASEのみを行い、service呼び出し、ATTACH、TS_DATA、receiver操作、capture、USB access、製品接続は行わない。UDTVは同一release内の内部契約であり、releaseをまたぐ安定wire APIではない。

## 期限付きstatus/recording client

期限付き操作には別の `TimedClientTransport` を注入する。これは呼び出し側が所有し、`write_all`、`read_some`、`monotonic_ms`、`close` を実装する。既存の `ClientTransport` の純virtualと `fetch_status(ClientTransport&)` のsignature・blocking挙動は変わらない。Timed clientは同じtransport objectを借り、socketやfile descriptorを生成・所有しない。各read/write実装は渡された有限timeoutを守り、呼び出しがtimeout後もblockし続けないことが前提になる。

`fetch_status(TimedClientTransport&, Timeout)` は呼び出し開始時に単調時計のabsolute deadlineを1つ作り、HELLO/LIST/STATUSとfragment readを通じて同じ残時間を渡す。部分進捗でdeadlineは延びない。期限値0は `INVALID_ARGUMENT`、deadline算出が時計のuint64範囲を越える場合は `TIMEOUT` とし、どちらもtransportを閉じてdefault snapshotを返す。I/O timeout、EOF、decode/validation失敗もtransportを閉じ、snapshotを公開しない。成功時はsnapshotを返し、接続はcallerへ開いたまま返す。

`fetch_status(TimedClientTransport&, Timeout, required_features)` は同じHELLO/LIST/STATUS sequenceを一つの期限内で実行し、HELLOのrequired feature maskを送る。required bitを提供しないserverはHELLOで拒否する。応答が要求maskを満たさない場合も `UNSUPPORTED`、要求していないfeatureを返すなど応答maskが一致しない場合は `PROTOCOL_ERROR` とし、default snapshotでtransportを閉じる。`required_features == 0` は既存の期限付きoverloadと同じstatus-only negotiationである。非期限付き `ClientTransport` と既存virtualの形は変わらない。

`begin_recording` はその `fetch_status` を最初に使い、必要なら `RecordingStatusValidator` をACQUIREより前に呼ぶ。validatorの拒否はdefault sessionで返り、transportを閉じる。status段のdeadlineが終わった後、ACQUIRE/TUNE/START段には同じ長さの新しいdeadlineを作る。この2 budgetの動作は旧recording helperとの互換実装であり、begin全体を単一budgetにはしない。optionの数値default/range、`TuneRequest.timeout_ms` への製品側対応、OS socket timeoutとの換算は定義しない。

この2 budgetの分割は旧recording helperとの互換を保つための現実装上の選択で、将来の製品向けtimeout契約や永久のユーザー仕様を定めるものではない。新しいtimeout数値、default、製品optionとの換算は定義しない。

beginは1つの上限付きworkspaceを例外なしのheap確保で使い、既存status実装のreply decoder/validationを呼び出す。ACQUIRE/TUNE/STARTの失敗はdefault sessionとremote raw error codeを返し、transportを閉じる。開始成功後にのみepoch、lease、tune result/revision、stream ID、attach token、expiry、次request IDを含むcaller-owned sessionを返す。attach tokenは別data connectionのATTACHにだけ使い、clientはdata socketやATTACHを行わない。START成功はarmed受理を示すだけで、TS activeを示さない。

`stop_recording` はsessionのstreamへ1回のtyped STOPを送り、leaseを解放しない。`release_recording` はsessionのleaseへ別のtyped RELEASEを1回送る。両APIは失敗・応答不明のrequestを再送せず、成功時はtransportを開いたまま返す。応答が届く前にdaemonがSTART/STOP/RELEASEを適用した可能性はclientから判定できないため、timeout後に物理cleanup完了や `quiesced=true` を推定しない。disconnect後の回収はdaemon owner契約の責任である。

## CARD session client

`begin_card(TimedClientTransport&, CardClientOptions, Timeout)` はCARD_SERVICEをrequired featureとしてHELLOし、同じ接続でLIST/STATUS、readerの存在確認、CARD_STATUS、CARD_CONNECTを行う。HELLO/LIST/STATUSには一つの期限、CARD_STATUS/CARD_CONNECTにはその後に始める同じ長さの期限を使い、各区間内でdeadlineを延長しない。request IDは1–5を使う。readerがsnapshotにない場合やidentity constraintが一致しない場合は `NOT_FOUND` で接続を閉じ、readerやCARD capabilityを推定しない。成功時はdaemon epoch、reader、handle、generation、CARD_CONNECTが選択したprotocol、次request ID 6を `CardClientSession` に保持し、接続を開いたまま返す。

`card_status` はsessionのepoch/readerを使って現在generationと挿入・初期化状態を取得する。`card_disconnect` はsessionのepoch/reader/handle/bound generationを使い、responseが同じreader/handleを示して `consumed=true` の場合だけsessionのhandleとgenerationを消費する。status/disconnectそれぞれ一回の呼び出し内で単一deadlineを使い、失敗時は接続を閉じてrequestを再送しない。remote errorのwire codeを保持する。今回のclient incrementにはCARD_RESET/TRANSMIT APIは含めず、後続incrementで追加する。

## 独立LNB power client

`set_lnb_power(TimedClientTransport&, enabled, Timeout)` は同じ接続でHELLO/LIST/STATUSを行い、STATUSのepochを使ってopcode `0x0039` の `LnbPowerRequest` をrequest ID 4で一度送る。HELLOからLNB応答まで一つの単調時計deadlineを共有し、timeout後にdeadlineを更新しない。wire versionやfeature bitは追加しない。LNB能力・daemonの `--allow-lnb-power`・物理操作はserver owner側の責任で、このclientはそれらを代行しない。

成功時はresponseのepochと `enabled` が要求に一致した `LnbPowerResponse` を返し、接続を開いたままにする。このresponseは制御要求に対するserverの応答であり、15V端子の実測・実機給電成功を意味しない。remote errorのraw codeを保持し、timeout、EOF、malformed response、epoch/state不一致ではdefault responseを返して接続を閉じる。requestは再送しない。timeout 0は `INVALID_ARGUMENT`、期限算出が単調時計のuint64範囲を越える場合は `TIMEOUT` となり、どちらも接続を閉じる。

期限付きrecording呼び出しでも、結果・decode record・client frameはcaller threadのstackを使い、bounded exchange workspaceはheap確保する。Linux x86_64のfocused probeはfake transportと記録応答をprobe thread外のheapに置き、GCC 14.2 ReleaseとClang 19.1.7 Debug ASan+UBSanの各512 KiB stackで正常なbegin→stop→releaseとtimed status EOFを確認する。これはそのcompiler/build構成での実測に限り、小stackを一般に保証しない。呼び出し側は実際のthread stack budget、結果の置き場所、decoder、transport実装の使用量を含めて余裕を確保する。PID request scratchはbounded heap workspaceを再利用し、最大合法PID listを扱う。read-only request scratchの小さな上限とは別である。
