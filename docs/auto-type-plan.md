# Auto-Type for GNOME Wayland

2026-11-21。v0.4.0の対象。検証には合成データを使う。

## 本家で使える部分

固定したKeePassXCの `upstream/keepassxc/src/core/Entry.cpp` にある `Entry::effectiveAutoTypeSequence()` を使う。エントリーと親グループの有効・無効、グループの既定値、エントリーごとの上書き、ユーザー名のみ・パスワードのみの既定値を本家と同じように扱う。フィールド内の参照式には同じファイルの `resolveMultiplePlaceholders()` を使用する。

本家のWayland実装は `upstream/keepassxc/src/autotype/wayland/AutoTypeWayland.cpp` と `upstream/keepassxc/src/gui/osutils/nixutils/RemoteDesktopPortal.cpp`。入力用セッションをRemote Desktopポータルから取得し、`NotifyKeyboardKeysym` を使う。上流の一式は `src/autotype/CMakeLists.txt` でQt Widgetsと結び付いているため、GTK側ではGIOでポータルを使う。

Wayland版の `activeWindowTitle()` は空を返し、他アプリの対象ウィンドウを列挙・選択しない。GTK側の最初の範囲は、利用者がエントリーを選び、入力先を自分でフォーカスし、GNOMEに登録したショートカットで入力を開始する操作。ウィンドウタイトルによる自動選択は現段階の対象外。保存されているウィンドウ関連付けは従来どおり保持する。

## 実装済みの入力シーケンス

`prototype/AutoTypeSequence.cpp` は上流の有効な設定を評価して、文字・Tab・Enter・待機に変換する。対応するパターンは、印字可能な文字、`{USERNAME}`、`{PASSWORD}`、`{TAB}`、`{ENTER}`、`{SPACE}`、`{DELAY ミリ秒}`、波括弧と修飾記号のエスケープ。フィールドの文字列は命令として解釈せず、制御文字は拒否する。対応範囲外のトークン、修飾キー付きシーケンス、期限切れ・ごみ箱・無効エントリーも拒否する。実行計画は最大512ステップ、明示待機の合計は5秒。途中まで作った配列は失敗時に消去して返す。

私有workerの `autoTypeSequence` は解除中の選択エントリーを取得し、`src/gnome/SessionProtocol.h` で結果を検証する。GTKから実行する場合は、入力先での明示操作が確認できてから計画を要求する。ポータル許可・ショートカット設定の段階では、資格情報を送らない。`tests/auto_type_tests.cpp` は継承、シンボルとUnicodeの解釈、制御文字と未対応命令、上限、不正応答、DBロックの拒否を合成データで確認する。

## GNOMEのポータル

T16ではGNOME Shell 50.1、xdg-desktop-portal 1.21.1、xdg-desktop-portal-gnome 50.0を使用した。キーボード用Remote Desktop v2、Global Shortcuts v1の両方を確認した。インストール済みのデスクトップIDでHost Registryに登録した接続は、プロセスをダンプ不可にしてからも、両セッションを作成できた。Remote Desktopにはキーボードだけを指定し、許可は永続化しない設定で試験した。

T16の専用GTK画面への試験では、利用者がキーボードを許可し、`NotifyKeyboardKeysym` が成功を返しても、英字・記号・日本語・Tabが各欄に届かなかった。VMの私有GNOMEセッションでは、同じポータルの `ConnectToEIS` を使うと英小文字・大文字・記号・Tabが入力欄に届いた。利用者の了承を得て `libei-dev` をプロジェクト内の開発SDKへ追加した。実行時にはUbuntuの既存 `libei1` を使う。

## GTKとEISをつなぐ実装

`src/main.cpp` はエントリーの選択と開始の確認を扱い、準備が済むとウィンドウ上部に案内を表示する。`src/gnome/NativeAutoType.cpp` が補助実行ファイルを起動し、`src/gnome/AutoTypePortal.cpp` がショートカット登録とキーボード許可を扱う。対象側でのショートカットの押下と修飾キーの解放の後、GTKがDB世代と自分のウィンドウのフォーカスを照合してから、私有workerに入力計画を要求する。補助プログラムへの転送は一回の有限長パイプ書き込みで行う。

`src/gnome/auto_type_worker.cpp` がEISで受け取ったキーマップから全手順を検証し、解決できるものだけを入力する。取消・期限・DBロック・画面ロック／スリープ・ポータルの終了通知で保留とセッションを破棄する。既定シーケンスにEnterが含まれるため、利用者へフォーム送信が起き得ることを案内する。入力先はその時点でフォーカスを持つウィンドウで、別アプリへのフォーカス移動をGTKで検知できるとは限らない。

Ubuntu 26.04.1 VMで、公開候補のdebから展開した実行ファイルにより、選んだ合成エントリーのユーザー名・パスワード・Tab・Enterが別GTKアプリに届くことを確認した。ショートカット前の欄は空で、準備後にアプリでDBをロックするとショートカットを押しても入力されなかった。分離GNOMEで権限画面を操作し、画面ロック・スリープ監視は試験用のD-Busサービスを使用した。

T16の日本語Waylandセッションでは、初回候補がショートカット後のキーボード状態の確認で停止した。修正版では修飾キーが離れるまで最大1.5秒待ってから入力処理を続ける。VMで `U`、`Alt`、`Ctrl` を時間差で離す試験と、DBロック時の取り消しが成功した。T16でも利用者がキーを操作し、合成フォームの2欄に期待値が入り、Enterを受け取った。

T16では日本語変換が有効な状態で、余分な入力が入ったとの報告もあった。EISはキーボードイベントを送る。入力先の変換状態はこの経路で取得できず、文字・Tab・Enterは入力先のIMEで処理されることがある。開始ダイアログ、準備完了の案内、利用ガイドに、入力先を直接入力に切り替える手順を加えた。[EISの入力経路](https://libinput.pages.freedesktop.org/libei/index.html)。

## IMEを一時的に切り替える方法の調査（2026-11-21）

Mutter 50.1の `src/backends/meta-eis-client.c` で、Auto-Typeが使うキーボードイベントの処理を確認した。IMEの取得・変更はこの経路に含まれない。IBus 1.5.34の `bus/inputcontext.c` では、`PropertyActivate` などを入力コンテキストの所有接続に限定している。IBusのグローバルエンジンだけを切り替える試験では、GNOMEの表示がMozcのままになった。最近使った入力ソースの設定値も、GNOMEが非対話的に切り替える経路を含まないため、現在の状態を判定する根拠には使えない。[Mutterの入力処理](https://github.com/GNOME/mutter/blob/50.1/src/backends/meta-eis-client.c)、[IBusの所有者確認](https://github.com/ibus/ibus/blob/1.5.34-rc2/bus/inputcontext.c#L1434)、[GNOMEの入力ソース管理](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/status/keyboard.js#L450)。

Ubuntu 26.04.1の隔離VMで、GNOME Shell内部の入力ソース切り替えを使い「日本語（Mozc）」から変換なしの「日本語」へ移した。合成IBusコンテキストでASCIIキーが変換されずに通り、Mozcへ戻した後は `あ` が確定した。Mozcを選択したまま `InputMode.Direct` を指定する試行では、期待した直接入力を確認できなかった。試験用のGNOME Shell内部呼び出しを使った結果で、本アプリとの連携、入力欄間の移動、未確定文字の保持、ロック・キャンセル・異常終了時の復元は未検証。

自動化の候補は、任意で有効にするGNOME Shell 50用拡張。扱うのはキーボード入力ソースと復元の状態で、データベースや資格情報は本アプリ内の経路で扱う。入力前にフォーカスと切り替え完了を確認し、取消時の復元を含めて用途ごとに検証する。GNOME Shell拡張が有効でない場合の動作は現行の手順を使う。今回、候補のアプリやT16の設定は変更していない。[GNOME Shell拡張の構成](https://gjs.guide/extensions/overview/architecture.html)。

## 同梱するGNOME Shell拡張

`packaging/gnome-shell/keepassxc-auto-type-ime@keepassxc-gnome/extension.js` を、同梱の任意拡張として配置する。利用者の有効化後に、GNOME ShellのセッションD-Busで、debに入った補助プログラムの接続元実行ファイルを確認する。拡張の入出力は開始・終了・取消の状態。パスワード、DB、クリップボードは扱わない。`src/gnome/AutoTypePortal.cpp` は入力用ヘルパーを保護する前に登録だけを行い、ショートカットを押した後に切り替えを要求する。`src/gnome/auto_type_worker.cpp` は確認を受けてからEISキーボードに接続し、適用済みのキー配列を取得する。

切り替えはフォーカス中のWaylandウィンドウの入力に限定する。Shellが把握する未確定変換がある場合や、別の入力コンテキストが選ばれている場合は開始を拒否する。入力ソース、変換エンジン、ウィンドウの変化を監視し、取消・終了・D-Bus接続切断時に外された入力ソースを復元する。パスワード専用欄でGNOMEがIMEを一時抑止する場合には、欄を離れたときの復元順を戻す。GNOME 50の内部APIを使うため、同梱メタデータの対象を50に限定する。[GNOME 50の入力ソース管理](https://github.com/GNOME/gnome-shell/blob/50.1/js/ui/status/keyboard.js)、[GNOME 50の変換状態管理](https://github.com/GNOME/gnome-shell/blob/50.1/js/misc/inputMethod.js)。

合成DBは `build/bin/gnome_auto_type_tests --write-ime-fixture /tmp/ime-synthetic.kdbx` で新規作成できる。解除用パスワードは `phase2-demo`。通常入力、ユーザー名の後で4秒待機する手順、Enterを送らない手順がある。VMにdebを入れ、READMEの手順で有効化したうえで、前後のMozcの状態と合成フォームへの入力を照合する。

[Remote Desktop Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)・[Global Shortcuts Portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)・[Host Registry](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.host.portal.Registry.html)。
