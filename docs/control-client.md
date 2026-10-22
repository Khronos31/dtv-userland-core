# 読み取り専用 control client

`dtv::control` にsocketなしgate componentの一部として `ClientTransport`、`StatusSnapshot`、`fetch_status` を追加する。呼び出し側が `write_all`、`read_some`、`close` を実装したtransportを注入する。このCore libraryはsocketやfile descriptor adapterを提供しない。

`fetch_status` は同じtransportでHELLO、筐体scopeのLIST、STATUSを順に要求し、request ID 1、2、3を使う。optional featureなしでUDTV 1.0を要求する。HELLOで選択されたversion、系列内のepochとproducer product/version、canonical LIST、および `validate_status_response` によるLIST基準のSTATUSを検証する。LISTとSTATUSが公開する筐体/functionのidentity fieldも照合する。Modelは現行のname-only fieldで比較する。呼び出し前に未使用の新しいconnectionを渡すこと。

frameと一時payloadには1つの上限付きworkspaceを使い、各応答でpayload領域を再利用する。workspaceは呼び出し中だけ有効な約70 KBのheap領域で、確保には例外を使わない。確保できない場合は `RESOURCE_LIMIT` とdefault snapshotを返し、transportをcloseする。decodeしたframe viewはreceiverが進む前に処理し、返却するsnapshotはtyped recordを所有してtransport/workspace bufferを参照しない。`ClientTransport` は呼び出し側の所有物で、呼び出し中に有効でなければならない。`write_all` はview全体を書き終えて成功するか、errorを返す。`read_some` は読んだbyte数、stream終端の0、またはerrorを返す。成功readにはframeのfragmentや複数frameが含まれ得るが、clientは要求に一致しないframeを受け付けない。opcodeとrequest IDが一致するresponse/error-responseだけを処理する。

stack使用量は呼び出し側のthreadにも依存する。Linux x86_64のClang 19.1.7 Release (`-O2 -DNDEBUG -fstack-usage`) では、`fetch_status` frameが64,888 byte、共有records decoderの `decode_status_response` frameが54,248 byte、返却する `StatusFetchResult` が64,400 byteだった。単純合算は183,536 byte（約184 KB）で、caller自身のframeやruntime overheadは別に加わる。結果を呼び出しthread上のlocal変数に置く場合、これら全てを考慮する。今回の同compiler testでは、結果をprobe thread内に置いた正常・EOF経路を256/512/768 KiB stackで確認した。独立したGCC 14.2 Release (`-O3`) probe報告では、decoder frameが108,464 byteで、`StatusFetchResult` を同じthreadに置く正常経路は約237 KBを要した。GCC値はこの作業環境では再実行していない。stack probeはLinux x86_64で実行し、これらは限定環境での測定であり、全compiler/OSへのstack保証値ではない。今回測ったClang/GCC構成では、128 KiB workerに結果を含む通常呼び出しを置けない。callerは結果の置き場所とdecoderを含む余裕を確保する必要がある。

失敗時はdefault snapshotとlocal errorを返し、remote error responseをdecodeできた場合はraw codeも返す。その際 `close()` を呼ぶ。未知のremote codeはraw codeを保持したまま `PROTOCOL_ERROR` に対応づける。成功時はtransportを開いたまま呼び出し側へ返す。requestの自動再試行はしない。このAPIはtimeout値やtimeout policyを定めず、blockingとcancelの挙動は注入transportの責任とする。

clientはACQUIRE/TUNE/START/STOP/RELEASE、service呼び出し、ATTACH、TS_DATA、receiver操作、capture、USB access、製品接続を行わない。UDTVは同一release内の内部契約であり、releaseをまたぐ安定wire APIではない。
