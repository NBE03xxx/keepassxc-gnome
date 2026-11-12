# Phase 2 追加検証 — ダンプ抑止と GNOME デスクトップ連携

確認日 2026-11-07。Ubuntu 26.04.1、GNOME Shell 50.1 / Wayland、GTK 4.22.4、libadwaita 1.9.1、xdg-desktop-portal 1.21.1。

## 原因の確認

専用 Wayland / D-Bus セッションで、秘密を扱わない対照プログラムからポータルへ直接要求した。両条件で `RLIMIT_CORE` の上限は 0。変更したのは Linux の dumpable 属性だけ。

| dumpable | Settings.Read | FileChooser.OpenFile |
| --- | --- | --- |
| 1 | 成功 | リクエスト作成に成功し、対照プログラムで閉じた |
| 0 | `AccessDenied` | `AccessDenied` |

拒否の詳細は、ポータルがクライアントの `/proc/<pid>/root` を開けないことを示した。KeePassXC の `Bootstrap::disableCoreDumps()` は Linux で `RLIMIT_CORE` と `PR_SET_DUMPABLE` の両方を設定する。ポータルのプロジェクトも、呼び出し元の実行環境を識別できない場合にアクセスを拒否する挙動を記録している。 [KeePassXC の実装](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/core/Bootstrap.cpp#L79-L109)、[xdg-desktop-portal の Issue #1490](https://github.com/flatpak/xdg-desktop-portal/issues/1490)

導入済み GTK / libadwaita の小さな対照プログラムでも、dumpable が 0 のときに既定の外観取得が明暗設定に追従せず、`GtkFileDialog` の選択が完了しないことを再現した。GTK のポータル無効化 API はポータル実装向けと明記されている。試作は公開のアプリ向け API で境界を分ける。 [GTK のポータル無効化 API](https://docs.gtk.org/gtk4/func.disable_portal_interfaces.html)、[GTK のファイルダイアログ](https://docs.gtk.org/gtk4/class.FileDialog.html)

## 試作に加えた変更

- **明暗の追従** — libadwaita がシステムの配色を取得できる場合は従来の経路を使う。取得できず GNOME のスキーマがある場合は、`org.gnome.desktop.interface` の `color-scheme` を GSettings で読み取り、初期値と変更通知を `AdwStyleManager` に反映する。通常起動で設定を書き込む処理はない。 [GTK 試作](../prototype/gtk_probe.cpp)、[GSettings API](https://docs.gtk.org/gio/class.Settings.html)
- **ファイル選択** — ロック中に絶対パスで隣接する `phase2_file_picker` を起動する。補助プロセスは GIO で `FileChooser.OpenFile` を呼び、ファイル選択をデスクトップ側のポータルに委ねる。入力は試験用ディレクトリと親ウィンドウの識別子、返却はローカルパスだけ。先に `Request.Response` を購読し、応答ハンドルと URI を検証する。補助プロセスは GIO とその関連ライブラリにリンクし、メインが生成済みの合成ファイルだけを選択結果として受け入れる。 [補助プロセス](../prototype/file_picker.cpp)、[ターゲット定義](../prototype/targets.cmake)、[OpenFile 仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html#openfile)、[Request 仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Request.html)
- **ダンプ抑止** — メインは従来どおり上流の初期化を実行する。ポータル用の別プロセスにパスワード、鍵、DB ハンドルを渡す処理は追加していない。 [コア初期化](../prototype/CoreSession.cpp)

## 検証結果と範囲

- KeePassXC 2.8.0 の固定コミットと Phase 2 の `develop` 更新候補の両方でビルドした。固定版はコアの選定テスト 10 件、更新候補は合成 DB の往復テストが通過した。 [対象コミットと試験構成](phase2-results.md#ビルドとテスト)
- `--smoke-test-desktop` は、メインの `PR_GET_DUMPABLE=0` と `RLIMIT_CORE=0`、実際の Wayland バックエンド、起動中にメモリー上の GNOME 設定を明暗へ変更した場合の追従を確認する。その後、実際の `xdg-desktop-portal` に選択を要求し、模擬バックエンドが返した合成 DB のパスをメインが受け取って解除した。固定版と更新候補で通過した。実際のファイル選択 UI を手操作する試験とは区別する。
- 固定版で既存の期限付きコピー、クリップボード所有権、保存中のロック、再解除の試験も再実行し、通過した。
- 初回の統合試験で実 GNOME バックエンドが自動起動し、検索サービスが通常ホームのファイルのメタデータを参照したことをログで確認した。該当の専用セッションは終了を確認し、作業ログから通常ホームのファイル名を除いた。再試験では D-Bus のサービス自動起動を無効にし、ポータルと模擬バックエンドを明示起動した。
- ビルドは既存の `/tmp` のプレフィックスを再利用した。通常環境への開発パッケージ追加は不要だった。試験用ハーネスとログは `/tmp/keepassxc-phase2.YxadbPNq/portal-followup` に置いた。

## Wayland の親ウィンドウとキャンセル

- 選択の開始時に GTK ウィンドウの Wayland ハンドルを非同期に取得し、`wayland:<handle>` を補助プロセス経由でポータルへ渡す。応答後にハンドルを解放する。ハンドルを取得できない環境では、ポータル仕様に従い空の識別子を渡す。 [GTK 試作](../prototype/gtk_probe.cpp)、[GDK のエクスポート API](https://docs.gtk.org/gdk4-wayland/method.WaylandToplevel.export_handle.html)、[ポータルのウィンドウ識別子](https://flatpak.github.io/xdg-desktop-portal/docs/window-identifiers.html)
- ポータルがユーザーによるキャンセルを返した場合は、既に選択しているファイルとロック状態を保持する。親を閉じた場合は補助プロセスが `Request.Close` を送る。メインは補助プロセスの終了を待ってからハンドルを解放する。親を閉じる時点でまだハンドル取得中なら、取得結果を解放して終了する。 [GTK 試作](../prototype/gtk_probe.cpp)、[補助プロセス](../prototype/file_picker.cpp)、[Request.Close の仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Request.html#close)
- 固定版と `develop` 更新候補の両方で、実際のヘッドレス GNOME 50 コンポジターと `xdg-desktop-portal` を使用した。模擬バックエンドが受け取った識別子を GDK で取り込み、テストウィンドウを親に関連付けた。選択して解除する試験と、キャンセル後に再度開き、親を閉じたときにバックエンドが `Request.Close` を受け取る試験が通過した。
- 作業用環境が残っている場合は `bash /tmp/keepassxc-phase2.YxadbPNq/portal-followup/run-desktop.sh smoke-test-picker-lifecycle` で固定版を再試験できる。第二引数に `develop-build` を指定すると更新候補を使う。

## 実際の GNOME ファイル選択ダイアログ

GNOME Shell 50.1、xdg-desktop-portal-gnome 50.0、Nautilus 50.2.2 の実バックエンドを、ヘッドレス Wayland と専用 D-Bus で動かした。GNOME バックエンドから Nautilus への `OpenFile` に、親の Wayland ハンドルと合成ファイルの初期フォルダーが渡ったことを確認した。 [GNOME バックエンドの実装](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/main/src/filechooser.c)、[Nautilus のポータル実装](https://github.com/GNOME/nautilus/blob/main/src/nautilus-portal.c)

| 操作 | 結果 |
| --- | --- |
| 親ウィンドウから開く | 固定版と `develop` で実ダイアログが表示され、親からダイアログへキーボードフォーカスが移った |
| Escape でキャンセルして開き直す | 両ビルドでキャンセルが試作へ届き、親にフォーカスが戻った。再度ダイアログを開けた |
| 合成 KDBX を選ぶ | 両ビルドで Tab と Select を使って合成ファイルを確定し、試作で解除できた |
| 要求が開いた状態で親を閉じる | 固定版で実ポータルが `Close` を処理し、親と Nautilus のダイアログが消え、試作が正常終了した |

操作と状態確認には、専用コンポジターの仮想入力と AT-SPI を使った。ホストのホームと通常のデスクトップソケットはマウント名前空間で隠し、Nautilus の拡張も試験側では読み込めないようにした。セッションバスでサービスの自動起動を無効にし、必要なサービスだけを起動した。Nautilus が検索インデクサーのサービスに接続できないことをログで確認した。読み取り専用のマウント内で AppArmor ポリシーの問い合わせが失敗するため、試験専用の二つの D-Bus 設定で AppArmor によるバスの仲介を無効にした。開発パッケージの追加と試作コードの変更は不要だった。

作業用環境が残っている場合は `bash /tmp/keepassxc-phase2.YxadbPNq/real-dialog/isolate.sh exercise` で固定版の検証を再実行できる。第二引数に `develop-build` を指定すると更新候補を使う。親の終了を試す場合は第一引数を `shutdown` にする。

通常のデスクトップで人が操作する試験、システム設定画面からの明暗切り替え、実ファイルの上書きは未確認。

## 画面ロックとサスペンド前の通知

試作はセッションバスで `org.gnome.ScreenSaver.ActiveChanged`、システムバスで `org.freedesktop.login1.Manager.PrepareForSleep` を購読する。送信元のサービス名とオブジェクトパスを指定し、真の通知でロックを要求する。上流でもこの二つの通知を購読し、真のときにロックを通知する。 [試作の購読処理](../prototype/gtk_probe.cpp#L238-L240)、[試作の通知処理](../prototype/gtk_probe.cpp#L286-L309)、[上流の購読処理](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/osutils/nixutils/ScreenLockListenerDBus.cpp#L37-L63)、[上流の通知処理](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/osutils/nixutils/ScreenLockListenerDBus.cpp#L112-L128)

GTK 側は一覧と入力欄を消し、試作自身が現在所有するクリップボードの provider を手放す。解除や別の合成ファイルへの Atomic 保存が進行中なら、応答の表示を抑え、先行処理が戻ってから同じ Qt ワーカー上の DB を閉じる。保存していない変更は試作の手動ロックと同様に破棄する。選択ダイアログが開いている場合は補助プロセスに閉鎖を依頼し、途中の選択結果は破棄する。 [GTK の応答処理](../prototype/gtk_probe.cpp#L355-L370)、[選択結果の処理](../prototype/gtk_probe.cpp#L399-L469)、[ロック処理](../prototype/gtk_probe.cpp#L609-L713)、[コアのロック](../prototype/CoreSession.cpp#L111-L118)、[コアの保存](../prototype/CoreSession.cpp#L151-L155)

### 確認済み

固定版と `develop` 更新候補の両方で、ヘッドレス GNOME 50.1 と実際の `xdg-desktop-portal` を起動し、専用のセッションバスと模擬システムバスから通知を送った。ホストのホームと D-Bus ソケットを隠し、合成 KDBX だけを使用した。クリップボードの試験では仮想キー入力をアプリに届けてからコピーした。入力なしの初回試行では、ヘッドレス環境でコピー後の所有権を確認できなかった。 [試験モード](../prototype/gtk_probe.cpp#L908-L1019)

| 試験 | 結果 |
| --- | --- |
| 解除の応答前に画面ロック通知 | 一覧と入力欄が空のまま DB が閉じ、明示的に再解除できた |
| コピー後に画面ロック通知 | 試作が所有していた GDK クリップボードの provider が消えた |
| 保存の応答前にサスペンド前通知 | 保存したコピーを残してロックし、後でコピーを再解除できた |
| サスペンド復帰通知 | 解除済みの状態は維持し、ロック済みの DB は自動解除しなかった |
| ファイル選択中に画面ロック通知 | 模擬バックエンドが `Request.Close` を受信し、既に選択されていたパスが維持された |

固定版の選定コアテスト 10 件と `develop` のコア試作テスト 1 件も再実行して通過した。両ビルドで配色追従とファイル選択の試験、固定版で選択キャンセルと親ウィンドウ終了の試験も通過した。通常環境への開発パッケージ追加は不要だった。現行の再実行手順は次節に記載する。

## サスペンドの猶予とフォーカスを失ったクリップボード

### logind の遅延 inhibitor

試作のウィンドウが開いていて logind を利用できる間は、GIO で `Inhibit("sleep", …, "delay")` を非同期に呼び、返された Unix ファイル記述子を保持する。サスペンド前通知では画面を直ちに隠し、進行中の解除や保存を待ち、Qt ワーカーが DB を閉じてから記述子を解放する。DB が閉じていれば直ちに解放する。復帰通知で取得し直し、DB の解除はユーザーの操作を待つ。取得が拒否された場合も通知によるロックを試みる。 [記述子の取得と解放](../prototype/gtk_probe.cpp#L45-L94)、[logind のサービス監視](../prototype/gtk_probe.cpp#L240-L254)、[ロック完了との同期](../prototype/gtk_probe.cpp#L637-L712)、[systemd の仕様](https://systemd.io/INHIBITOR_LOCKS/)

**確認済み。** 固定版と `develop` で、テスト専用システムバスの模擬 logind が実際にパイプのファイル記述子を渡した。別プロセスで、サスペンド前通知を送る時点で記述子を保持し、保存中の合成 DB をロックした後はパイプが閉じ、復帰後に再取得し、試作の終了時に再び閉じることを確認した。固定版で `AccessDenied` を返しても、通知で DB をロックし、保存済みコピーを再解除できた。両ビルドでコア試作、配色とファイル選択のテストも通過し、固定版では選択のキャンセルと親ウィンドウ終了を再確認した。

**未検証。** 通常の logind による実機のサスペンド・復帰、サービス再起動、猶予の上限を超える保存は試していない。logind が待つ時間にはシステム設定による上限があり、猶予の取得前や取得に失敗した場合も実機が停止する前にロックを完了できるとは限らない。 [systemd の仕様](https://systemd.io/INHIBITOR_LOCKS/)

### フォーカスを失った後のクリップボード

**確認済み。** 固定版と `develop` の両方で、ヘッドレス GNOME 50.1 の別 GTK プロセスを前面にし、合成文字列を読み取れることを確認してから、試作に模擬画面ロック通知を送った。試作で DB のロックが完了した後も、前面のプロセスは改めて同じ文字列を読めた。コピーから 6 秒後にも再読できた。Wayland の記録では、試作が元のデータソースを破棄し、新しい空の選択を要求すると、Mutter が新しいデータソースをキャンセルした。対照試験では、試作がフォーカスを維持した状態でロック通知を受けた後、別 GTK プロセスが読んだクリップボードは両ビルドで空だった。 [試作の消去処理](../prototype/gtk_probe.cpp#L609-L630)、[GTK 4.22.4 の Wayland 実装](https://github.com/GNOME/gtk/blob/4.22.4/gdk/wayland/gdkclipboard-wayland.c#L164-L194)

**技術的な判断。** Mutter 50.1 はフォーカスを持たないクライアントの選択変更を拒み、標準のテキスト形式をコピーして元の所有者がいなくなるとその内容を再提供する。コードと通信記録は、試作の provider が解放された後に別プロセスから再読できた結果と一致する。現行の GDK によるコピーは、ロックやタイマーによるパスワードの消去を製品機能として約束できる状態にない。今回の試作は合成データでこの制約を再現した。実際のロック画面を表示したときの挙動と、クリップボード履歴サービスは未検証。 [Mutter のフォーカス判定](https://github.com/GNOME/mutter/blob/50.1/src/wayland/meta-wayland-data-device.c#L1036-L1067)、[Mutter のコピー保持と再提供](https://github.com/GNOME/mutter/blob/50.1/src/core/meta-clipboard-manager.c#L81-L156)

通常環境への開発パッケージ追加は不要だった。作業用環境が残っている場合は `bash /tmp/keepassxc-phase2.YxadbPNq/sleep-delay/isolate.sh` で固定版の模擬 logind 試験を再実行できる。第一引数に `develop-build` を指定すると更新候補、第二引数に `clipboard-focus` を指定すると別 GTK プロセスからの読み取り試験になる。フォーカスを維持する対照試験は、読み取り試験のコマンドに環境変数 `PHASE2_TEST_FOCUSED_CONTROL=1` を付けて実行する。

## フォーカスを失ったクリップボードの対策比較

GNOME Shell / Mutter 50.1、GTK 4.22.4、xdg-desktop-portal 1.21.1 と GNOME バックエンド 50.0 で検証した。マウント名前空間で通常のホームと D-Bus ソケットを隠し、専用 Wayland と D-Bus を起動した。二つの GTK プロセス間で合成文字列をやり取りし、送信側がフォーカスを失った状態で消去を要求した。ポータルの試験には実際の GNOME の許可画面を使った。比較プログラムは一時ディレクトリに置いた。

### 標準の Wayland クリップボード

| 候補 | 確認結果 |
| --- | --- |
| 通常の UTF-8 テキストと `x-kde-passwordManagerHint` | 別の GTK プロセスで文字列を読めた。フォーカスを失った送信側が消去しても、Mutter が同じ文字列を再提供した |
| `UTF8_STRING` のみ、または `text/plain;charset=UTF-8` のみ | いずれも、消去前から GTK 4 の通常のテキスト読み取りで互換形式がなかった |
| `wl-copy -c` | 通常環境には未導入。試験セッションが広告する Wayland グローバルに `ext_data_control_manager_v1` と `zwlr_data_control_manager_v1` は見つからなかった。実コマンドでの消去は未検証 |

固定版と `develop` の上流 KeePassXC は、通常の Qt コピーに秘密扱いの MIME ヒントを添え、Wayland の消去経路では必要に応じて `wl-copy -c` を起動する。試験した Mutter が永続化の候補として列挙するテキスト型は `text/plain` と `text/plain;charset=utf-8`。GTK 4 が UTF-8 ロケールで通常の文字列として受け付ける型もこの二つなので、形式の差だけで永続化を避ける案は採用候補から外した。 [上流の MIME ヒント](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/Clipboard.cpp#L47-L72)、[上流の Wayland 消去経路](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/osutils/nixutils/NixUtils.cpp#L407-L436)、[wl-clipboard のデバイスと一時ウィンドウの分岐](https://github.com/bugaevc/wl-clipboard/blob/v2.2.1/src/wl-copy.c#L219-L299)、[Mutter の型一覧](https://github.com/GNOME/mutter/blob/50.1/src/core/meta-clipboard-manager.c#L25-L57)、[GTK の文字列デシリアライザー](https://github.com/GNOME/gtk/blob/4.22.4/gdk/gdkcontentdeserializer.c#L914-L936)

### Remote Desktop ポータル

上流は Auto-Type のポータル経路に任意設定のクリップボード連携を持つ。コピー時はテキスト型を広告し、他アプリが所有者になった通知を受け取った場合は内部状態を更新する。消去時は自分が所有していると判断した場合に `SetSelection` に空の MIME リストを渡す。試験では公開ポータル API を直接呼び、`SelectDevices` を省略したセッションで画面共有を選ばず、`Start` の結果が `clipboard_enabled=true`、`devices=0` になることを確認した。GNOME 50.0 の許可画面では、この条件でも `Share` を有効にするために「Allow Remote Interaction」をオンにする必要があった。 [上流の既定値](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/core/Config.cpp#L85-L89)、[上流のポータル連携](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/osutils/nixutils/RemoteDesktopPortal.cpp#L156-L217)、[上流の所有者変更処理](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/gui/osutils/nixutils/RemoteDesktopPortal.cpp#L450-L465)、[Clipboard の公開仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Clipboard.html)、[GNOME 許可画面のボタン制御](https://github.com/GNOME/xdg-desktop-portal-gnome/blob/50.0/src/remotedesktopdialog.c#L59-L122)

| 消去要求 | フォーカス喪失後の結果 | セッションを閉じた後 |
| --- | --- | --- |
| `mime_types=[]` を送信し、送信元の文字列も空にする | 別 GTK プロセスは空文字列を受け取った。公開 D-Bus 呼び出しは成功を返したが、バックエンドのログでは Mutter が空リストを拒否していた | 元の文字列が再び読めた |
| `mime_types` を省略して所有権を手放す | 元の文字列が再び読めた | 元の文字列が読めた |
| 同じテキスト形式に空文字列を新規登録する | 空文字列を受け取った | 空文字列を受け取った |

空文字列を新規登録する方式では、所有者変更通知を処理した後なら、別の GTK プロセスがコピーした新しい値を保持できた。一方、古い所有者情報を使って消去する条件を意図的に再現すると、別の GTK プロセスの新しい値も上書きした。公開 `SetSelection` に所有者を条件として指定する引数はなく、Mutter は新しいデータソースを設定すると前の所有者を置き換える。消去と他アプリのコピーを競合させても安全に使えることが、採用判断に残る。 [ポータルがバックエンドの完了を待たない実装](https://github.com/flatpak/xdg-desktop-portal/blob/1.21.1/src/clipboard.c#L200-L217)、[空リストの拒否](https://github.com/GNOME/mutter/blob/50.1/src/backends/meta-remote-desktop-session.c#L1117-L1137)、[所有権の設定と解除](https://github.com/GNOME/mutter/blob/50.1/src/backends/meta-remote-desktop-session.c#L1412-L1456)、[Mutter の所有者の置き換え](https://github.com/GNOME/mutter/blob/50.1/src/core/meta-selection.c#L100-L148)

### ダンプ抑止との両立

試験用プロセスの起動直後から `PR_SET_DUMPABLE=0` にすると、ポータルの `CreateSession` は `/proc/<pid>/root` を開けず `AccessDenied` を返した。別の条件では、`RLIMIT_CORE=0` にした後、ポータルを使う前に試験用の `.desktop` ID を `org.freedesktop.host.portal.Registry.Register` で登録した。その後に `PR_SET_DUMPABLE=0` とし、同じ D-Bus 接続でクリップボードの許可取得、コピー、空テキストへの置き換えまで確認した。ポータルのログは接続に対応するアプリ情報をキャッシュから取得していた。再起動と接続変更の結果は次節に記載する。既存 GTK 試作への組み込みは未検証。 [Registry の公開仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.host.portal.Registry.html)、[アプリ情報登録の実装](https://github.com/flatpak/xdg-desktop-portal/blob/1.21.1/src/registry.c#L55-L131)

**評価。** 標準の GTK コピーは、GNOME 50.1 でフォーカスを失ったときに消去を保証できない。ポータルで空テキストに置き換える方式は、試験した条件ではセッション終了後も元の内容を再提供しなかったが、他アプリのコピーとの競合、許可画面、ポータル再起動時の復旧が残る。実パスワードを扱う段階での既定方式は保留する。実際のロック画面、ブラウザーやクリップボード履歴サービスとの組み合わせは未検証。

通常環境への開発パッケージ追加は不要だった。作業用環境が残っている場合は `bash /tmp/keepassxc-phase2.YxadbPNq/clipboard-options/isolate.sh hint` で MIME ヒントを再試験できる。ポータルは同じスクリプトに `portal-registered-protected` を指定し、`PHASE2_CLEAR_STRATEGY` を `empty-list`、`unset`、または `blank` に設定する。セッションを閉じた後の読み取りは `PHASE2_CLOSE_AFTER_CLEAR=1`、古い所有者情報の試験は `PHASE2_FORCE_STALE=1` を使う。

## ポータルの再起動と D-Bus 接続の変更

GNOME Shell / Mutter 50.1 と前節のポータルを、ホストのホームと D-Bus ソケットを隠した専用セッションで起動した。比較用 GTK プロセスはアプリ ID を事前登録してからダンプ抑止を有効にし、別 GTK プロセスで合成文字列を読み取った。ポータル本体または GNOME バックエンドのどちらか一方を `SIGKILL` で停止し、再起動によってその D-Bus 所有者が変わったことを確認した。再登録を試す直前にも `PR_GET_DUMPABLE=0` と `RLIMIT_CORE=0` を確認した。

### 確認済み

| 条件 | 既存の保護済みプロセス | 別 GTK プロセスからのクリップボード読み取り |
| --- | --- | --- |
| ポータル本体を停止して再起動 | 元の D-Bus 接続でも `Register` と `CreateSession` は `/proc/<pid>/root` を開けず拒否された。新しいプロセスでは起動時に登録し、改めて許可を得られた | 本体の停止中と再起動後、いずれも約 1.8 秒で読み取りを打ち切った。その後、旧 GNOME バックエンドも停止すると元の合成文字列を読めた |
| 空テキストへの置き換えを確認してからポータル本体を停止して再起動 | 同じ理由で再登録を拒否された。新しいプロセスでは登録し直せた | 本体の停止中と再起動後は読み取りを打ち切った。旧 GNOME バックエンドも停止した後に読めたのは空テキストだった |
| GNOME バックエンドだけを停止して再起動 | 本体側のアプリ情報はキャッシュに残り、新しい `CreateSession` は成功した。バックエンド再起動後の `RequestClipboard` は `Invalid state` を返し、待ち時間を設けた再試験でも同じだった | バックエンドの停止中に元の合成文字列を読めた |
| 同じ保護済みプロセスから、同じバスに別の D-Bus 接続を追加 | 新しい一意名の接続では登録とセッション作成を拒否された | 登録済みの元の接続では同じセッションでコピーの提供を続けられた |

ポータル本体のアプリ情報は接続の一意名で管理される。公式仕様はサービス再起動時の再登録を案内しているが、この構成では保護済みプロセスの再登録時に再び呼び出し元の検出が必要になり、アクセスが拒否された。読み取りの時間切れを消去成功と判定すると、後で元の文字列が読めるケースを見逃す。バックエンド再起動後の `Invalid state` の直接原因と、セッションバス自体を再起動した場合の挙動は未確認。 [Registry の再登録仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.host.portal.Registry.html#org-freedesktop-host-portal-registry-register)、[登録時の呼び出し元検出](https://github.com/flatpak/xdg-desktop-portal/blob/1.21.1/src/registry.c#L55-L96)、[アプリ情報キャッシュの検索](https://github.com/flatpak/xdg-desktop-portal/blob/1.21.1/src/xdp-app-info-registry.c#L95-L155)、[クリップボード要求の状態判定](https://github.com/flatpak/xdg-desktop-portal/blob/1.21.1/src/remote-desktop.c#L143-L163)、[Mutter のコピー再提供](https://github.com/GNOME/mutter/blob/50.1/src/core/meta-clipboard-manager.c#L128-L156)

### ファイル選択ヘルパー

固定版と `develop` 更新候補の両方で、実際の Nautilus のファイル選択中にポータル本体を停止した。既存のヘルパーは D-Bus 所有者の消失を検知して終了し、GTK のメイン画面に選択失敗が表示された。古いダイアログも閉じた。ポータル本体を再起動し、同じメインプロセスからファイル選択を開き直して、合成 KDBX の選択と解除を確認した。ヘルパーを新規起動する構成なら、ファイル選択はメインプロセスの事前登録に依存しない。クリップボードでは秘密そのものを転送するため、パスだけを渡すファイル選択と同じ境界として扱うには追加設計が要る。 [ヘルパーのポータル監視](../prototype/file_picker.cpp#L88-L120)、[GTK 側の結果処理](../prototype/gtk_probe.cpp#L434-L499)

通常環境への開発パッケージ追加は不要だった。作業用環境が残っている場合は `bash /tmp/keepassxc-phase2.YxadbPNq/portal-recovery/isolate.sh` に `frontend-live`、`frontend-cleared`、`backend-live`、`new-connection` または `file-picker` を指定すると再試験できる。ファイル選択の第二引数は `upstream-build` または `develop-build`。
