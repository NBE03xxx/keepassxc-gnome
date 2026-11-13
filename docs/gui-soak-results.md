# 長時間GUIメモリ検証

2026-11-13。手順1の実施記録。**本試験は完走した。機能・アクセス検査・終了時の所有関係照合は成功したが、保持量の継続増加を検出し、長時間安定性の合格とはしない。** 実施条件は開始前に [検証計画](gui-soak-plan.md) に固定した。

## 範囲と方法

ASanを有効にした、製品と同じWindowコードを使う `gnome_lifetime_ui_probe` を、私有GNOME / Wayland / session・system D-Bus・XDG設定で実行する。同じプロセス・同じウィンドウを保ち、約5秒ごとに解除、保護付き編集、コピー、フォーム、手動呼出しの自動ロック、暗号化復旧コピーの再認証を反復する。KDBX3の小さな合成fixtureを使い、毎巡元ファイルの不変も検査する。全画面・全形式・大規模DBの長時間試験ではない。

60分の反復後、DBをロック・一覧とフォームを消去した状態で15分待機し、最後にWindow / ApplicationのfinalizeとASan / LSanを確認する。自然な解放を観測するため、malloc_trimやフォントキャッシュの強制消去は本試験に加えない。

- 実行：`tools/test-gui-offset-audit.sh --soak 3600 vulkan 900`
- ワークロード：`tests/lifetime_ui_probe.cpp`。確認済みのワークロードに長時間モードを追加しただけで、製品のテスト用RPCは増やしていない。
- 数値採取：`tests/SoakMetrics.h`。`/proc/self/status`、FD・task・module mappingのメタデータ、ASanの生存割り当て量と確保ヒープ量を記録する。メモリ内容・秘密文字列を記録しない。FD値には計測用の一時FDも含むため、絶対値ではなく同条件での推移を評価する。
- 結果：`build/gma/soak-3600-vulkan/memory.jsonl`。`run-config.json`に依存の版・hash、ソースと実行バイナリのhash、要求時間を記録し、実行中に対象のソース・バイナリが不変であることを確認した。
- 集計：`tools/analyze-gui-soak.py`。最初の10分を除外し、10分窓の中央値と傾向を評価する。閾値は疑わしい増加を調べるためのもので、閾値以下でも全将来時間・全操作での上限を証明したとはしない。

ASanのRSSには検査用shadow/quarantineやアロケーター予約領域を含む。製品版の常駐メモリ量とは比較しない。GUIプロセスの計測であり、私有GNOME Shellや子プロセスのピークメモリ全体を測定する試験ではない。

## 本試験の結果

実測反復3,601秒で718巡、その後ロック待機900秒を完了した。全巡でワークロードの検査を通り、最後にWindow / Applicationのfinalizeを確認した。ASanのアクセスエラーはなかった。

| 区間 | 生存割り当て中央値 MiB | RSS中央値 MiB | FD中央値 | thread中央値 |
|---|---:|---:|---:|---:|
|10～20分 |22.579 |761.045 |32 |17 |
|20～30分 |23.558 |770.133 |32 |17 |
|30～40分 |24.299 |775.572 |32 |17 |
|40～50分 |25.227 |777.752 |32 |17 |
|50～60分 |26.046 |779.340 |32 |17 |

最初と最後の評価窓の差は **3,636,077 bytes（約3.468 MiB、約15.4%）**。warmup後の回帰傾向は約 **88.9 KiB/分** だった。事前に定めた「1 MiBまたは5%の大きい方」と「64 KiB/分」の両方に要調査フラグが付いた。閾値を後から変更して成功にはしない。集計の終了コード1はこの2フラグを示す。

15分待機では約26.122 MiBでほぼ一定となり、反復中の増加分は自然には解放されなかった。thread数は11へ減少した。Window / Applicationを解放した後は約25.183 MiB、FD数18となった。解放後のRSSはASanのquarantine等の影響で即座に下がるとは限らず、生存割り当てと同一視しない。

終了時の初回LSan報告6,769,078 bytes / 168,498件は、固定したFontconfigの生存owner 3,148個とMesaの生存DB 49個等からの参照により全件を説明できた。終了時の再報告168,458件がその部分集合であることも確認し、**未説明0 bytes**、所有関係の補助診断は成功した。生LSan終了134は保持する。

これは「到達できない割り当て」と「到達可能だが増え続けるキャッシュ」の別の問題である。今回の保持量増加は、終了時の所有関係照合が成功しても残件になる。

生ログは `.cache/gui-soak-hour.log`、終了時照合は `offset-result.json`、数値集計は `soak-result.json`、補正後の集計ログは `.cache/gui-soak-hour-analysis-corrected.log`。最初の集計は二進浮動小数の減算で `4501.48 - 3601.48` を `899.9999999999995` と評価し、待機不足扱いになった。記録された十進時刻をDecimalで減算し、900秒と正しく評価するよう修正した。probe自身の単調時計の実測値も3,601秒・900秒と照合した。判定時間や増加の閾値は緩めていない。元の集計失敗ログも保存した。

通常ビルドとCTest 21/21、shell / Python構文、`git diff --check`、固定上流の整合性検証は成功した。実行中のソース・バイナリと依存hashも一致した。

## 増加の追加調査

### 別プロセスの割り当てプロファイル

固定した試験ソースの別コピーにASanの `__sanitizer_print_memory_profile(100, 100)` を追加し、1・5・10・15分の割り当て元を採取した。プロファイルは上位100コンテキストであり、ワークロードの同一タイミングでの完全なヒープ差分ではない。数値比較だけで全増加分を断定しない。

1分の生存割り当て20,690,092 bytesから15分の24,101,482 bytesへ増え、共通の大きな増加コンテキストは `FcPatternDuplicate` → `pango_fc_font_map_reload_font` → `gsk_reload_font` → GPU glyph cacheの経路だった。Fontconfigの版・Build IDは既存の固定値と一致し、GTKの対応デバッグ情報もUbuntu公式debuginfodから取得した。

- GTK 4.22.4 / Build ID `4c74ae5215a17f78b116635961e03231361bf671`。
- `gsk/gpu/gskgpunodeprocessor.c:3661–3697`：描画倍率をglyph lookupへ渡す。
- `gsk/gpu/gskgpucachedglyph.c:186`：`gsk_reload_font`を呼ぶ。
- `gsk/gskprivate.c:45–118`：倍率をPangoのreload_fontへ渡す。
- Pango 1.57.0の `pango/pangofc-fontmap.c:1787–1829`：倍率に応じてFcPatternを複製し、uniquifyする。
- 同 `:2232–2250`：未登録のpatternを参照付きでpattern_hashへ格納する。同ファイル `:61–64` では保持期限を設けないキャッシュと説明されている。

GTK参照ソースは[公式タグ4.22.4](https://github.com/GNOME/gtk/tree/4.22.4/gsk)、Pangoは[公式タグ1.57.0](https://github.com/GNOME/pango/blob/1.57.0/pango/pangofc-fontmap.c)。ローカルの参照コピーは `.cache/gui-memory-sources/`。Pangoの[公開reload_font API](https://docs.gtk.org/Pango/method.FontMap.reload_font.html)の仕様とも対応する。対応デバッグ情報の関数・行番号と実測stackを照合したが、配布元の全パッチを再ビルドして完全一致させる作業ではない。

この補助プロファイル実行は `build/gp/900/` に保存した。初回の所有関係照合は5,516,862 bytes / 137,787件を全て説明したが、プロセス終了時の再報告に初回集合外のFontconfig由来4 bytes / 1件があった。`tools/check-font-offset-audit.py` はこれを拒否した。**この実行はLSan総合監査の合格には含めない。** 割り当て元の比較に限る参考資料として保持する。ライブラリ名や少量という理由で例外を追加していない。

### アニメーション無効の対照

同じワークロードの別コピーで、当該プロセスのGtkSettingsの `gtk-enable-animations` だけをfalseにし、5分＋15秒待機を実行した。通常環境のGSettingsは変更していない。本試験にも同じ変更を入れていない。

60巡、実測反復301秒＋待機15秒を完了。最初の1分と最後の1分の生存割り当て中央値は約14.705 / 14.915 MiB、最終巡14.937 MiBだった。終了時の所有関係照合は未説明0で成功した。`build/ga/300/`に結果を保存した。差は拡縮描画経路の影響を支持するが、この短い対照だけで無限時間の安定性を主張しない。

再現用の `tools/gui-memory-control.py` は元ソース・製品バイナリを変更せず、私有ディレクトリへ別コピーを生成する。`no-animation --seconds 60`の最小動作確認も成功した（`build/gc/n60/`）。プロファイル比較は `tools/compare-gui-profiles.py`。本試験のソースと実行バイナリのhashは、対照の作成後も不変だった。

### Pango単独の同倍率／異倍率対照

`tests/pango_scale_cache_control.cpp` はGTK、Qt、DB、IPC、クリップボードを使わず、独自に所有するPango font mapを使う。

| 測定点 | ASan生存割り当て bytes |
|---|---:|
| 同倍率でwarmup後 |796,604 |
| 同じ倍率をさらに1,000回reloadしてunref |796,604 |
| 異なる倍率1,000回を追加してunref |3,314,088 |
| 当該mapの公開cache_clear後 |780,809 |

同倍率での増分は0、異倍率で2,517,484 bytes増加した。公開cache_clear後に減少し、保持していた基底フォントのmetricsも有効だった。**この公開APIの呼出しは単独対照だけであり、製品・本試験への対策実装ではない。** sourceのpattern_hashと合わせ、異倍率の保持が増加原因になることを確認した。

`.cache/pango-scale-control.log`と`.status`に保存した。数値対照は成功したが、この単独プログラムの最終LSanは320 bytes / 3件を報告し、終了コード1だった。これをLSan0件成功とは数えず、GUI本試験の結果とも混同しない。

## Vulkan単独の128 bytes

[別記録](vulkan-unload-memory.md)で、MesaのCPU topology affinity表がmodule unload後に残ると特定した。1回128 bytes、10回1,280 bytes、moduleを保持した10回はLSan0件。製品へ強制dlopen等の回避策は追加しない。アプリ側の測定では各時点でMesa mappingが存在した。測定間の瞬間的なunloadを完全監視したわけではなく、本試験の終了時照合にもこの128 bytesは現れず、反復回数に比例する同種の増加は検出しなかった。常駐module内のonce初期化と整合する。unload問題そのものを修正したとは扱わない。

## 再実行

```bash
source tools/dev-env.sh
bash tools/test-gui-offset-audit.sh --soak 3600 vulkan 900
python3 tools/analyze-gui-soak.py build/gma/soak-3600-vulkan
bash tools/test-vulkan-lifetime.sh
python3 tools/gui-memory-control.py profile --seconds 900
python3 tools/compare-gui-profiles.py build/gc/p900
python3 tools/gui-memory-control.py no-animation --seconds 300
bash tools/test-font-memory.sh --pango-scales
```

補助対照も生の終了コード・未説明を保持する。LSanの自己検査と私有D-Bus / GNOMEを実行できる環境を使う。新しい依存パッケージの導入は不要。対照や集計のスクリプト自体を実行中に編集しない。

## 予備試験と記録の区別

最初の予備試験はWindow / Applicationを毎巡作り直す方式だった。本試験には同じWindowでの反復を使った。初回コピーの確認ダイアログが後のコピーでは省略される前提も、テスト側で修正した。

予備試験中に実行スクリプトを編集し、同じ予備実行が再度走る状態を招いたため、その専用process groupだけを停止し、結果には数えなかった。固定後の60秒＋15秒の予備実行は12巡・所有関係照合を完走した。本試験の開始は `run-config.json` のUTC時刻に基づき、予備試験の時間や途中値を混ぜない。


## 結論と残件

手順1の検証と原因調査は完了した。Vulkanの128 bytesは既存Mesaのmodule unload時の外部課題として特定できた。GUIの操作・秘密消去の検査、FD / thread数、終了時の所有関係照合は今回の範囲で確認できた。

一方、拡縮フォントの保持により、反復操作中に生存割り当てが継続増加した。**長時間メモリ安定性を完了扱いにはしない。** 最終レビューで、この保持量の制御を優先して扱う必要がある。公開cache_clear APIは単独対照で保持量を下げたが、製品への導入・周期・対象map・表示とアクセシビリティ・性能への影響は未実装・未検証である。アニメーションの強制無効化を製品の対策にしたわけではない。

今回の追加はテスト・解析・記録のみ。通常環境へのパッケージ導入、製品の描画・暗号・KDBX・保存処理や既存KeePassXCへの変更、実DBの使用、commit / pushは行っていない。

## 後続の制御実装

その後の利用者指示で、公開APIによる約5分間隔の整理を製品コードへ加え、別の75分試験を完走した。窓中央値の増加は3.468→2.126 MiBへ減り、ロック待機終了時は26.122→19.477 MiBとなったが、反復中の増加基準は1項目残る。**長時間安定性の残件は継続する。** 本書の整理なしの結果は履歴として保持し、実装・比較・限界は [キャッシュ保持量制御](font-cache-control.md) に記録した。
