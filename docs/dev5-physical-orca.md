# dev5の実機更新・Orca音声確認

2026-11-14。**実機更新と、今回の範囲の作成・編集・検索・ロック・終了確認を完了。明示した伏せ字の条件では英字は読まれなかった。初回の文字読み上げは入力条件が不明で、原因未特定の観測として残す。** VMで通過した [dev5-1](session-guard-candidate-validation.md) と同じdebを使用する。

- OS認証を利用する `pkexec apt-get` でdev4-1からdev5-1へ更新した。追加依存はなく、前後の全パッケージ状態は開発アプリ以外で一致した。本家 `keepassxc` のパッケージ照会・検証状態も不変。
- 3実行ファイルのSHA-256は候補manifestと一致。保存済み合成DB `build/host-final.l6nr7R/fixtures/host-native.kdbx` のSHA-256は `86bc33f66719e5244f8cfbda17027ac5a1ab881d0c42ebc9f6dc2eb559a11191`。
- `/usr/bin/keepassxc-gnome` を通常のGNOME / Waylandで起動した。OSの `/etc/default/locale` の日本語LANGを使い、検証コマンドのC.UTF-8上書きを外した。起動前の翻訳検査は「準備完了」。state / cache / dataは従来の合成専用profileを使用する。
- 通常環境で本人PIDのsleep delay inhibitorを取得していること、GNOMEの初期ロック状態・logindのスリープ準備状態がfalseであることを確認した。FDOのGetActiveはGNOME 50で既知のNotSupported。新規作成ボタンが操作可能で、日本語の作成画面を表示した。

起動時刻は2026-11-14 08:34:58 JST、PIDは485780。開始時のOrcaは停止・screen-reader-enabled=false、配色prefer-dark、文字倍率1.2。利用者がSuper+Alt+SでOrcaを開始し、欄名と合成入力qzvの非読み上げを確認した。新しいDBは作成しなかった。次に同じ保存済み合成DBで、起動5分経過後の編集欄・検索を聴取した。終了確認までの単調時計の経過は1,762.1秒（約29分22秒）。音声の詳細ログや他アプリの内容は取得していない。

## 観測手段の注意

プロセスのdumpable抑止により、同一ユーザーでも `/proc/PID/exe` のreadlinkは拒否された。保護を変更せず、起動時に記録したPID・cmdline・開始tickを照合した。kernelの開始tickはサスペンドを含むBOOTTIME基準のため、単調時計との差を補正する必要があった。これは試験側の照合方法の調整であり、製品の保護を無効化したものではない。

監視警告のAT-SPIノードはSHOWING=trueだが、取得した実サイズは高さ0だった。ノードの存在だけで警告が画面に表示されていると判断せず、利用者にも見え方を確認した。利用者は警告が表示されていないと回答し、高さ0の非表示領域という観測と一致した。これをロック監視の障害とは扱わない。

## 作成画面の読み上げの切り分け

最初の回答は「欄名が読まれ、入力文字qzvも読まれる」だった。これを成功とは数えない。入力時の正確なフォーカス・伏せ字／表示状態は取得できておらず、後の自動取得時には入力欄が既に消えていたため、初回の原因は未特定として残す。

作成画面を開き直し、入力値を取得せずに3欄の名前とロールを確認した。データベース名は通常のtext、マスターパスワードと確認欄はpassword textだった（`src/main.cpp:583`、`:590`、`.cache/host-dev5-password-role-before-recheck.json`）。インストール済みOrca 50.3-0ubuntu0.1の `/usr/lib/python3/dist-packages/orca/typing_echo_presenter.py:841` は、パスワードロールの印字可能キーを読み上げない処理を持つ。設定を変更して読み上げを抑えたものではない。

利用者は、目のボタンがない確認欄、続いてマスター欄について個別に「文字は●で隠れ、英字は読まれない」と回答した。作成画面の欄名と、**明示的に伏せ字を確認した条件の文字非読み上げ**を確認した結果であり、初回の原因を推測して取り消したり、読み上げ不具合を修正したと扱ったりしない。追加質問への回答は「入力位置や表示状態は覚えていない」で、初回の原因を特定できなかった。

作成画面へキャンセル操作を送ったが、AT-SPIから閉鎖を確認する待機が失敗した。この自動操作を完了扱いにせず、利用者にEscでの閉鎖と、同じ合成DBの明示的なパス選択を案内した。新しいDBの作成・保存は要求していない。保存済み合成ファイルのhashは引き続き不変だった。

利用者が保存済み合成DBのパスを選び、パスワード入力画面を確認した。既知の合成パスワードで解除し、同じPIDのエントリー編集画面を起動から982.3秒時点で開いた。これは5分経過後の画面・音声確認の準備であり、通常アプリに整理回数の計測フックを追加しておらず、整理APIの実機呼出し回数の計測とは扱わない。

## 5分経過後の編集画面

利用者は同じ編集画面で、タイトル・ユーザー名・パスワードの3欄名を区別して聞け、パスワード欄の合成入力qzvが伏せ字になり、英字は読まれないと回答した。編集の保存は要求していない。元の合成ファイルのhashはこの時点でも不変。これは通常のデスクトップ・通常のOrcaによる今回の操作範囲の聴取であり、音声ログの自動判定や全画面の網羅試験ではない。

続いて、利用者にEscによる編集終了、Ctrl+Fでの検索欄の読み上げとHost finalの検索、Ctrl+Lでのロック、Super+Alt+SでのOrca停止を依頼した。利用者は検索の前にロックしたと回答したため、一括手順の成功とは扱わず、検索は未実施として残した。ロックの原因・操作順はこの回答だけで推定しない。

その時点の `tests/host_ui.py verify-lock` は成功。エントリー・編集画面・password欄・workerが無く、保存済み合成ファイルは不変だった。Orcaはまだ起動・有効だった。検索だけを先に確認するため、同じ合成DBの選び直しを案内した（`.cache/host-dev5-interim-{verify-lock.log,desktop.json}`）。

## 検索・最終ロック・終了

同じ合成DBを再解除し、今回は編集画面を開かず検索だけを依頼した。利用者はCtrl+Fで検索欄名が聞こえ、Host finalを入力して「合成アカウント / Host final」が結果に出たと回答した。その後こちらのPID限定操作でロックし、`verify-lock` でフォーム・一覧・workerの消去と保存済みファイルのhash不変を再確認した。検索前にロックした試行と、再解除後に検索できた試行を分けて記録する。

利用者がSuper+Alt+SでOrcaを停止した。自動確認でもscreen-reader-enabled=false、Orcaプロセスなしで開始前と一致した。検証アプリへ通常の閉じる操作を送り、PIDの消失を確認した。終了コード自体を回収したという主張ではない。配色prefer-dark・文字倍率1.2、開発アプリ以外の全パッケージ名・版・状態、本家パッケージの照会・検証状態、合成ファイルのhashも開始前と一致した。実機にはdev5-1を維持する。

## 判定と残件

今回の日本語の実機確認では、正常な監視開始とsleep猶予取得、保存済み合成DBの解除、作成・編集欄の名前、明示した伏せ字条件の英字非読み上げ、検索、ロック後の消去、終了を確認した。実際のOS画面ロック・スリープを今回再度実行したわけではなく、dev2等での過去の検証と区別する。英語の実機聴取、全画面・全支援機器の網羅を主張しない。

**初回のqzv読み上げは未解決の観測。** 入力位置・表示状態を特定できず、明示的な伏せ字での再確認では再現しなかった。利用者の誤操作、アプリ・GTK・Orcaの不具合のいずれとも断定しない。製品コードや設定を変えて修正したとは扱わない。再発時は合成入力だけで欄名・伏せ字状態・フォーカスを記録して切り分ける。この観測を含めて、実データ利用の合格・無条件の秘密文字非読み上げを宣言しない。

GUIの生LSan非ゼロ、長時間反復の増加基準未達、libadwaitaの既知の保持問題、クリップボードの外部コピー等は従来の制限のまま。実機の聴取結果を、それらの検査の合格へ変更しない。今回、製品コード・本家コード・依存の追加・システム言語・配色・読み上げ設定をプログラムから変更せず、本番DB使用・commit / push・公開も行っていない。

記録：`.cache/host-dev5-{preflight.json,plan-rechecked.log,install.log,install-result.json,desktop-before.json,desktop-after.json,initial-status.json,monitoring-observation.json,physical-progress.json,password-echo-observation.json,password-role-before-recheck.json,creation-speech-result.json,entry-speech-ready.json,entry-speech-result.json,search-unlock.log,search-ready.json,search-lock-result.json,final-lock.log,final-verify-lock.log,orca-stopped.json,close.log}`、`build/host-final.l6nr7R/{installed-dev5.json,dev5-physical-result.json,app-installed-dev5.log}`。
