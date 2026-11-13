# 拡縮フォントキャッシュの保持量制御

2026-11-13。利用者の指示により、[長時間試験で確認した保持量増加](gui-soak-results.md)への制御を実装し、同条件の75分試験を完走した。**保持量は改善したが、反復中の増加が事前基準を超えるため、長時間安定性は未合格。**

## 選択した方式

`src/gnome/FontCache.h` にGTK用の保守処理を追加した。Windowが既に使っている1秒タイマーから期限を確認し、約5分間隔で、実際のWidgetのPangoContextが使うfont mapへ `pango_fc_font_map_cache_clear()` を適用する。

- 本家KeePassXC・Pango・GTKのソースを変更しない。Pangoの公開APIだけを使う。
- 一般の `pango_font_map_changed()` はbackendへの変更通知用であり、この版のFc backendではキャッシュを消さない。Fcの `cache_clear()` はキャッシュを再初期化し、fontsetとfamily modelの変更を通知する（参照ソース `.cache/gui-memory-sources/pangofc-fontmap.c:2604–2631`）。
- GTKメインスレッドで、単調時計の5分間隔を確認する。別スレッドからの誤用を拒否し、整理中の通知で同じ期限の処理が再入しないよう、期限を先に更新する。
- Windowはweak referenceで追跡し、処理中のWidgetとfont mapだけを一時的に参照する。破棄済み・未realizeのWindowには処理しない。Fontconfig以外のbackendには型を確認して適用しない。
- font mapのshutdown、FcFini、displayやrendererの終了・再作成は行わない。アニメーション・明暗・言語・フォント設定を変更しない。
- 5分ごとの整理は瞬間的なメモリ上限を保証するものではなく、今回の反復で増え続けた履歴を定期的に解放する方式。大きな一回の入力や別のキャッシュの総量は、読み込み上限等の既存の制御と区別する。

公式APIは [PangoFc.FontMap.cache_clear](https://docs.gtk.org/PangoFc/method.FontMap.cache_clear.html)、[Pango.FontMap.changed](https://docs.gtk.org/Pango/method.FontMap.changed.html)、[Gtk.Widget.get_pango_context](https://docs.gtk.org/gtk4/method.Widget.get_pango_context.html)。公開ドキュメントの現行版だけで判断せず、実行対象Pango 1.57.0の対応ソースと短い対照も確認した。

`src/main.cpp` のWindow生成・既存タイマー・破棄の寿命内に配置した。データベースのロック・保存・暗号処理・workerのプロトコルは変更していない。`cmake/Frontend.cmake` で既存のPangoFT2への直接リンクを追加した。ビルド環境にはGTKの開発依存として既に存在し、新規パッケージのインストールは不要だった。

実装の参照位置は `src/gnome/FontCache.h:12`（周期）、`:21`（公開API）、`:30`（スレッド・期限・Widget寿命）、`src/main.cpp:131`（Window所有）、`:298`（自動ロック確認後の呼出し）。検査は `tests/font_cache_tests.cpp:27` と `tests/lifetime_ui_probe.cpp:173`。

## 短い検証

- `tests/font_cache_tests.cpp`：4,000個の拡縮フォントを作成・解放しながら8回整理する。保持中の日本語・英語レイアウトの大きさ、基底フォントの有効性、font mapのserial更新と指定DPIの維持を確認する。GUI表示を必要としない。
- `tests/lifetime_ui_probe.cpp`：資格情報・編集中のpasswordとnotesを持つ実Windowのfont mapを整理し、次のイベント処理後もテキスト・カーソル・フォーカスが保たれること、5分前や同じ期限で再度整理しないことを確認する。その後のロック・秘密消去・復旧も継続して検査する。
- 最初の試行では、GtkTextの初回フォーカスで遅延実行される選択処理と、カーソル検査の設定が重なった。入力とフォーカスは同じでカーソルだけ末尾だった。初回選択のイベント処理を待ってから検査位置を設定し、整理後の次イベント処理での維持を確認する方式にした。製品へカーソルを操作する回避コードは加えていない。
- 日本語3巡と英語1巡の整理中編集・ASanアクセス検査・終了時所有関係照合が成功し、未説明0だった。`build/gma/fc-positive-3-vulkan/`、`build/gma/fce-positive-1-vulkan/`。生LSan終了134を保持する。
- 新しいテストの通常ビルド一覧への登録漏れで一度22件目がNot Runとなった。登録を直し、CTest 22/22が成功した。ログは `.cache/font-cache-ctest-final.log`。失敗した実行は成功に含めない。

## タイマーと長時間検証

長時間モードでは、編集試験用の未来時刻による整理は実行しない。製品の既存タイマーが実時間で呼ぶ処理だけを使う。`memory.jsonl` に `font_cache_clears` と `longest_font_clear_us` を追加し、実行回数と最大同期所要時間を記録する。実行用の環境変数やテスト用RPCを製品へ追加しない。

前回の反復60分＋ロック待機15分と同じ条件・要調査閾値で比較する。最初の10分を除外した10分窓の中央値、傾き64 KiB/分、増加1 MiBまたは5%の大きい方、FD / thread数の増加を評価する。新しい `--require-font-cache` は、実際に5分タイマーが動いたこと・過剰な頻度で整理していないことも確認する。

旧ログを上書きしないよう `KPXC_GUI_RUN_TAG=fc` で別の保存先へ記録する。実行中に対象ソースやスクリプトを変更しない。

```bash
source tools/dev-env.sh
KPXC_GUI_RUN_TAG=fc bash tools/test-gui-offset-audit.sh --soak 330 vulkan 15
KPXC_GUI_RUN_TAG=fc bash tools/test-gui-offset-audit.sh --soak 3600 vulkan 900
python3 tools/analyze-gui-soak.py build/gma/fc-soak-3600-vulkan --require-font-cache
```

330秒の予備試験は66巡＋15秒待機を完走し、実時間301.791秒で最初の整理を観測した（`build/gma/fc-soak-330-vulkan/`）。本試験は `build/gma/fc-soak-3600-vulkan/` に保存した。通常環境への再インストールや実機の再確認は行っていない。

### 75分試験の比較

前回と同じ718巡、実測3,601秒の反復＋900秒のロック待機を完走した。最初の整理は実時間で約5分後、整理は計14回で、製品のタイマーだけが動いた。対象ソース・バイナリ・スクリプト11ファイルは終了後も開始時SHA-256と全件一致した（`freeze-verification.json`）。

| ASan生存割り当て | 整理なし | 5分整理あり |
| --- | ---: | ---: |
| 10〜20分の中央値 | 22.579 MiB | 21.061 MiB |
| 20〜30分 | 23.558 MiB | 21.404 MiB |
| 30〜40分 | 24.299 MiB | 21.792 MiB |
| 40〜50分 | 25.227 MiB | 22.345 MiB |
| 50〜60分 | 26.046 MiB | 23.187 MiB |
| 最初／最後の窓の増加 | 3.468 MiB | 2.126 MiB |
| ウォームアップ後の傾き | 88.897 KiB/分 | 60.572 KiB/分 |
| ロック待機終了時 | 26.122 MiB | 19.477 MiB |
| Window / Application破棄後 | 25.184 MiB | 18.100 MiB |

中央値の増加は約39%減、傾きは約32%減だが、増加量は依然として事前の約1.053 MiBの要調査基準を超える。傾き64 KiB/分の基準には該当しなくなった。**閾値を変更せず、長時間安定性は合格扱いにしない。** 約5分ごとの整理だけで総保持量の増加を止めたとは言えない。

反復中のFDは32、threadは14で一定。ASan環境で整理の同期処理は最大53.461 msだった。GUIメインスレッドで実行するため、この時間だけ処理を占有する。通常ビルドで同じ遅延とは断定せず、整理後の再整形・再描画まで含むフレーム時間も未測定である。

ロック待機中は約1分後から19.477 MiBで横ばいだった。threadは8、FDは32、最後にWindow / Applicationを破棄するとFDは18へ減った。RSSはASanの予約領域・検査用領域・mapping等も含むため、この表の生存割り当てや通常アプリの使用量と同一視しない。

毎巡の解除・保護付き編集・コピー・秘密消去・復旧再認証・元ファイル不変と、最後のWindow / Application破棄を検査した。ASanのアクセスエラーはなかった。終了時LSanの初回報告296,908 bytes / 6,540件を、固定版Fontconfigの生存owner 548個とMesaのowner 49個から全件説明でき、未説明0。終了時の再報告6,498件は初回集合の部分集合だった。**生LSan終了134はそのまま残し、LSan 0件成功とは扱わない。**

所有関係照合の実行コードは0（`.cache/font-cache-soak-hour.status`）。増加基準の集計は1（`.cache/font-cache-soak-analysis.status`）で、`soak-result.json` の `investigation_flags` に中央値増加の1項目が残る。後者をツール障害や成功に読み替えない。生ログは `.cache/font-cache-soak-hour.log`、`.cache/font-cache-soak-analysis.log`、本試験ディレクトリの `app.log`、`memory.jsonl`、`offset-result.json`。

分離sessionから通常portal settingsを参照できない警告も生ログに残る。今回の試験は私有GNOME・D-Bus・設定内で行っており、通常ログインのportal接続や描画環境の再検証ではない。

## 描画キャッシュとの区別

**ソースで確認した事実。** GTK 4.22.4 の `gsk/gpu/gskgpucachedglyph.c:28–60` は、glyph cacheがfont参照を保持し、atlas内のglyphはatlas解放時に回収すると定める。`gsk/gpu/gskgpucache.c:154–190` はatlas内のglyphをまとめて解放し、古い／生存pixelの少ないatlasを回収する条件を持つ。参照用コピーは `.cache/gui-memory-sources/`、公式ソースは [glyph](https://github.com/GNOME/gtk/blob/4.22.4/gsk/gpu/gskgpucachedglyph.c#L28) と [atlas](https://github.com/GNOME/gtk/blob/4.22.4/gsk/gpu/gskgpucache.c#L154)。

**技術的な推論。** Pangoの整理で新しい基底fontが作られても、以前のfontが描画キャッシュから参照されていれば、直ちに全て解放されるとは限らない。今回のPango制御をGTK描画キャッシュ全体の上限保証とは扱わない。残る増加の全てがこの経路かどうかは未検証である。GTKの非公開データを製品から直接消去する実装は加えない。

## 判定と残件

- **実装・検証済み：** 公開APIによる5分間隔の保持量制御、既存の入力・ロック・復旧への短い回帰、75分の同条件比較。指定DPIを追加検査した最新版の単体試験も4,043チェックで成功した（`.cache/font-cache-dpi-test.log`）。
- **残件：** 反復中の生存割り当ては2.126 MiB増えた。[後続調査](gui-retention-characterization.md)で描画キャッシュの保持・回収とlibadwaitaのbreakpoint解放漏れを確認した。依存ライブラリの根本修正は未適用で、総保持量上限は未検証。周期だけを短くして解決扱いにせず、今回の候補を長時間安定性の完成版とはしない。
- **未検証：** 新しい整理処理の通常ビルドのフレーム時間、5分経過後の実機表示・Orca音声、大規模DB・全画面・別依存版・日単位の上限。短い編集検査は音声聴取・描画pixel比較の代わりではない。

本家・GTK・Pangoの参照ソース、通常環境の既存KeePassXC・導入済みdeb・設定、本番DBには変更していない。新規パッケージの導入、commit / pushは行っていない。通常環境への反映を行う際は、先にUbuntu 26.04 VMで候補debを検証する。
