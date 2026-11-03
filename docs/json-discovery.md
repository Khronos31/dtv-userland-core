# Discovery JSON 投影

`discovery_json.h` は、完全なスキャン結果を公開仕様の `userland.discovery` JSON 形状へと投影する。単一の `ListResponse` ではなくスキャンエンベロープ（scan envelope）を入力とするため、同一の観測スキャン全体を通じて筐体のグループ化とシリアルの一意性を判定する。

出力は物理階層を維持する:

```json
{
  "schema": {"name": "userland.discovery", "major": 1, "minor": 0},
  "producer": {"product": "px4", "version": "1.0.0"},
  "scan": {"complete": true, "issues": []},
  "enclosures": [{
    "id": 1,
    "model": "PX-Q3U4",
    "serial": "serial-value",
    "serial_unique": true,
    "status": "ready",
    "lnb_15v_supported": null,
    "readers": [{"reader": 0, "device": 2}],
    "devices": [{
      "id": 2,
      "usb": "0511:084a",
      "serial": "usb-serial",
      "bus": 1,
      "address": 6,
      "port": "1-2.3",
      "receivers": [{"receiver": 3, "device": 2, "local": 0,
                     "systems": ["ISDB-T", "ISDB-S"]}]
    }],
    "candidates": []
  }],
  "ungrouped_usb_devices": []
}
```

ID は JSON の数値である。VID および PID は小文字の 16 進数による `VID:PID` 文字列である。レシーバ構成が未知のデバイスは `receivers` を省略し、レシーバが 0 個であることが判明しているデバイスは空配列を出力する。本プロジェクタはスキャンによって提供された筐体とデバイスのみを出力し、欠落している親や子を勝手に創作することは決してない。

筐体の `id` および USB ファンクションの `id` は、STATUS 投影と同じ識別子の意味を持つ。レシーバレコードにおいて、`receiver` はレシーバ番号であり、`device` はそれを含む USB ファンクション ID を参照する。`local` はそのデバイス上でのレシーバのローカルインデックスである。リーダーの `device` も USB ファンクション ID を参照する。候補（candidate）の `device` は候補観測内での旧来のデバイススロットであり、筐体 ID ではなく、また候補が子デバイスであることを主張するものでもない。

`serial_unique` は、デーモン起動時にこの筐体をどのように選択すべきかを説明する: `true` は `--device serial:…` で選択できることを意味し、`false` は重複筐体または一致するシリアルによってシリアル選択が曖昧になるため、USB パスを指定する `--device usb:…` を使用すべきであることを意味する。`null` は既存の意味を維持する。すなわち、シリアルが観測されなかったか、またはスキャンが不完全で重複が観測されなかったことを意味する。観測されたシリアルについては、不完全なスキャンであっても、重複状態または一致するシリアルが存在すれば false となる。重複のない観測シリアルは、完全なスキャンの後にのみ true となる。これは本スキャン内の筐体間における一意性を表すものであり、グローバルな保証ではない。

初期のデーモン起動ではシリアル選択のみを受け付けていた。M1UR および S1UR ではシリアル `000000000000001` が共有される場合があり、その選択が曖昧になる。そのため USB パス選択のための `--device`/`--usb-path` オプションが追加され、現在は統合された `--device serial:`/`usb:` 形式で表現される。

`scan.complete` および `scan.issues` はスキャン全体の列挙状態を説明する。issues は安定したコードと人間が読める詳細を保持する。筐体ステータスはグループ化の状態を説明し、独立している: `ready`、`incomplete`、`duplicate`、または `invalid_observation`（あるいはアダプタが分類できない場合は `unknown`）。`candidates` は重複筐体に対する代替の USB 観測結果を保持する。それらの観測結果がその子デバイスであると主張されることはない。各候補はデバイススロット、nullable なシリアル、および nullable な bus/address/port を保持する。筐体に割り当てられない USB 観測結果は `ungrouped_usb_devices` に留まり、nullable な serial/model/location、VID:PID、および `usable`、`unsupported`、`invalid_serial`、`insufficient_speed`、`invalid_topology`、`open_failed`、`unknown` のいずれかのステータスを持つ。

`lnb_15v_supported` は筐体の静的な LNB 機能である。未知の場合は `null` であり、現在の `lnb_15v_enabled` ステータスではない。`readers` はリーダー構成が未知の間は省略され、その構成が判明している場合は配列（`[]` を含む）となる。各リーダーはリーダー番号と、それを提供する観測デバイスを特定する。レシーバ番号は、1 つのデバイス内を含め、筐体全体を通じて一意でなければならない。

`discovery_json_size` は入力全体を検証しサイズを計測する。エンコーダは書き込み前に検証と計測を行い、出力先不足には `BUFFER_TOO_SMALL` を返し、いずれの失敗時にも出力バッファを変更しない。この API は既存の `dtv::json` ライブラリの一部であり、出力専用ターゲットを追加するものではない。スキャン配列は借用されたポインタ/要素数のペアであるため、アダプタは固定の検出件数上限に縛られることなく観測されたすべての筐体を提供できる。いずれの呼び出しの期間中も、指し示された配列を変更してはならない。
