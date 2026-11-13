# GUI の LSan 報告と Fontconfig の相対ポインター

2026-11-13。合成データだけを用いた Phase 3 の追加調査。GUI 全体の生の LSan は非ゼロ終了のままであり、本書の所有関係の検査を LSan の0件合格とは呼ばない。

## 確認済み：Fontconfig の報告には構造上の誤検出がある

対象は Fontconfig 2.17.1-3ubuntu1 / x86_64 LP64。実体の Build ID・SHA256・ソース SHA256 を `tests/font-offset-audit.json` に固定した。Ubuntu の[対応ソース](https://archive.ubuntu.com/ubuntu/pool/main/f/fontconfig/fontconfig_2.17.1.orig.tar.gz)を[署名対象の dsc に記載されたハッシュ](https://archive.ubuntu.com/ubuntu/pool/main/f/fontconfig/fontconfig_2.17.1-3ubuntu1.dsc)と照合した。署名自体の検証は今回実施していない。デバッグ情報は同じ Build ID の Ubuntu debuginfod から取得した。パッケージの導入は行っていない。

展開先は `.cache/gui-memory-sources/fontconfig-2.17.1-6d0a98982ec351c165c9224c8b7dbdfca3010e47/`。

| ソース位置（展開先に対する相対パス） | 確認した事実 |
|---|---|
| `src/fcint.h:155–184` | ポインターをオブジェクトからの相対オフセットとして格納・復元するマクロ |
| `src/fcint.h:231–238` | FcPattern の要素配列は `elts_offset` |
| `src/fcpat.c:484–509` | ヒープ上の要素配列も相対オフセットで保持 |
| `src/fcpat.c:1267–1284` | 公開 iterator は要素配列の通常のポインターを保持 |
| `src/fcint.h:403–414` | FcCharSet の leaves / numbers も相対オフセット |
| `src/fccharset.c:141–228` | 配列と leaf の確保・相対オフセット格納 |

通常のポインターを走査する LSan は、この相対参照を自動で復元できない。これは起点が Fontconfig という理由での推定ではなく、以下の最小対照で確認した。

| 対照 | 所有中の生 LSan | 同じオブジェクトの参照を通常ポインターにした場合 | 解放後 |
|---|---:|---:|---:|
| `tests/font_pattern_offset_control.cpp:10` | 310 bytes / 3件 | 公開 FcPatternIter を root に置くと0件 |0件 |
| `tests/font_charset_offset_control.cpp:12` | 84 bytes / 4件 | 固定版の相対参照を復元した root では0件 |0件 |

対照では所有者を global に保持し、所有を失っていないことを明示した。オブジェクトを解放して初めて成功扱いにする。ログは `.cache/offset-{pattern,charset}-control.log`。この対照だけが通常ポインターの root を追加する。次の GUI 診断では追加しない。

## GUI の割り当てごとの所有関係の照合

`tests/FontOffsetAudit.h:35` 以降の診断は、全GUI操作とウィンドウ・アプリの finalize が完了した後に、まず抑制なしの `__lsan_do_recoverable_leak_check()` を実施する。その生ログを保存してから、同じプロセスのメモリを読み取り専用で調べる。

- ASan の allocation metadata と、固定した FcPattern / FcCharSet のサイズ・件数・参照数・配列サイズを照合する。
- 生 LSan に含まれない生存オブジェクトから、ソースで確認した相対参照だけを復元する。報告済みの所有者を新しい root にしない。
- 報告済みの割り当て同士の通常ポインターも辿り、報告された各アドレスとサイズを照合する。検証した相対参照と末尾参照を `app.log.ownership-edges.tsv` に記録し、元の owner が生 LSan に含まれるかも区別する。
- 自身の匿名 private writable mapping のみを対象とし、1領域64 MiB・全体512 MiBで上限を設ける。`process_vm_readv(getpid(), ...)` は自分自身だけに使い、他プロセスは読まない。ダンプ抑止を解除しない。メモリ内容は保存しない。
- 生ログ・判定・プロセス終了時の再報告を分離する。終了時の報告に最初の検査になかった割り当てが加われば失敗させる（`tools/check-font-offset-audit.py`）。
- `LSAN_OPTIONS` の suppression、ignore API、リーク検査の無効化を使わない。製品の Fontconfig / Pango / display の終了処理も変更しない。

Mesa の空コンテキストについては、後述する固定版のDB構造から末尾参照も復元する。報告されたアドレスのサイズ・起点・空ヘッダー、報告にない生存 owner のサイズ・hash table・保持パス・UUID・alive を検査する。library名だけで除外する方式ではない。

これは固定した実装に対する補助診断であり、独立した汎用リーク検出器ではない。構造に合う生存 owner からの到達可能性を説明するもので、キャッシュの必要性・上限・全操作での寿命を証明しない。版・ABI・ハッシュが変わったら再調査する。診断は ASan の `gnome_lifetime_ui_probe` にだけ入り、製品バイナリの機能にはしない（`cmake/Frontend.cmake:111`、`tests/lifetime_ui_probe.cpp:227`）。

## 確認結果

Cairo は比較用に検証プロセスの環境変数で選択した。製品の描画設定は変更していない。

| 実行 | 生のスナップショット | 所有関係で説明 | 未説明 | 判定 |
|---|---:|---:|---:|---|
| GUI 1巡・Cairo | 2,604,535 bytes / 66,316件 |2,604,535 bytes |0 |補助診断成功 |
| GUI 10巡・Cairo | 2,604,535 bytes / 66,316件 |2,604,535 bytes |0 |全10巡完了、補助診断成功 |
| GUI 1巡・意図的リーク・Cairo | 2,604,915 bytes / 66,321件 |2,604,535 bytes |380 bytes / 5件 |意図どおり拒否 |
| GUI 7巡・Vulkan | 2,986,954 bytes / 75,698件 |2,986,954 bytes |0 |Mesaの49件も照合 |
| GUI 10巡・Vulkan | 2,987,115 bytes / 75,698件 |2,987,115 bytes |0 |全10巡完了、Mesaの49件も照合 |
| GUI 2巡・意図的リーク・Vulkan | 2,684,269 bytes / 68,261件 |2,683,889 bytes |380 bytes / 5件 |Mesaの49件を照合し、実際のリークは拒否 |

意図的リークはアプリ側の malloc 37 bytes と、所有を失った FcPattern 343 bytes（24+256+32+31）。後者も Fontconfig 起点だが説明済みには分類されなかった。ライブラリ名を丸ごと除外しないことを確認した。すべて生の終了コードは134のまま保存した。Vulkanもシェーダーキャッシュを有効にしたままである。

結果は `build/gma/{positive-1-cairo,positive-10-cairo,negative-1-cairo,positive-7-vulkan,positive-10-vulkan,negative-2-vulkan}/offset-result.json`、全件の照合は `app.log.offset-audit.txt`、初回と終了時の報告は `app.log.snapshot.log` / `app.log`。終了時の報告は初回の部分集合であることも検査した。通常 CTest は21/21成功（`.cache/offset-ctest.log`）、上流参照 checkout の固定コミットとファイル整合性も検証した。shell構文・Python構文・`git diff --check`も確認した。

## 確認済み：AMD Vulkan の空コンテキストも末尾参照で保持される

既定描画の実行では Fontconfig の2,671,608 bytesを説明できても、AMDドライバーに2,352 bytes / 49件（各48 bytes）が残った。`build/gui-memory-audit/offset-positive-1/app.log.offset-audit.txt` に未説明として保存し、補助診断も失敗させた。

同じ報告はプロジェクトのモデル・IPC・暗号・クリップボード処理を含まない `tests/gtk_memory_control.cpp` でも再現している（`build/ui-gtk-memory-control/app.log`）。Build ID `fa4e884aa7a23eaf80dc04ea27eb5da408865051` と一致する Ubuntu デバッグ情報により、起点 `libvulkan_radeon.so+0x339949` は `ralloc_context`、呼び出し元は Mesa 26.0.8-1ubuntu0.3 の `src/util/mesa_cache_db.c:694`、multipart cache の初期化・読出し、`disk_cache_get` と確認した。一部の DWARF 補助情報が未取得で、全ての型情報を解決したとはしない。

Vulkan の instance / device を作成・破棄するだけの `tests/vulkan_memory_control.cpp` では、同じ49件は再現せず、別の128 bytes / 1件を報告した。後者は module unload 後で stack の一部が未解決であり、2,352 bytes の再現証拠としては扱わない。結果は `build/gui-memory-audit/vulkan-{instance,device}.log`。GTK の描画・シェーダーキャッシュの経路を含まない対照である点に注意する。

[Ubuntu公式Mesaソース](https://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_26.0.8.orig.tar.xz)も[対応するdsc](https://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_26.0.8-1ubuntu0.3.dsc)のSHA256と照合した。展開先 `.cache/gui-memory-sources/mesa-26.0.8/` の次の箇所で参照方式を確認した。

- `src/util/ralloc.c:40–87,103–146`：48バイトのヘッダーを確保し、payload 0バイトの context でもヘッダー直後のポインターを返す。実バイナリの逆アセンブルでも `malloc(0x30)` と返却時の `+0x30` を確認した。
- `src/util/mesa_cache_db.h:31–40`：DBがmem_ctxを保持する。実バイナリからDBサイズ112、mem_ctxのオフセット88も照合した。
- `src/util/mesa_cache_db.c:143–154,347–395,461–468`：lock解除でFILEだけを閉じ、DB自体は維持する。新規ファイルの生成直後は保持中UUID欄が0の状態もある。これら正当な状態を拒否していた診断途中の失敗も保存した（`build/gma/positive-{2,3,4,5,6}-vulkan/` および `positive-10-vulkan-before-uuid-fix/`）。診断途中の失敗を製品の修正や合格として扱わない。
- `src/util/mesa_cache_db.c:694,740–747`：contextの作成とDB終了時の解放。

`tests/empty_context_control.cpp` の構造対照では、所有中の末尾参照だけだと48 bytes / 1件を報告し、通常の参照では0件、解放後も0件だった（`.cache/empty-context-control.log`）。Mesaそのものの代替実装ではなく、LSanの検出範囲を確認する最小モデルである。

GUIの49件では、全てについて対応する生存DBが見つかり、LSanに報告されていない112バイトのownerから、各48バイトの空contextの末尾を参照していた。FILEは閉じていたが、hash table、保持パス、UUIDとaliveの状態をソース定義と照合できた。リーク検査用のrootを追加するのではなく、初回の生報告後に読み取った参照を記録している。7巡のVulkan試験は2,986,954 bytes / 75,698件を全て説明し、未説明0件、生LSan終了134だった。`build/gma/positive-7-vulkan/{offset-result.json,app.log.ownership-edges.tsv}` に根拠を残した。新しい私有キャッシュからの最終10巡でも、2,987,115 bytes / 75,698件すべてを説明した。Vulkanの負の対照でも、Mesa49件は説明できる一方、意図的なアプリ・Fontconfigリーク380 bytesは残り、検出を確認した。

検証プロセスだけ `MESA_SHADER_CACHE_DISABLE=true` とした既定描画の1巡でも、2,352 bytesの報告が消え、Fontconfigのみになった。ただしキャッシュ無効化による成功だけで原因を確定せず、上記の実際のowner照合も必要とした。製品のシェーダーキャッシュ・renderer設定は変更していない。

**判定：固定したライブラリと検証した操作範囲では、Fontconfigの相対参照とMesaの空contextの末尾参照により、GUIの報告を割り当て単位で説明できた。これは補助診断の合格であり、生のLSanは依然0件ではない。** 他の版・ドライバー・全画面・無限時間でのキャッシュ上限は未検証とする。後の[60分反復＋15分待機](gui-soak-results.md)では、到達可能なPangoの拡縮フォント保持にも継続増加を観測したため、メモリ安定性は残件とした。GUIで同じ報告を確認していない単独Vulkan対照の128 bytesも、解決したとは扱わない。

## 再実行

私有 D-Bus / GNOME / LSan が利用できる実行環境で行う。新しい開発パッケージは不要。既存の私有開発環境と `build/reader-asan` 構成を使用する。

```bash
bash tools/test-font-memory.sh --offset-pattern
bash tools/test-font-memory.sh --offset-charset
bash tools/test-font-memory.sh --empty-context
bash tools/test-gui-offset-audit.sh --positive 1 cairo
bash tools/test-gui-offset-audit.sh --positive 10 cairo
bash tools/test-gui-offset-audit.sh --negative 1 cairo
env -u MESA_SHADER_CACHE_DISABLE bash tools/test-gui-offset-audit.sh --positive 10 vulkan
env -u MESA_SHADER_CACHE_DISABLE bash tools/test-gui-offset-audit.sh --negative 2 vulkan
# AMD ドライバーを導入済みの検証機だけの補助対照。非ゼロ終了を保存する。
bash tools/test-font-memory.sh --vulkan-instance
bash tools/test-font-memory.sh --vulkan-device
```

最初の試行では長い保存先パスが Wayland の108バイトのソケット名制限を超えて起動できなかった。未完走を成功には含めず、診断先を `build/gma/` へ短縮した。通常環境への再インストール、通常設定の変更、実DBの操作、上流の変更、commit / push は今回行っていない。
