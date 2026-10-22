# 読み取り専用 control client

`dtv::control` にsocketなしgate componentの一部として `ClientTransport`、`StatusSnapshot`、`fetch_status` を追加する。呼び出し側が `write_all`、`read_some`、`close` を実装したtransportを注入する。このCore libraryはsocketやfile descriptor adapterを提供しない。

`fetch_status` は同じtransportでHELLO、筐体scopeのLIST、STATUSを順に要求し、request ID 1、2、3を使う。optional featureなしでUDTV 1.0を要求する。HELLOで選択されたversion、系列内のepochとproducer product/version、canonical LIST、および `validate_status_response` によるLIST基準のSTATUSを検証する。LISTとSTATUSが公開する筐体/functionのidentity fieldも照合する。Modelは現行のname-only fieldで比較する。呼び出し前に未使用の新しいconnectionを渡すこと。

frameと一時payloadには1つの上限付きworkspaceを使い、各応答でpayload領域を再利用する。workspaceは呼び出し中だけ有効な約70 KBのheap領域で、確保には例外を使わない。確保できない場合は `RESOURCE_LIMIT` とdefault snapshotを返し、transportをcloseする。decodeしたframe viewはreceiverが進む前に処理し、返却するsnapshotはtyped recordを所有してtransport/workspace bufferを参照しない。`ClientTransport` は呼び出し側の所有物で、呼び出し中に有効でなければならない。`write_all` はview全体を書き終えて成功するか、errorを返す。`read_some` は読んだbyte数、stream終端の0、またはerrorを返す。成功readにはframeのfragmentや複数frameが含まれ得るが、clientは要求に一致しないframeを受け付けない。opcodeとrequest IDが一致するresponse/error-responseだけを処理する。

失敗時はdefault snapshotとlocal errorを返し、remote error responseをdecodeできた場合はraw codeも返す。その際 `close()` を呼ぶ。未知のremote codeはraw codeを保持したまま `PROTOCOL_ERROR` に対応づける。成功時はtransportを開いたまま呼び出し側へ返す。requestの自動再試行はしない。このAPIはtimeout値やtimeout policyを定めず、blockingとcancelの挙動は注入transportの責任とする。

clientはACQUIRE/TUNE/START/STOP/RELEASE、service呼び出し、ATTACH、TS_DATA、receiver操作、capture、USB access、製品接続を行わない。UDTVは同一release内の内部契約であり、releaseをまたぐ安定wire APIではない。
