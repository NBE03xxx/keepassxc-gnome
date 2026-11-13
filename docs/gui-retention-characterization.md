# 残る描画メモリ保持の性質

Ubuntu 26.04での日常的な短時間の個人利用という [公開対象](release-scope.md) に向けた調査。前回の75分試験の増加基準の不合格を変更せず、保持されるものと解放条件を調べた。**描画キャッシュの保持・回収と、libadwaitaのbreakpoint解放漏れを確認した。** 以下で説明できない割り当てを説明済みには数えない。

## 採取条件

製品と同じWindowコードを使うprobeの独立コピーに、ASanの割り当て元採取だけを加えた。30分反復＋1分ロック待機を私有GNOME / Wayland / D-Busで実行する。実時間5分間隔の製品の整理処理を使い、未来時刻による整理・アニメーション無効化・描画方式変更は行わない。

各周期のロック完了時に採取する。ただし整理の直前・直後は別の状態なので、経過時刻と整理回数も記録する。上位200コンテキストの記録であり、全ヒープの完全な内訳ではない。表示されないコンテキストは0とみなさない。`tools/compare-gui-profiles.py`、`tools/summarize-gui-retention.py` はこの限界を結果へ記録する。

```bash
source tools/dev-env.sh
python3 tools/gui-memory-control.py profile --seconds 1800 --idle 60 --tag fc2
python3 tools/compare-gui-profiles.py build/gc/fc2-p1800 --from-minute 15
python3 tools/summarize-gui-retention.py build/gc/fc2-p1800
```

既存ログを上書きせず、実行中はソース・バイナリ・harnessを固定する。保存先は `build/gc/fc2-p1800/`。このプロファイルの傾きや中央値を前回の75分試験の代わりに用いない。

## Cairo：上限を持つ文字描画キャッシュ

**確認した事実。** 実行版Cairoは1.18.4-3、Build IDは `05177818680003739ec6cb6b7b3a77249b1aa633`。一致するUbuntu公式デバッグ情報で、`libcairo.so.2+0x75494` の割り当てが `src/cairo-scaled-font.c:2762` のglyph page作成であることを確認した。

公式1.18.4ソースとUbuntu `.dsc` のSHA-256が一致し、Ubuntuのpatch seriesは空だった。署名検証済みとは主張しない。対応は `.cache/gui-memory-sources/cairo-provenance.json` に記録した。

- `src/cairo-scaled-font.c:65–87`：全fontで共有するglyph page cache。通常の目標上限512ページ、各ページ32glyph。
- 同 `:358–368`、`:1384–1413`：他から参照されなくなったscaled fontを最大256個保持し、追加時には古いものを解放する。
- 同 `:2742–2806`：glyph pageを作成・登録する。`src/cairo-cache.c:169–180`、`:240–246`：凍結解除時等に回収する。利用中で回収できない場合や凍結中の一時的超過があるため、厳密な全メモリ上限とは異なる。

参照用ソースは `.cache/gui-memory-sources/cairo-1.18.4/`、公式配布元は [Cairo 1.18.4](https://cairographics.org/releases/cairo-1.18.4.tar.xz)。

`tests/cairo_glyph_cache_control.cpp` は、ほぼ同じ描画サイズでmatrixだけ異なる4,096個のfontを描画して参照を返す独立対照。DB・GTK・IPC・clipboardは使わない。上位報告中のページ数は192→384→512、その後512 / 1,024 / 2,048 / 4,096回時点で512を維持した。ページ本体は計3,956,736 bytes。これに付随するglyph画像等は別の割り当てで、全キャッシュのbyte上限をこの数値と同一視しない。

`bash tools/test-font-memory.sh --cairo-glyph-cache` で再実行する。最初のサンドボックス内実行では最終LSan自己検査が拒否された。実行可能な環境での再実行は描画対照を完走したが、677 bytes / 8件を報告し生終了1だった。**この単独対照はLSan合格ではない。** ログは `.cache/cairo-glyph-cache-control{,-unrestricted}.log` とそれぞれの `.status`。

## GTK：文字画像とfont参照をまとめて保持するキャッシュ

**確認した事実。** GTK 4.22.4、Build ID `4c74ae5215a17f78b116635961e03231361bf671` のコンテキストで、`g_malloc0` → `libgtk-4.so.1+0x63045c` の112 bytes単位の保持が増えた。対応バイナリの割り当てと後続のclass / cache / atlas / listフィールド設定は `gsk_gpu_cached_new_from_atlas()` に対応する。

対応ソースの参照位置：

- `gsk/gpu/gskgpucache.c:91–119`：キャッシュ内のlinked listとatlasへ割り当てを登録。
- `gsk/gpu/gskgpucachedglyph.c:62–83`、`:203–260`：fontポインター・glyph・flags・倍率をキーにし、fontと画像を参照付きで保持。各glyphにpaddingを加えてatlas内へ配置する。
- 同 `:28–60`：古いglyphはstaleとし、atlas解放時にfont・画像・glyphを解放する。
- `gsk/gpu/gskgpucache.c:154–190`：atlas内のglyphをまとめて解放。残る生存pixelや空き領域を回収の判断に使う。
- `gsk/gpu/gskgpudevice.c:12`、`:32–76`：既定15秒の回収処理。全体がstaleになるとcacheオブジェクト自体を解放する。

公式ソースは [GTK cache](https://github.com/GNOME/gtk/blob/4.22.4/gsk/gpu/gskgpucache.c)、[glyph](https://github.com/GNOME/gtk/blob/4.22.4/gsk/gpu/gskgpucachedglyph.c)、[device](https://github.com/GNOME/gtk/blob/4.22.4/gsk/gpu/gskgpudevice.c)。参照用コピーは `.cache/gui-memory-sources/`。

**技術的な推論。** 5分整理でPangoのmapが新しいfontを生成しても、以前のfontがGTKキャッシュから参照されていれば、atlas回収まで残る。解除・編集・ロックを短い間隔で続ける間と、画面更新を止めた待機では保持量が異なる。前回のロック待機後の減少と整合するが、上位コンテキストだけで残る増加の全量を証明したわけではない。

DB等を含まない `tests/gsk_glyph_retention_control.cpp` でも1,200フレーム・12回整理後に待機し、1,295個のglyphがstaleになることを観測した。60秒待機ではcurrent atlasの古さ判定（既定15秒×4）を確実に越えなかった。90秒待機の追加対照では、GTK自身のdebug出力でglyphとatlasの全回収（`Cached items`が空）まで確認した。`GSK_CACHE_TIMEOUT`は変更せず、既定15秒であることもログで確認した。加速対照の整理頻度は製品とは異なるため、製品の実時間試験や通常操作の性能確認の代わりにはしない。

対照のログは `build/gsk-retention-{control,idle90}/app.log`、`memory.jsonl`、`.cache/gsk-retention-{control,idle90}.log`。生LSanはともに終了134、初回60秒版は664,023 bytes / 16,255件、90秒版は649,163 bytes / 15,742件を報告した。この独立対照では所有関係の全件照合を行っておらず、LSan合格には数えない。

## libadwaita：breakpointの解放漏れ

**確認した事実。** 反復に比例して残る `adw_breakpoint_new()` → `g_hash_table_new_full()` のコンテキストも見つかった。これは上記の描画キャッシュと別の問題である。

検証版はlibadwaita 1.9.1-0ubuntu0.2、Build ID `dd624448cd87b1b782306781db8fce9926ed6d99`。対応1.9.1ソースの `src/adw-breakpoint-bin.c:649` は所有権を引き受けたbreakpointの配列を `g_ptr_array_new()` で作り、`:542–557` では配列だけを解放する。要素を解放する関数がなく、`:783` の登録・`:811` の除去でも対応するunrefがない。

公式の修正 [f2e30ed1ea41b2238acda1cf2af232f61a48a17d](https://github.com/GNOME/libadwaita/commit/f2e30ed1ea41b2238acda1cf2af232f61a48a17d)（2026-10-20）も同じ所有権の不足を修正し、関連するsignalの寿命も整えている。上流mainでは修正済み。実機のローカルAPT情報で候補も1.9.1-0ubuntu0.2だったことは確認したが、Ubuntu全リポジトリの将来の配布状況を保証するものではない。

**実測による確認。** `tests/dialog_lifetime_control.cpp` で、公開APIを使い各dialogへ1個のbreakpointの所有権を渡した。20個のdialogは20/20がfinalizeしたが、breakpointは0/20だった。表示直後に閉じる条件と3フレーム後に閉じる条件の両方で再現した。早期に閉じるタイミングだけが原因ではない。弱参照の消去に加えて、finalize時に呼ばれるqdataの解放カウンターで確認した。

製品と同じWindowの独立コピーでも、秘密値を記録せずにdialogのfinalizeを数えた。60秒・12巡＋60秒待機で37/37がfinalizeし、保持中dialogは0だった（`build/gc/dc1-d60/`）。この実行の所有関係照合も未説明0・生LSan終了134。LSanで報告された割り当てを説明できることは、到達可能なオブジェクトの不必要な保持まで否定する証拠にはならない。

**判断。** 実装中のダイアログ本体の参照残りと、依存ライブラリのレイアウト制御オブジェクトの解放漏れを区別できた。この漏れはdialog等を作る回数に応じて増え、時間だけで一定量ずつ漏れるものではない。プロセスの終了でOSが当該メモリを回収する。認証・編集フォーム本体の解放と入力消去の検査は成功しているが、あらゆる秘密コピーの不存在の証明とはしない。

この調査ではlibadwaitaへパッチを適用せず、通常環境のライブラリも更新しない。公開APIで渡した参照をアプリ側から余分にunrefする回避は、修正取り込み版での二重解放を招くため採用しない。取り込み版への移行後に対照を再実行する。今回は既知の依存問題として明示した検証候補を作り、VM→実機の確認へ進む。公開時の最終判断にこの制限を含める。

ログは `.cache/dialog-breakpoint{,-mapped}-control.log` と `build/dialog-{immediate,mapped}-control/app.log`。採取中のライブラリ・上流修正の情報は `.cache/gui-memory-sources/libadwaita-1.9.1/`。

## 30分反復の完走結果

実測1,801秒・358巡＋60秒ロック待機を完走した。製品タイマーによる整理は6回、ASan下の同期処理最大51.901 ms。毎巡の機能・入力消去・元ファイル不変を確認した。

| 採取状態 | 生存割り当て | 報告中のGTK glyph割り当て数 |
| --- | ---: | ---: |
| 15分・整理直前 | 22,369,710 bytes | 3,803 |
| 20分・整理直前 | 22,382,801 bytes | 4,064 |
| 25分・整理直前 | 22,777,961 bytes | 4,136 |
| 30分・整理直前 | 23,287,898 bytes | 4,093 |
| 1分ロック待機後 | 18,149,358 bytes | 198 |
| Window / Application破棄後 | 16,902,634 bytes | 198 |

glyphの数は固定Build IDの割り当て地点を持つ、報告上位コンテキストに含まれた分だけ。完全な全数とはしない。採取された上位コンテキストの割合も `retention-summary.json` に記録した。周期内のFontconfig保持、描画キャッシュ、breakpointの漏れが重なるため、全差分を一種類の割り当てへ帰属させない。

終了時305,502 bytes / 6,750件の報告を生存Fontconfig owner 553個とMesa owner 49個から説明し、未説明0だった。再報告6,700件は初回集合の部分集合、生LSan終了134を保持する。対象ソース・バイナリ・harnessは終了後もSHA-256が一致した。結果は `offset-result.json`、`profile-comparison.json`、`retention-summary.json`、`freeze-verification.json` に保存した。

## 判定の扱い

ライブラリが割り当て元という理由だけで安全と断定しない。ソースの所有・回収と観測を組み合わせ、説明できた部分・未知の部分を記録する。大きなDB、異なるフォント・ドライバー・別ライブラリ版、日単位の連続操作での総保持量上限は未検証。実際のパスワードDBは使用しない。

最新版のVMと実機の確認は、この調査結果を含む候補を固定してから実施する。75分試験の結果は [キャッシュ制御](font-cache-control.md) にそのまま保持する。
