# KeePassXC GNOME Native Edition — Phase 1 移植可能性報告書

調査日 2026-11-07 ／ 対象環境 Ubuntu 26.04、GNOME 50、Wayland

**判定は条件付きで実現可能。** KeePassXC 2.8.0 の内部静的ターゲット `keepassxc_core` を固定コミットからビルドし、C++ アダプター越しに GTK4 / libadwaita の画面を新設する方式を第一候補とする。CLI が同じコアでデータベースを扱う構造を確認した。安全な再利用の判定には、GTK と Qt のイベント処理を分けた状態での保存・ロックの競合試験、KDBX の双方向互換性試験、上流更新のリハーサルが必要。Phase 2 の試作を評価対象とし、製品実装への移行は別途判断する。

> Phase 2 の実測では、GNOME Wayland でフォーカスを失ったアプリのコピー内容が、DB のロック後も別アプリから読み取れた。対策比較と製品化に残る条件は [デスクトップ追加検証](phase2-desktop-followup.md#フォーカスを失ったクリップボードの対策比較) を参照。
>
> 保存の追加検証では、Atomic 保存でも外部の更新と公開が競合すると新しい版を上書きすることを再現した。 [外部更新と保存失敗時のデータ保全](phase2-results.md#外部更新と保存失敗時のデータ保全)
>
> 2026-11-10 の VM・実機試験を踏まえた判定は [§12 実測を踏まえた判定](#12-実測を踏まえた判定) に記録した。

本文の **確認** は固定コミットのソースまたは公式仕様で確認した事実、**評価** はそこからの設計上の推論、**未検証** は実行または追加調査が必要な事項を指す。機能表の難易度と保守負担は評価である。ソースのパスと行番号は特記がなければ §1 の固定コミットに対するもの。

## 1. 調査対象と実施範囲

| 項目 | 記録 |
| --- | --- |
| 公式リポジトリ | [keepassxreboot/keepassxc](https://github.com/keepassxreboot/keepassxc) |
| 既定ブランチ | `develop`。調査時のリモート HEAD は `6bcb901aefdd6c7a525466d7dfe099c5ebc13c0e`。ブランチの先端は参照先の確認のみ |
| 分析対象 | 安定リリース `2.8.0`、2026-10-21。タグの参照先コミット `1c2f23ef5914c0f442a00533def1689857afb5a7`。タグオブジェクト `fef511f45c191745b3822e2585de8e050daeada2`。タグから取得した detached HEAD を分析 |
| バージョンの根拠 | [`CMakeLists.txt:17-23`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/CMakeLists.txt#L17-L23)、[公式リリース 2.8.0](https://github.com/keepassxreboot/keepassxc/releases/tag/2.8.0)、[固定ソースツリー](https://github.com/keepassxreboot/keepassxc/tree/1c2f23ef5914c0f442a00533def1689857afb5a7) |

再取得する場合は `git clone --depth 1 --branch 2.8.0 https://github.com/keepassxreboot/keepassxc.git <調査用ディレクトリ>` の後に `git rev-parse HEAD` で上記コミットと照合する。今回は安定した評価基準を得るためリリースタグを選び、取得先は一時ディレクトリにした。作業開始時に `AGENTS.md` を確認した。指定の `docs/phase1-instructions.md` が作業ディレクトリに存在しなかったため、依頼本文をフェーズ指示として使用した。

調査はソース・ビルド定義・テスト定義の静的分析。実行環境は Ubuntu 26.04.1 で、CMake と GTK4・libadwaita・Qt6・Botan 3 の開発環境が揃っていない。コンパイル、リンク、テスト実行、GNOME 50 での動作確認は未実施。依存追加、既存 KeePassXC や設定の変更、実データベースの操作、上流コードの変更、GitHub への公開作業は行っていない。

## 2. アーキテクチャと再利用境界

| 層 | 確認した構造 |
| --- | --- |
| ドメイン | `Database` が `Metadata` とグループツリー、変更通知、ファイル監視を管理する。エントリーには属性・添付・履歴・カスタムデータがある。編集と検索には `Entry::beginUpdate/endUpdate`、`Database::recycleEntry/recycleGroup`、`EntrySearcher` を利用できる。 `src/core/Database.cpp:43-98`、`src/core/Database.cpp:1018-1054`、`src/core/Entry.cpp:45-66`、`src/core/Entry.cpp:1100-1124`、`src/core/EntrySearcher.cpp:38-108` |
| 鍵と暗号 | `keys/` の `CompositeKey` がパスワード、キーファイル、チャレンジレスポンスの材料を扱う。 `crypto/` は Botan を使い、AES KDF・Argon2 KDF、暗号、ハッシュ、乱数を担当する。 `Crypto::init()` はライブラリのバージョン確認と暗号自己テストを行う。 `src/keys/CompositeKey.cpp:78-166`、`src/crypto/Crypto.cpp:241-263`、`src/CMakeLists.txt:52-58` |
| ファイル形式 | `Database::open` は `KeePass2Reader` を使い、署名とバージョンを確認して KDBX3/4 のリーダーに振り分ける。保存は `Database::save/saveAs` → `KeePass2Writer` → バージョン別 writer。 `KdbxXmlReader/Writer` がメタデータ、グループ、エントリーなどを扱い、KDBX4 はヘッダーとブロックを HMAC で検証する。 `src/core/Database.cpp:131-192`、`src/core/Database.cpp:237-247`、`src/core/Database.cpp:476-505`、`src/format/KeePass2Reader.cpp:60-97`、`src/format/KeePass2Writer.cpp:117-139`、`src/format/Kdbx4Reader.cpp:60-103` |
| Qt GUI | `Application` は `QApplication` の派生で、メイン実行ファイルは `MainWindow` を生成する。 `DatabaseWidget` がグループ・エントリービュー、検索、保存とロック時の UI 状態を管理する。 `src/gui/Application.h:23-41`、`src/main.cpp:182-208`、`src/gui/DatabaseWidget.cpp:90-165`、`src/gui/DatabaseWidget.cpp:2020-2143` |
| GUI なしの既存経路 | `keepassxc-cli` は `QCoreApplication` を生成し、`keepassxc_gui` をリンクせずに同じコアを利用する。 `db-create` は複合鍵を組み立てて `Database::setKey` と `saveAs(Atomic)` を使い、CLI の解除も `Database::open` に委ねる。 [`src/cli/CMakeLists.txt:47-74`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/cli/CMakeLists.txt#L47-L74)、`src/cli/keepassxc-cli.cpp:91-105`、`src/cli/DatabaseCreate.cpp:82-139`、`src/cli/DatabaseCreate.cpp:165-180`、`src/cli/Utils.cpp:124-216` |

**評価** — GTK 版はファイル形式・暗号・鍵派生・ドメインモデルを共用し、画面と操作の状態管理を所有する。既存 CLI は利用方法の参照と互換性試験の比較対象にする。CLI を製品のバックエンドにする場合はコマンドの出力仕様、秘密の標準入出力、プロセス間での解除状態の管理が別途必要になる。

## 3. CMake ターゲットと Qt 依存

| 依存 | 確認した範囲 |
| --- | --- |
| Qt Core / Concurrent | `keepassxc_core` は静的ライブラリ。Qt Core のオブジェクト、文字列、I/O、XML、タイマー、ファイル監視を使用し、Concurrent で保存や検索などの処理を分担する。 `src/CMakeLists.txt:19-98`、[`src/CMakeLists.txt:352-364`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/CMakeLists.txt#L352-L364)、[`src/core/AsyncTask.h:36-75`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/core/AsyncTask.h#L36-L75)、`src/core/Group.cpp:607-625` |
| Qt Gui / SVG | コアの依存先 `qrcode` が `Qt6::Svg` と libqrencode を公開リンクする。コアの `EntryAttachments::openAttachment` は `QDesktopServices` と一時ファイルを使う。CLI の存在は、添付を外部アプリで開く経路まで `QCoreApplication` で使えることを保証しない。 `src/CMakeLists.txt:328-330`、`src/CMakeLists.txt:352-364`、`src/qrcode/CMakeLists.txt:16-24`、`src/core/EntryAttachments.cpp:219-283` |
| Qt Widgets / SvgWidgets | GUI 実行ファイルは `keepassxc_gui` にリンクし、同ライブラリがコアと Widgets、SvgWidgets、Auto-Type、KeeShare、機能フラグに応じた各連携を集約する。Qt の設定画面、ダイアログ、リスト・ツリーのモデルやビューは GTK に置き換える。 `src/CMakeLists.txt:325-350`、[`src/CMakeLists.txt:366-378`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/CMakeLists.txt#L366-L378)、`src/CMakeLists.txt:405-417` |
| Qt Network | GUI と browser・SSH Agent・proxy がリンクする。外向き HTTP（更新、アイコン、HIBP）と、ブラウザーや単一起動などに使うローカルソケットは用途が異なる。 `KPXC_FEATURE_NETWORK=OFF` で外向き機能のソースを除いても Qt Network の構成時要求は残る。 `src/CMakeLists.txt:312-320`、`src/CMakeLists.txt:367-378`、`src/browser/BrowserHost.cpp:34-60`、`src/sshagent/SSHAgent.cpp:170-197`、`src/proxy/CMakeLists.txt:23-27`、`src/gui/Application.cpp:84-106`、`INSTALL.md:105-106` |
| Qt DBus | Linux のコアは Qt DBus、libusb、keyutils にリンクする。Polkit Quick Unlock と D-Bus 生成コードの一部がコア側に入り、画面ロック監視とポータル制御本体は GUI 側にある。Secret Service と Linux の Auto-Type も Qt DBus を使う。 `src/CMakeLists.txt:231-282`、`src/CMakeLists.txt:389-396`、`src/fdosecrets/CMakeLists.txt:4-34`、`src/autotype/CMakeLists.txt:40-46` |
| Qt 以外 | コアは Botan、PC/SC、zlib、Minizip、zxcvbn、同梱の補助ライブラリ等にリンクする。Botan 2.19.1 以降または 3 系を検出する。Linux で `WITH_X11=OFF` にしても、既存 Auto-Type の構成で xkbcommon を検出する。 `CMakeLists.txt:550-596`、`src/CMakeLists.txt:352-364`、`src/autotype/CMakeLists.txt:14-31` |

**確認** — 2.8.0 のルート設定は C++20 と Qt 6.2.4 以上を指定する。 `find_package(Qt6)` は Core、Concurrent、Gui、Network、Widgets、Svg、Test、LinguistTools、Linux の DBus を要求し、`src` で SvgWidgets も要求する。 `KPXC_MINIMAL` が切るのは Browser/Passkeys、SSH Agent、Secret Service。コアターゲットだけをビルドする場合にも、既存構成を通る限りこれらの Qt 開発モジュールの検出が必要。 `CMakeLists.txt:51-73`、`CMakeLists.txt:166-167`、`CMakeLists.txt:444-461`、`src/CMakeLists.txt:322-364`

**評価** — 「GTK 版の実行ファイルが Widgets をロードしない」と「本家の構成に Widgets の開発パッケージが不要」は別々に検証する。まず前者を目標にし、Qt Gui / Svg / DBus の内部依存は受け入れる。 `keepassxc_core` はソースツリー内のターゲットで、調査範囲の CMake に公開 SDK としてのインストール・export は見当たらない。上流の生成ヘッダー・MOC・D-Bus 生成・リンク順序を保持する接続が必要。ルートの `CMAKE_SOURCE_DIR` 参照があるため、無条件の `add_subdirectory(upstream)` は採用前に検証する。 `CMakeLists.txt:99-129`、`CMakeLists.txt:476-478`、`src/CMakeLists.txt:249-282`、`src/CMakeLists.txt:346-364`

## 4. GTK 化の範囲と接続方式

| 方式 | 実現性・保守の評価 |
| --- | --- |
| 1. 既存コアに GTK GUI を直結 | コア呼び出し自体の実現性は高い。最小のビルド・リンク確認に向く。画面ごとに `QObject` の所有、Qt 型、コアの内部 API を扱うと、寿命や再入の対策が各画面に分散し、上流変更も広く波及する。根拠は CLI の構成とコアの通知・タイマー実装。 `src/cli/CMakeLists.txt:63-70`、`src/core/Database.cpp:43-76` |
| 2. 既存コアを C++ アダプター越しに利用（推奨） | GTK 側はファイル選択、編集フォーム、通知、ロック UI、クリップボードを担当。アダプターは開閉・保存・鍵・検索・編集・世代管理を担当し、Qt の型と生ポインターは境界の内側に留める。初期の橋渡しは増えるが、上流変更を吸収する場所を限定できる。 `src/core/Database.cpp:131-192`、[`src/core/Database.cpp:269-355`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/core/Database.cpp#L269-L355)、`src/core/Database.cpp:554-586` |
| 3. 上流に小さな再利用用変更 | 未変更の構成で安全にリンクできない場合に検討。候補はコアターゲットと生成物を渡す CMake の接点、または実測で切り分けが必要と分かったデスクトップ呼び出し。変更理由と代替案を先に報告し、差分をビルド・境界に限定する。フォーマットや暗号への独自パッチは方式の前提に置かない。 `src/CMakeLists.txt:346-378`、`src/core/EntryAttachments.cpp:219-283` |

**評価** — C++20 と CMake を使い、試作は GTK4・libadwaita の C API を C++ から呼ぶ。 `AdwApplication` / `AdwApplicationWindow` を外枠に、`GtkTreeListModel` と `GtkColumnView` / `GtkListView` を候補にしてグループ・エントリーを表示する。 `GtkFileDialog` の結果は試作ではローカルパスに限り、Qt の保存・監視と同じファイルを扱う。明暗は `AdwStyleManager` のシステム設定追従を基本とする。gtkmm4 は型付きシグナルと C++ のライフタイム管理が利点だが、追加依存と libadwaita API の橋渡しを要する。まず C API を選び、GObject の所有とシグナル切断を C++ RAII で管理する。 [AdwApplication](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.Application.html)、[Libadwaita の外観設定](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/styles-and-appearance.html)、[GTK の Wayland バックエンド](https://docs.gtk.org/gtk4/wayland.html)、[gtkmm リファレンス](https://gnome.pages.gitlab.gnome.org/gtkmm/)

### イベントループ、スレッド、寿命

- **確認** — GTK のウィジェットは主スレッドで扱い、GLib の既定コンテキストを回す。Qt の queued 接続、タイマー、ファイル監視、遅延破棄には所有スレッドのイベント処理が必要。Qt ソースには GLib ディスパッチャーがあるが、選択はビルドと環境に依存する。 [GTK のスレッド仕様](https://docs.gtk.org/gtk4/section-threading.html)、[Qt の QObject とスレッド](https://doc.qt.io/qt-6/threads-qobject.html)、[Qt のディスパッチャー選択](https://github.com/qt/qtbase/blob/dev/src/corelib/thread/qthread_unix.cpp)、[Qt の GLib 実装](https://github.com/qt/qtbase/blob/dev/src/corelib/kernel/qeventdispatcher_glib.cpp)
- **評価** — 主スレッドで `QCoreApplication` を生成し、`g_application_run()` が GTK を駆動する。専用の `QThread` に Qt 側のイベントループを持たせ、DB オブジェクトとコアの操作窓口はそのスレッド内で生成・破棄する。 `Config` のように `qApp` を親とするシングルトンは主スレッドで先に設定し、操作中に別スレッドから設定を更新しない。Qt 側へのコマンドは queued 接続で渡し、GTK への返却は GTK の主スレッドだけが駆動する `GMainContext` に source を attach して行う。返却値は UUID・状態・表示に必要な値だけとし、解除セッションの世代番号で古い応答を無効にする。GTK から `Entry*` や `QSharedPointer<Database>` を保持しない。 `src/core/Config.cpp:575-586`、`src/core/Config.cpp:637-655`、`src/core/Database.cpp:43-98`
- **評価** — アダプターはコア操作を一件ずつ開始する。 `saveAs` は内部で `QtConcurrent` とネストした `QEventLoop` を使うため、同じ QThread に queued で入れただけでは直列化を保証できない。処理中は編集・監視通知からの再読込・次の保存を保留し、ロック要求を記録する。ロック・終了時は画面を先に秘匿し、新規要求を止め、処理中の仕事を回収してから DB を解放する。ファイル監視はコールバックに QObject の context を指定する一方、バックグラウンドのチェックサム計算も `this` を捕捉する。破棄時の監視処理の終了順は試験対象にする。 `src/core/Database.cpp:269-355`、`src/core/Database.cpp:554-586`、`src/core/AsyncTask.h:36-75`、`src/core/FileWatcher.cpp:111-146`、`src/gui/DatabaseWidget.cpp:2020-2035`
- **未検証** — 主スレッドの Qt の通知、`deleteLater`、終了処理の要件を洗い出す。 `QCoreApplication::exec()` を主スレッドで使わない構成では `aboutToQuit` に頼らず設定の明示同期と終了順を設計する。単一ループ案（GTK が Qt の GLib source も処理）は比較試験に限る。既定コンテキストの共有、ネストループ中の GTK 再入、KDF 中の画面応答、主スレッドで必要な Qt のイベントが処理されることを確かめる。一定周期の `processEvents()` 呼び出しを製品の前提にしない。 `src/core/Config.cpp:319-330`、`src/core/Config.cpp:575-586`、`src/core/AsyncTask.h:36-55`、[QCoreApplication の仕様](https://doc.qt.io/qt-6/qcoreapplication.html)、[GLib のコンテキスト通知](https://docs.gtk.org/glib/method.MainContext.invoke_full.html)

## 5. 機能別の移植難易度と保守負担

難易度は初回移植の範囲、保守負担は上流更新ごとの再確認の範囲で評価する。低は主に既存コア呼び出し、中は状態管理やデスクトップ境界の追加、高は複数の UI・サービス・プロトコルにまたがるもの。

| 機能・優先度 | 確認した実装と Widgets 依存 | GTK 化の評価と上流変更の要否 | 難易度 / 保守 |
| --- | --- | --- | --- |
| KDBX3 / KDBX4 の読書き・新規作成（必須） | `Database` と `KeePass2Reader/Writer`。CLI は Widgets の GUI を介さず作成・保存する。 `src/core/Database.cpp:131-192`、`src/core/Database.cpp:269-395`、`src/format/KeePass2Writer.cpp:52-139`、`src/cli/DatabaseCreate.cpp:114-139`、`src/cli/DatabaseCreate.cpp:165-180` | ファイル選択、鍵の入力、バージョン変更と保存エラーの表示を GTK に追加。暗号・形式の上流変更は不要と評価。ビルド接点は §4 の条件付き | 中 / 中 |
| ロック・解除（必須） | `DatabaseOpenWidget` は複合鍵を構築して `Database::open` を呼ぶ。 `DatabaseWidget::lock` は保存確認、クリップボード消去、ビュー初期化、DB 差し替えまで扱う。コアに `releaseData` がある。 `src/gui/DatabaseOpenWidget.cpp:329-371`、`src/gui/DatabaseOpenWidget.cpp:438-472`、`src/gui/DatabaseWidget.cpp:2020-2143`、`src/core/Database.cpp:554-586` | 状態機械をアダプターに設け、GNOME の画面ロックやサスペンド時の通知はデスクトップ連携で受ける。ロック中に失敗した保存・解除と古い非同期通知を処理する。上流変更は現時点で前提にしない。 `src/gui/osutils/nixutils/ScreenLockListenerDBus.cpp:24-102` | 中 / 中 |
| エントリー・グループ管理、検索（必須） | コアの `Group`、`Entry`、`Database::recycleEntry/recycleGroup`、`EntrySearcher`。Qt GUI は専用の編集ウィジェットとビューを使用。 `src/core/Group.cpp:451-529`、`src/core/Entry.cpp:1100-1124`、`src/core/Database.cpp:1018-1054`、`src/core/EntrySearcher.cpp:38-108`、`src/gui/entry/EditEntryWidget.cpp:1284-1309` | UUID で対象を特定し、既存のエントリーに変更されたフィールドだけを適用する。履歴・ごみ箱・変更通知は既存ロジックを利用。GTK モデルと編集 UI を追加し、上流変更は不要と評価 | 中 / 中 |
| パスワード生成（必須） | `PasswordGenerator` はコアで設定値と乱数から生成。Qt 側は生成器の設定画面を持つ。 `src/core/PasswordGenerator.cpp:38-98`、`src/core/PasswordGenerator.cpp:125-184`、`src/gui/PasswordGeneratorWidget.cpp:19-64` | GTK フォームの入力をコアの setter に渡し、生成器を共用する。上流変更は不要と評価 | 低 / 低 |
| クリップボード（必須） | Qt GUI の `Clipboard` は `QApplication::clipboard`、OS 別 MIME ヒント、タイマー、終了時の消去を使う。 `src/gui/Clipboard.cpp:34-119` | `GdkClipboard` で実装し、所有中の provider だけを時間切れ・ロック・終了で失効させる。GNOME Wayland 上でフォーカス喪失時と他アプリによる置換を実測。上流変更は不要と評価。 [GdkClipboard](https://docs.gtk.org/gdk4/class.Clipboard.html) | 中 / 中 |
| TOTP（データ保持は必須、コード生成 UI は後段） | `Entry` が属性から従来形式・OTP URI・KeePass 2 形式を読み、`Totp` がコードを生成する。属性はフォーマット層で読書きし、表示・設定は Qt のダイアログ。 `src/core/Entry.cpp:576-646`、`src/core/Totp.cpp:231-285`、`src/format/KdbxXmlReader.cpp:834-874`、`src/format/KdbxXmlWriter.cpp:416-456`、`src/gui/DatabaseWidget.cpp:516-572` | 編集フォームが既存の属性と保護フラグを保持する。後段の表示には既存のコード生成を使う。上流変更は不要と評価、保持は往復試験で判定 | 低（保持）・中（UI） / 低〜中 |
| Browser Integration / Passkeys（後段） | `keepassxc-proxy` と `BrowserHost` はローカルソケットを使用。 `BrowserService` は `MainWindow`、確認 UI、解除 UI、Passkey 処理にまたがる。 `src/proxy/CMakeLists.txt:17-27`、`src/browser/BrowserHost.cpp:34-80`、`src/browser/CMakeLists.txt:19-40`、`src/browser/BrowserService.cpp:28-91`、`src/core/EntryAttributes.cpp:45-59` | IPC とプロトコルの再利用は候補。サイト・エントリーへのアクセス承認や解除を GTK の状態に接続する必要があり、UI コールバックの分離に上流変更の可能性がある。初期は接続を無効にし、KDBX のペアリング・パスキー関連データは保持を検証 | 高 / 高 |
| Auto-Type（後段） | `autotype` は Widgets にリンクし、Linux の Wayland 実装は RemoteDesktop ポータル経由で入力する。Wayland のウィンドウ名取得は空を返す。 `src/autotype/CMakeLists.txt:14-46`、`src/autotype/wayland/AutoTypeWayland.cpp:99-177`、`src/gui/osutils/nixutils/GlobalShortcutsPortal.cpp:89-134` | 選択ダイアログ、ポータルの同意とセッション、フォーカス管理、Global Shortcuts の実装を検証する。既存実行器を共用するには Qt GUI との分離が必要になる可能性がある。KDBX のシーケンス・関連付けはコアで保持する。 [RemoteDesktop ポータル仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html) | 高 / 高 |
| SSH Agent（要検討） | `SSHAgent` の Unix 側は `QLocalSocket`。鍵の登録・解除は DB の解錠・ロックと連動する。ターゲットは Qt Widgets と設定 UI を含む。 `src/sshagent/CMakeLists.txt:19-32`、`src/sshagent/SSHAgent.cpp:170-197`、`src/sshagent/SSHAgent.cpp:530-579`、`src/gui/DatabaseWidget.cpp:2128-2130` | ソケット・鍵処理と UI を分け、登録済み鍵をいつ解除するかを状態機械につなぐ。上流のターゲット分割や呼び出し境界の変更が必要になる可能性。初期は設定・添付を保持する | 中〜高 / 中 |
| Secret Service（要検討） | Qt DBus のサービスで、`Service` と `Collection` は `DatabaseTabWidget` / `DatabaseWidget` を直接参照し、ロックや解除を連携する。サービス名の登録がある。 `src/fdosecrets/CMakeLists.txt:4-34`、`src/fdosecrets/objects/Service.cpp:38-127`、`src/fdosecrets/objects/Service.cpp:552-589`、`src/fdosecrets/objects/Collection.cpp:34-56`、`src/fdosecrets/dbus/DBusMgr.cpp:375-431` | DB の一覧・公開グループ・解錠プロンプトをアダプター化し、GNOME Keyring と同じバス名の競合と既存サービスへの影響を検証。再利用には上流側の UI 境界変更の可能性が高い。初期はサービスを登録しない | 高 / 高 |
| YubiKey / チャレンジレスポンス（要検討） | コアに `ChallengeResponseKey` と USB / PC/SC ドライバーがあり、Qt GUI が検出・選択・操作案内を担当する。検出は QtConcurrent を使用し、タッチ待ちが発生し得る。 `src/CMakeLists.txt:83-91`、`src/keys/drivers/YubiKey.cpp:29-55`、`src/keys/drivers/YubiKey.cpp:74-96`、`src/keys/drivers/YubiKey.cpp:139-170`、`src/gui/DatabaseOpenWidget.cpp:438-487` | 検出とタッチ案内を GTK に接続。USB 権限・PC/SC・取り外し・タイムアウトを実機で検証する。上流の暗号・ドライバー変更は前提にしない。未対応の鍵で解除できない場合はエラーとして扱い保存に進まない | 中〜高 / 中 |

高度な DB 設定、CSV 等の入出力は後続候補とする。試作で設定画面を用意していない KDF・暗号方式・履歴ポリシーも読み込み時の値を保持する。明示した上流のフォーマット昇格や正規化は個別に検証する。CSV・XML の平文出力は試作の互換性経路から外す。 `src/CMakeLists.txt:59-82`、`src/format/KeePass2Writer.cpp:52-139`

## 6. 上流更新に追従する方法

| 構成 | 評価 |
| --- | --- |
| GitHub Fork で GTK 版を保守 | 上流との差分、マージ、上流への変更提案を GitHub で扱いやすい。GUI を大きく書き換えるとリリースごとの競合が増え、セキュリティ修正と UI 差分を切り分ける作業が発生する |
| 独立リポジトリと固定した上流ソース（推奨） | GTK UI、アダプター、テストを独立して管理する。 `upstream/` に公式リポジトリを submodule 等で固定し、配布ビルドは具体的な SHA を使用。上流は独立した CMake プロジェクトとして構成し、別のビルドディレクトリに生成する。GTK 側は上流ターゲットの成果物とリンク情報を取り込む方法を先に検証する。上流に変更提案が必要になった時点で Fork を作業用に使える |
| 大きなパッチセットを保持 | GUI・暗号・保存をまたいで独自差分を持つほど、修正取り込みと互換性の再確認が増える。必要な差分が判明したら目的ごとに分け、上流化または削除できる条件と検証範囲を記録する |

**評価** — 運用は、公式リリース・Security Advisories・`develop` を確認し、更新用ブランチに候補コミットを取り込む。前後の SHA と差分（コア、CMake、テスト・fixture）を記録し、まず本家の基準テスト、次にアダプター、KDBX の双方向互換性、Wayland の順で確認する。通常は検証した安定タグへ進める。重大なセキュリティ修正は安定版を待つ影響を確認し、修正を含む特定コミットまたは上流の該当修正だけを先に取り込める経路を用意する。修正の適用確認、残パッチの棚卸し、次の正式タグへの置換までを更新作業に含める。戻す場合はコードの固定 SHA を戻し、保存済み DB の自動ダウングレードは行わず、互換性試験と暗号化バックアップからの復旧を区別する。 `SECURITY.md:1-17`、[上流の Security Advisories](https://github.com/keepassxreboot/keepassxc/security/advisories)、[上流のリリース一覧](https://github.com/keepassxreboot/keepassxc/releases)

コアに安定 ABI を仮定しない。上流でクラスやビルド定義が変わった際はアダプターとビルド接点を更新し、GTK UI の操作契約を保つ。公開前には上流が示す GPL-2 または GPL-3 と、同梱ファイルの個別ライセンスを配布方法に照らして確認する。試作の実行ファイル名、アプリ ID、設定・キャッシュ・状態ディレクトリは本家と分ける。 `src/CMakeLists.txt:346-378`、`src/core/Config.cpp:589-655`、`README.md:67-69`、`COPYING:1-47`

## 7. セキュリティとデータ互換性

| リスク | 確認と Phase 2 以降の対策 |
| --- | --- |
| 未実装の機能に属するデータの消失 | 既存コアは文字列属性と保護フラグ、添付、Auto-Type、履歴、対応する KDBX 版の CustomData を読書きする。GTK 側の表示項目だけでエントリーを作り直すと、画面にない値が欠ける。既存オブジェクトを更新し、TOTP、パスキー、SSH 設定、添付、履歴、カスタムアイコン、削除済み UUID、メタデータ・保護フラグの保持を比較する。 `src/format/KdbxXmlReader.cpp:752-824`、`src/format/KdbxXmlReader.cpp:834-874`、`src/format/KdbxXmlWriter.cpp:139-170`、`src/format/KdbxXmlWriter.cpp:416-480` |
| 未知の要素とフォーマット変更 | Reader は未認識の XML 要素を読み飛ばす。既知の属性が残ることと、未知の要素が往復できることは区別する。Writer は現在の版・KDF・使用フィールドに応じて必要な版を選び、条件によって AES-KDF を KDBX4 用に切り替える。検証済みの組合せ以外の上書きは保留する。未知要素の検出・保存拒否をどこで実施できるかは試作課題で、既存 API が警告を構造化して返す前提は置かない。 `src/format/KdbxXmlReader.cpp:778-794`、[`src/format/KdbxXmlReader.cpp:1220-1224`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/src/format/KdbxXmlReader.cpp#L1220-L1224)、`src/format/KeePass2Writer.cpp:52-139` |
| 保存失敗・同時更新 | `Database::saveAs` は同時保存を拒否し、先頭ブロックのハッシュを使って外部変更を検出する。 `Atomic` は `QSaveFile::commit` を使う。先頭の比較と置換の間の競合や、削除・置換を含む全変更の検出は保証できない。バックアップ処理の戻り値を `performSave` が確認しないため、バックアップ必須の運用では保存成功とは別に成否を確認する。試作はローカルの fixture コピー、Atomic、明示保存に限定する。失敗時の元ファイル保持、エラー通知、暗号化バックアップとアクセス権、再試行、別プロセスからの置換を確認する。 `src/core/Database.cpp:269-395`、`src/core/Database.cpp:397-438`、`src/core/Database.cpp:589-635`、`src/core/FileWatcher.cpp:111-146` |
| 秘密がプロセス内に残る | `PasswordKey` は Botan の `secure_vector` を持つ一方、エントリー属性は `QString` を含み、入力や API 境界で UTF-8 のコピーも発生する。本家には `Alloc.cpp` の C++ delete 時の消去と Linux のダンプ抑制処理がある。GTK/GObject、GLib、外部ライブラリを含む全領域への適用は確認できていない。実際のリンク結果で delete と初期化・ダンプ抑制を確認し、秘密を一覧モデル・ログ・起動引数・環境変数・平文ファイルに残さない。入力バッファ、DTO、クリップボード provider とタイマーの寿命を短くする。 `src/keys/PasswordKey.h:34-47`、`src/keys/PasswordKey.cpp:46-66`、`src/core/EntryAttributes.h:92-103`、`src/core/Alloc.cpp:34-95`、`src/core/Bootstrap.cpp:62-109` |
| クリップボード・画面からの露出 | GTK 側では通常のクリップボードに限り、秘密を PRIMARY 選択へ追加しない。自分の provider の同一性と所有状態を確認し、新しいコピーや他アプリの内容を期限切れ処理で消さない。ロック・終了時も同じ規則を使う。既に貼り付けられた内容や外部履歴の消去は制御できない。画面キャプチャ抑止は別途検証し、GNOME Wayland で利用可能と仮定しない。 `src/gui/Clipboard.cpp:47-119`、[GdkClipboard の provider と所有状態](https://docs.gtk.org/gdk4/class.Clipboard.html) |
| 既存環境との干渉 | Linux の `Config` は app ID と独立に既定パスへ `keepassxc` を含める。試作では初回 `config()` 呼び出しより先に `Config::createConfigFromFile` の両パス、または試作プロセス専用の `KPXC_CONFIG` / `KPXC_CONFIG_LOCAL` と一時 XDG ディレクトリを指定する。実データと既存アプリの設定・バス名・ソケットは使用しない。 `src/core/Config.cpp:559-586`、`src/core/Config.cpp:589-655`、`src/browser/BrowserShared.cpp:53-75`、`src/fdosecrets/dbus/DBusMgr.cpp:414-431` |

## 8. 既存テストの再利用と GTK 版の追加試験

| 区分 | 確認と再利用方法 |
| --- | --- |
| 上流テストの構成 | `WITH_TESTS` は既定 ON、`WITH_GUI_TESTS` は既定 OFF。 `add_unit_test` が QtTest の実行ファイルを CTest に登録する。既定の `TEST_LIBRARIES` は `keepassxc_gui` と Qt Test、`testsupport` も Widgets とリンクする。GUI テストを切っても、テストのビルド依存から Widgets は外れない。 `CMakeLists.txt:40-42`、`CMakeLists.txt:598-605`、`tests/CMakeLists.txt:25-34`、[`tests/CMakeLists.txt:50-85`](https://github.com/keepassxreboot/keepassxc/blob/1c2f23ef5914c0f442a00533def1689857afb5a7/tests/CMakeLists.txt#L50-L85)、`tests/CMakeLists.txt:201-203` |
| ヘッドレスで再利用する候補 | `testkdbx3`、`testkdbx4`、`testkeys`、暗号・ストリーム、`testpasswordgenerator`、`testtotp`、`testentrysearcher` など。KDBX3 は `QTEST_GUILESS_MAIN`、KDBX4 は `QCoreApplication` を使用する。まず上流のターゲットをそのまま実行し、GTK 側のコア専用テストに移す場合は実際の include とリンクを個別に確認する。 `tests/CMakeLists.txt:63-94`、`tests/CMakeLists.txt:118-149`、`tests/TestKdbx3.cpp:26-32`、`tests/TestKdbx4.cpp:31-43`、`tests/TestTotp.cpp:25-35`、`tests/TestEntrySearcher.cpp:19-33` |
| Widgets・画面依存 | `tests/gui` は実際の Qt GUI を起動する。 `testcli` も `QTEST_MAIN` を使用し、クリップボードのケースは `WAYLAND_DISPLAY` があると明示的にスキップする。 `testentrymodel` や `testgroupmodel` など Qt 側のモデル検証は GTK モデルの代用にならない。操作の期待結果を GTK 側に移して検証する。 `tests/gui/CMakeLists.txt:16-34`、`tests/gui/TestGui.cpp:78-103`、`tests/TestCli.cpp:60-68`、`tests/TestCli.cpp:631-646`、`tests/TestEntryModel.cpp:18-45` |
| データ互換性の既存ケース | `tests/data` の上流 fixture と新規の合成データを使用。KDBX の保護属性、添付、非 ASCII パスワード、I/O 失敗、鍵変更、KDBX4.1 昇格のテストがある。ハードウェアはモックと実機の検証を分ける。YubiKey の実機テストには検出状況によるスキップがある。 `tests/CMakeLists.txt:25-26`、`tests/CMakeLists.txt:66-73`、`tests/TestKeePass2Format.cpp:492-566`、`tests/TestKdbx4.cpp:255-316`、`tests/TestYkChallengeResponseKey.cpp:31-87` |

追加する検証は次の三系統とする。

1. **ファイル往復** — 上流 CLI または GUI で生成した KDBX 3.1、4.0、4.1 を試作で開き、変更なしの保存と標準フィールド一つだけの編集保存を行い、同じ固定コミットの上流で開き直す。逆方向は試作で作成・保存した DB を上流で編集し、試作で開き直す。KDF・暗号方式、UUID、階層、Unicode、保護属性、複数の TOTP 形式、パスキーとブラウザー設定、添付・履歴・カスタムデータなど、該当バージョンで表現できる項目を意味単位で比較する。ランダム化される暗号文は一致条件にせず、変更前ファイルの保持が必要な失敗ケースのみファイルハッシュを比較する。KDBX 3.0 の読み込みと 3.1 への書き出し、4.1 への意図した昇格もケースを分ける。 `src/format/KeePass2Writer.cpp:52-139`、`tests/TestKdbx3.cpp:92-106`、`tests/TestKdbx4.cpp:255-316`
2. **アダプターと異常系** — 誤パスワード、キーファイル不一致、破損・切断された入力、書込権限不足、保存先の置換、容量不足を模擬する。保存中の編集・二重保存・再読込・ロック・終了、監視処理中の解放、直後の再解除、失敗後の再試行を試す。自動ロックでは秘密を画面から直ちに隠し、保存失敗時は破棄せず、再認証後に保存または破棄を判断できるかを検証する。Qt タイマー、queued 通知、ファイル監視、遅延破棄の到達と GTK 側の UI 応答を測る。旧世代からの応答で表示やクリップボードが復活しないことを確認する。 `src/core/Database.cpp:269-355`、`src/core/Database.cpp:554-586`、`src/core/FileWatcher.cpp:111-146`、`tests/TestDatabase.cpp:273-309`
3. **GNOME 50 の Wayland セッション** — `GDK_BACKEND=wayland` の試作プロセスで、ライト・ダークの切替、キーボード操作、ファイル選択、貼り付け、フォーカス喪失・他アプリによる置換後のタイマー、画面ロック・サスペンド・終了時の消去を検証する。ログとテスト出力には合成秘密の値も出さず、比較結果だけ記録する。 [GTK の Wayland バックエンド](https://docs.gtk.org/gtk4/wayland.html)、[GdkClipboard](https://docs.gtk.org/gdk4/class.Clipboard.html)

## 9. 未解決の技術課題

| 未検証事項 | 確認方法と判定への影響 |
| --- | --- |
| 未変更ツリーのコアを外部ターゲットに接続できるか | 生成ヘッダーと依存関係を含む再現可能な CMake 接点を確認。ビルド定義の複製が増える、または Widgets の実行時ロードが不可避な場合は §4 の方式 3 の要否を判断する |
| Qt と GLib の終了・再入・スレッド親和性 | 主スレッドの Qt に残る通知、`Config` の参照、QtConcurrent の仕事、ファイル監視、DB オブジェクトと GLib source の破棄順を実行確認。未解決の競合があれば書込み可能な試作の判定を保留する |
| 保持可能な KDBX の範囲と非対応形式の扱い | 未知 XML や公開ヘッダーの未対応データの検出、KDBX 昇格、外部更新時の上書き防止の境界を決める。検出が確認できるまでは事前に用意した fixture だけを試作の書込み対象とする |
| Linux デスクトップ・配布時の動作 | Ubuntu 26.04 の実パッケージと依存、GNOME 50 上のポータルや画面ロック、外部クリップボード履歴との挙動を確認。deb の作成とオプションの OS 連携は後段 |
| 上流の更新で同じ構成が維持できるか | 固定タグから別の公式コミットへ一度差し替え、コアのビルドとテストを再実行し、必要だった差分・工数を記録。更新ごとに広範なソース修正を要する場合は構成を再評価する |

## 10. Phase 2 の最小試作計画

1. **本家の基準を取る。** 隔離した Ubuntu 26.04 環境で CMake と開発パッケージの用途・不足分を確認し、導入が必要なら事前に報告・相談する。固定タグをソースとして別ディレクトリでビルドする。候補フラグは `-DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF -DKPXC_FEATURE_DOCS=OFF -DWITH_X11=OFF -DWITH_TESTS=ON -DWITH_GUI_TESTS=OFF`。まず `keepassxc_core`、`keepassxc-cli` と選定した上流テストをビルドし、合成データで基準を取る。 `CMakeLists.txt:40-73`、`src/CMakeLists.txt:346-378`
2. **表示なしでコアをつなぐ。** 本家に手を入れず、GTK 側のアダプター用ターゲットへコアと生成物をつなぐ経路を検証する。 `QCoreApplication` で初期化し、固定 fixture の解除・一覧・一件編集・Atomic 保存・ロック・再解除と、新規 DB の作成を確認する。実行ファイルの依存（`readelf` 等）で Qt Widgets の有無を調べ、ビルド時要求と実行時要求を分けて記録する。
3. **小さな GTK ウィンドウで共存を試す。** ファイルを開く、解除、タイトル一覧、一件の標準項目編集、別名で fixture に保存、ロック、合成パスワードの期限付きコピーだけを検証用 UI にする。専用 QThread でコアを動かし、ネストした Qt ループ中の GTK 応答とコマンドの直列化を確認する。新規 DB、生成、検索、CRUD はコアの確認を優先し、全画面の実装は後続判断とする。
4. **判定する。** §8 の双方向往復と失敗・ロック競合を通し、実機で Wayland と外観の切替を確認する。別の上流コミットへの差し替えを試し、上流変更が必要なら理由・変更箇所・更新への影響を報告する。固定 SHA、ビルド手順、テスト結果、上流に必要な差分を再現可能な形で残す。

合格条件は、既存の暗号・保存経路で読み書きでき、検証した KDBX のデータが保持され、保存失敗で元のファイルが壊れず、ロック後に UI や古い通知から秘密を取得できず、Wayland で GTK UI が正常に操作できること。更新リハーサルではコアの取り込みがアダプターとビルド接点に収まり、互換性試験を同じ手順で再実行できることを求める。

## 11. 最終評価

| 判断基準 | Phase 1 の結論 |
| --- | --- |
| KeePassXC コアを安全に再利用できるか | **有力、実証待ち。** CLI とコアの分離、KDBX と暗号の既存経路、Atomic 保存を確認した。採否は KDBX のデータ保持、失敗時の元ファイル保護、ロック時の秘密の寿命と競合試験で判断する |
| GNOME ネイティブ UI を実現できるか | **設計上は可能。** GTK4 / libadwaita が画面と操作を担い、Qt の UI ターゲットは使わない構成を試す。実機のイベント配送、クリップボード、画面ロック、外観の確認が残る |
| 上流更新に継続して追従できるか | **分離方針は妥当、運用は未検証。** 固定上流＋独立アダプターは差分を絞れる。上流のビルド接点と内部 API が変わった場合の負担は、一回の更新リハーサルで測る |

製品の実装可否は Phase 2 の結果で判断する。特に、保持を確認していないデータを保存してしまう場合、ロックと保存を安全に調停できない場合、または上流の修正を取り込むたびにコアの大きな差分を再適用する必要がある場合は、書込み可能な版への移行を保留する。

## 12. 実測を踏まえた判定

2026-11-10 に、Phase 2 の VM・実機の検証結果を §11 の三つの判断基準と照合した。試験用 deb `0.0.9~phase2` の SHA-256 は `48fef51f19b569aa5439027a69ed78bb5c79ade2d62c8b74f0c8830a926e3881`。実測には固定した合成データを使用した。 [同一 deb の VM 回帰試験](phase2-results.md#同一-deb-の-vm-回帰試験) [実機での確認結果](phase2-results.md#実機での確認結果)

| 判断基準 | 確認済み | 評価と残る条件 |
| --- | --- | --- |
| KeePassXC コアを安全に再利用できるか | 既存コアを使い、KDBX 3.1、4.0、4.1 を元の形式で別名保存した。元ファイルの SHA-256 を維持し、上流 CLI と reader で検証対象の TOTP 属性、添付、履歴などを照合した。4.1 は編集後の復旧点をアプリ再起動後に開いた | 固定した合成データでは再利用が成立。KDBX 3.1 の自動復旧点、未知の拡張データ、通常の上書きは別の合格条件が必要。基準試作の入口は `prototype/CoreSession.cpp`、独立復旧試作の読み込み・別名保存は作業用の `gtk-legacy-cross-filesystem/prototype/CoreSession.cpp:664`、`:732`。 [VM 回帰試験](phase2-results.md#同一-deb-の-vm-回帰試験) |
| GNOME ネイティブ UI を実現できるか | Ubuntu 26.04.1、GNOME 50.1、Wayland で画面ロック、復帰後の再認証、ファイル選択、日本語表示を実機で確認した。VM は配色切り替え、文字・表示倍率 200%、S3、試験用 deb の導入と削除を確認した | GTK4 / libadwaita を使う構成は採用候補。Qt の内部依存を保持し、DB の寿命と GTK の表示状態を分離する。実機では deb を展開して試し、APT での導入・削除は VM で確認した。作業用の `gtk-recovery-screen-deb/runtime/resume_ui.py:562`、`:667`、`:703`。 [実機での確認結果](phase2-results.md#実機での確認結果) |
| 上流更新に継続して追従できるか | KeePassXC 2.8.0 `1c2f23ef5914c0f442a00533def1689857afb5a7` と `develop` `6bcb901aefdd6c7a525466d7dfe099c5ebc13c0e` の既存ビルドで、現在の基準アダプターを再ビルドした。`phase2_core_probe` は両コミットで合格し、両上流ツリーに差分はなかった | CMake の接点とアダプターに差分を局所化できる見込みを確認。長期追従とセキュリティ修正の取込みは、固定 SHA を更新するたびに試験を繰り返す運用で確かめる。根拠は `prototype/attach.cmake:3`、`prototype/targets.cmake:5`、`:15`。 [ビルドとテスト](phase2-results.md#ビルドとテスト) |

再ビルドと CTest の記録は `/tmp/keepassxc-phase2.YxadbPNq/feasibility-upstream-build-recheck.p4wgbIIQ` と `/tmp/keepassxc-phase2.YxadbPNq/feasibility-develop-build-recheck.8lT9fnTq` の `core-build.log`、`core-ctest.log` に置いた。

**判定。** 上流の暗号・KDBX コアを再利用し、GTK4 / libadwaita で画面を構築する方式は条件付きで実現可能。合成データを使う限定試作の方式として評価できる。実データの書き込みと一般配布の判断には、次の条件が残る。

- **データ保全。** 合成 DB の別名保存で、未知の XML とバイナリヘッダーのフィールドが失われた。未知の XML 要素に保護値がある試験では、解除に成功しても後続エントリーのパスワードを誤って読み取った。書き込み可否に加え、読み取った内容を利用者に渡せるかを判定する必要がある。外部更新と公開が競合する場合は、元ファイルの上書きで新しい版を失わないことを確認する。 [未知の XML の試験](#13-未知の-xml-を含む-kdbx-の別名保存) [未知のバイナリヘッダーの試験](#14-未知のバイナリヘッダーを含む-kdbx-の別名保存) [保存と秘密の利用可否](#15-解除結果に基づく保存と秘密の利用可否) [保存の競合試験](phase2-results.md#外部更新と保存失敗時のデータ保全)
- **検証完了。** KDBX 4.0・4.1 の合成試験では、XML の後ろに大きな正当なコメントがあると、終端ブロックの破損や欠落を検出せずに読み込みが成功した。モデルを公開し保存を許可する前に、暗号ストリームの末尾まで検証が完了したことを確認する必要がある。 [終端検証の試験](#22-xml-終端と暗号ストリームの検証完了)
- **秘密情報。** GNOME 50.1 の試験では、フォーカスを失った後にロックしても、合成のクリップボード文字列を別プロセスが再読できた。保存が logind の遅延上限を超えた VM 試験では、画面を隠した状態で DB の子が復帰後まで残った。パスワードのコピーと自動消去、子と GTK のメモリ上の秘密、保存中のロックにそれぞれ合格条件を設ける。 [クリップボードの対策比較](phase2-desktop-followup.md#フォーカスを失ったクリップボードの対策比較) [保存中の S3 試験](phase2-results.md#同一-deb-の-vm-回帰試験)
- **機能と保守。** 現在の GTK 試作の編集は合成タイトルが対象。製品に必要な新規 DB 作成、エントリー・グループ管理、検索、生成などは GTK の実装と確認が残る。上流固定 SHA、アダプター、合成データ、試験とパッケージのビルド手順を揃え、上流更新と配布の判断に使えるようにする。 [最小試作の範囲](#10-phase-2-の最小試作計画)

## 13. 未知の XML を含む KDBX の別名保存

2026-11-10 に、§12 の二つの固定コミットで KDBX 3.1 と 4.1 を試した。試験用実行ファイルのリンク時だけ `KdbxXmlWriter::writeDatabase` と `KdbxXmlReader::readDatabase` を包み、合成 DB の内部 XML に未知の要素または属性を入れた。圧縮・暗号化・認証と復号は KeePassXC の既存経路を使用した。比較用に、認識される `Meta/CustomData` に試験用のキーを持たせた。根拠は `src/format/Kdbx3Writer.cpp:140`、`src/format/Kdbx4Writer.cpp:169`、`src/format/Kdbx3Reader.cpp:115`、`src/format/Kdbx4Reader.cpp:146`、`src/format/KdbxXmlReader.cpp:414`、`src/format/KdbxXmlWriter.cpp:169`。 [KeePass の KDBX 4 仕様](https://keepass.info/help/kb/kdbx_4.html)

| 合成ファイルの条件 | 通常の読み込み | 既存アダプターの別名保存 | 書き込み前に試験用判定を適用 |
| --- | --- | --- | --- |
| `Meta/CustomData` 内に試験用キー | 成功、警告 0 件 | キーと値を保持 | 保存できた |
| `Entry` 内に未知の XML 要素 | 成功、読み飛ばしの警告 1 件 | 要素を失った。比較用の `CustomData` は保持 | 出力ファイルを作る前に拒否 |
| `Entry` の開始タグに未知の XML 属性 | 成功、警告 0 件 | 属性を失った。比較用の `CustomData` は保持 | 出力ファイルを作る前に拒否 |

両形式・両コミットで同じ結果になり、元の合成ファイルの SHA-256 は変化しなかった。入力と出力をそれぞれ復号し、要素と属性の有無を比較した。上流の `parseEntry` は開始タグの名前を確認してから子要素を読み、未知の子要素は警告付きで読み飛ばす。今回追加した開始タグの属性については警告が出なかった。根拠は `src/format/KdbxXmlReader.cpp:689`、`:793`、`:1220` と、作業用 `/tmp/keepassxc-phase2.YxadbPNq/kdbx-unknown-xml.6foMMNow/prototype/fault_probe.cpp:52`、`:79`、`:97`、`:162`、`:192`。両ビルドの `*-test/run.log` に結果を記録した。

**評価。** 読み込み成功と警告の有無だけでは、別名保存によるデータ消失を判定できない。試験では、同じ解除セッションが読み込む内部 XML の構造を調べ、比較用 `CustomData` は許可し、注入した要素・属性があるときは書き込み前に停止できた。この判定が扱うのは今回の試験用マーカーに限る。実装の採否には、正式な構造の許可範囲、暗号化された内部値、保持を保証できない場合の表示と保存経路の制御を追加検証する必要がある。試験では合成 XML をメモリに一時保持したため、実データを扱う設計では秘密の寿命も別途検証する。 [バイナリヘッダーの追加試験](#14-未知のバイナリヘッダーを含む-kdbx-の別名保存)

## 14. 未知のバイナリヘッダーを含む KDBX の別名保存

2026-11-10 に、§12 の 2.8.0 と `develop` の固定コミットで、KDBX 3.1、4.0、4.1 を試した。試験用実行ファイルのリンク時だけ writer を包み、未知のフィールド ID `0x7e` と合成の値を外側ヘッダーまたは 4 系の内側ヘッダーに一つずつ入れた。外側のハッシュと 4 系の HMAC は注入後に上流 writer が計算し、内側は上流の圧縮・暗号化・認証経路を通した。比較用に 4 系の `PublicCustomData` に、既知の文字列型で試験キーと値を入れた。根拠は `src/format/Kdbx3Writer.cpp:71`、`:100`、`src/format/Kdbx4Writer.cpp:69`、`:89`、`:105`、`:151`、`:202`。 [KeePass の KDBX 形式仕様](https://keepass.info/help/kb/kdbx.html)

| 合成ファイルの条件 | 通常の読み込み | 既存アダプターの別名保存 | 試験用の書き込み前判定 |
| --- | --- | --- | --- |
| 未知のフィールドなし（3.1） | 成功、警告 0 件 | 再読込に成功 | 保存できた |
| `PublicCustomData` に試験キー（4.0、4.1） | 成功、警告 0 件 | キーと値を保持 | 保存できた |
| 外側に未知のフィールド（3.1、4.0、4.1） | 成功、警告 1 件 | 未知フィールドを失った | 出力ファイルの作成前に拒否 |
| 内側に未知のフィールド（4.0、4.1） | 成功、警告 0 件 | 未知フィールドを失った | 出力ファイルの作成前に拒否 |

**確認済み。** 両コミットで各 8 ケースが合格し、すべての元ファイルで SHA-256 が一致した。外側はファイルのフィールド列を直接比較し、内側は上流 reader が復号・展開したストリームを読むときに、ID と値を照合した。4 系の全ケースで `PublicCustomData` の試験キーと値が保持された。外側の未知 ID は警告して読み飛ばす。内側は値を読み切った後、該当する分岐がないまま次のフィールドに進む。根拠は `src/format/Kdbx3Reader.cpp:136`、`:209`、`src/format/Kdbx4Reader.cpp:119`、`:221`、`:239`、`:252`、`:280`。

**評価。** 書き込み可否は解除に使った読み込み結果に結び付ける。鍵なしの外側ヘッダー読み込みは復号前に終了するため、内側の未知フィールドを検出できない。試験では、同じ解除セッションのヘッダー読み込みでマーカーを検出したときに、別名保存を停止できた。製品で扱うには、上流の読み込み結果として保持可否を受け取る接点と、別名保存・上書き・自動復旧点のすべてに適用する条件の検証が残る。根拠は `src/format/KdbxReader.cpp:99`、`src/format/Kdbx4Reader.cpp:77`、`:119`。

**試験範囲。** 判定は単一の試験 ID と Gzip の合成ファイルに限った。未認識フィールドを網羅する仕組みは未検証。作業用 `/tmp/keepassxc-phase2.YxadbPNq/kdbx-unknown-header.e11kOxwo/prototype/fault_probe.cpp:84`、`:122`、`:195`、`:213`、`:226` と両ビルドの `*-test/run.log` に手順と結果を残した。両上流ツリーに差分はなく、試験は既存のビルド環境で実施した。 [既知フィールド内部の追加試験](#16-既知フィールド内部の情報欠落と読み取り異常)

## 15. 解除結果に基づく保存と秘密の利用可否

2026-11-10 に、§12 の両コミットで、KDBX 3.1、4.0、4.1 の合成データを使った。§13・§14 の試験用マーカーを、保存するモデル自身の読み込みで観測した。上流と試作のコードは既存のまま使い、試験用ドライバーで同じ判定を別名保存、自動復旧点、上流の `Database::save(Atomic)` に適用した。上書きの試験には元と同一バイト列の独立したファイルを用意した。

| 読み込み条件 | 別名保存・上書き | 自動復旧点（4.0、4.1） | 後続エントリーの合成パスワード |
| --- | --- | --- | --- |
| 比較用の既知データを観測 | 許可。保存後に編集内容を再読込できた | 許可。編集内容と復旧マーカーを再読込できた | 一致 |
| 未知のヘッダー、通常の XML 要素または属性の試験用マーカーを観測 | 拒否 | 拒否 | 一致 |
| 保護値を含む未知の XML 要素の試験用マーカーを観測 | 拒否 | 拒否 | **不一致。3形式とも解除に成功した** |
| 既知データで観測を無効化（4.1） | 拒否 | 拒否 | 一致 |

**確認済み。** 両コミットで各18ケースが合格した。拒否ケースでは `KeePass2Writer::writeDatabase` の呼び出しが0回で、出力ファイルや作業ディレクトリを追加せず、上書き試験用ファイルの SHA-256 も変化しなかった。全ケースで元の合成ファイルを維持した。自動復旧点の経路は現行試作が対応する4系を対象とした。根拠は作業用 `gtk-legacy-cross-filesystem/prototype/CoreSession.cpp:732`、`:787` と上流 `src/core/Database.cpp:237`、`:358`、`:476`。

**保護値の確認。** 上流 writer が生成した最初のパスワードの保護値をそのまま使い、その `String` 要素のタグだけを未知の名前に置き換えた。ファイル全体は通常の上流経路で暗号化し、読み込み後に別のエントリーのパスワードを合成の期待値と比較した。未知の要素を読み飛ばす処理は、内部の保護値を復号するストリームを進めない。後続の既知の保護値は同じストリームを順番に使うため、誤った位置から復号されると考えられる。根拠は `src/format/KdbxXmlWriter.cpp:416`、`:431`、`src/format/KdbxXmlReader.cpp:793`、`:1027`、`:1220` と `src/format/KeePass2RandomStream.cpp:67`。

**設計評価。** 上流のパーサーが読み飛ばしや変換を検出した箇所で、読み取りと再保存への影響を区別する型付きの状態を返す方式を第一候補とする。現行の `KeePass2Reader` が公開するのは成功・エラー、形式版、内部 reader への参照で、保持可否は取得できない。`Database::open` は reader を関数内で破棄する。鍵付きの一度の読み込みで形式版と診断を取得し、そのモデルと解除セッションの寿命に結び付ける。ヘッダーだけの読み込み、ログ警告、XML の strict mode では今回のケースを網羅できない。根拠は `src/format/KeePass2Reader.h:29`、`src/format/KeePass2Reader.cpp:60`、`src/core/Database.cpp:178`、`:1126`、`src/format/KdbxReader.cpp:99`、`src/format/KdbxXmlReader.cpp:504`、`:1220`。

| 判定 | アダプターの動作案 |
| --- | --- |
| 表示・再保存の安全性を確認できた | 認証済みの同じセッションで編集と書き込みを許可する |
| 読み取った値は正しいと確認でき、再保存で保持できない項目がある | 読み取り専用にし、編集と再シリアライズを無効にする |
| 保護値の読み取りに影響しうる、または判定できない | モデルを画面へ渡さず、解除を完了させない。パスワード表示・コピーも無効にする |

**未検証。** 実装には、上流から読み込み結果を渡す接点と、未認識データを検出する範囲の設計が残る。試験用のリンク時フックはマーカーだけを観測し、合成 XML を一時的にメモリへ保持した。製品では診断に秘密値や XML を載せず、ロック・再解除・復旧点の再読込で判定を持ち越さない。試験した三経路に加え、復旧点を経由する別名保存なども一時ファイルの作成前に判定する必要がある。検証済み暗号化バイト列をそのまま保管する復旧経路の扱いと、上書き時の外部更新競合は別の検証項目とする。根拠は作業用 `gtk-legacy-cross-filesystem/prototype/CoreSession.cpp:638`、`:949`、`:1094`。試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-write-decision.CKcIZ62H/prototype/fault_probe.cpp`、記録は同ディレクトリの両ビルドの `*-test/run.log` に置いた。

## 16. 既知フィールド内部の情報欠落と読み取り異常

2026-11-10 に、§12 の両コミットで各17ケースを実行した。添付のフラグと `PublicCustomData` は KDBX 4.0・4.1、保護属性は 3.1・4.0・4.1 を対象とした。試験用リンク時フックで値を変更してから、上流 writer で合成ファイルを作成した。外側の辞書は生バイト列を、内側の添付フラグは上流 reader が復号・展開したフィールドを確認した。保存を試したケースでは既存アダプターで別名保存して再読込した。全ケースで元ファイルの SHA-256 は変化しなかった。

| 入力の変更 | 通常の読み込み | 別名保存または照合結果 |
| --- | --- | --- |
| 添付フラグ `0x01`（対照） | 成功、警告 0 件 | フラグと添付内容を保持 |
| 添付フラグ `0x00`、`0x81` | 成功、警告 0 件 | どちらも `0x01` に変化。添付内容はバイト単位で一致 |
| `PublicCustomData` の既知の文字列型（対照） | 成功、警告 0 件 | 辞書の内容とバイト列を保持 |
| 同じ名前の項目を2件記録 | 成功、警告 0 件 | 後の値だけを保存 |
| 辞書の終端後に余剰バイトを追加 | 成功、警告 0 件 | 辞書の項目は保持。余剰バイトは消失 |
| 辞書の値型を未定義の `0x7e` に変更 | 外側ヘッダーでエラー。鍵なし・鍵付きとも失敗 | アダプターも解除を拒否 |
| 最初のパスワードの `Protected` を `True` から `Future` へ変更 | 成功、警告 0 件 | **後続のパスワードが元の値と不一致**。このケースは読み込みのみ検証 |

**添付。** 仕様の `0x01` はプロセスメモリ保護の指定。上流 reader は先頭1バイトを除いて添付の内容だけを保持し、writer は先頭に常に `0x01` を付ける。合成試験で確認したのはファイル内のフラグと添付内容の往復。実行中の添付がどのようにメモリ保護されるかは未検証。根拠は `src/format/Kdbx4Reader.cpp:292`、`src/core/EntryAttachments.h:69`、`src/format/Kdbx4Writer.cpp:221`。 [KeePass の KDBX 仕様](https://keepass.info/help/kb/kdbx.html)

**公開辞書。** `readVariantMap` は同名項目を上書きし、終端バイトで項目の読み込みを終える。フィールド末尾まで読み切ったかの確認はなく、未対応の型には既存のエラーを返す。仕様は公開辞書の名前を一意にするよう求めている。根拠は `src/format/Kdbx4Reader.cpp:221`、`:324`、`:414`、`:422`、`:429` と `src/format/Kdbx4Writer.cpp:268`、`:323`。 [KeePass の KDBX 仕様](https://keepass.info/help/kb/kdbx.html)

**保護属性。** 試験では XML の要素名と暗号化済みの値を維持し、属性値だけを変更した。KDBX 4.1 のスキーマでは `Protected` は真偽値。上流の判定は `Future` を偽として扱い、保護値を処理するストリームを進めない。この経路でも §15 と同様に後続の復号位置がずれると判断した。根拠は `src/format/KdbxXmlReader.cpp:196`、`:1027`、`:1143`。 [KeePass の KDBX 4.1 XML スキーマ](https://keepass.info/help/download/KDBX_XML.xsd)

**設計評価。** 診断の候補に、添付の未知ビット、辞書の重複名と終端後の残り、保護属性の不正値を加える。保護属性の不正値は、認証付きの読み込みが成功してもモデルを公開できない条件とする。辞書の重複・余剰バイトは再保存前に検出し、既知の添付フラグが変わる場合は保護方針と互換性を別に評価する。上流のパース箇所で判定を記録すれば、アダプターは診断用にファイルを再復号する必要がない。非圧縮の内側ヘッダー、全形式を網羅する診断の実装は未検証。試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-known-fields.9JIpNr9I/prototype/fault_probe.cpp:99`、`:155`、`:183`、`:224`、ログは両ビルドの `*-test/run.log`。上流・既存試作は変更せず、既存のビルド環境を使用した。 [KDF 辞書の追加試験](#17-kdf-設定の保持と-argon2-の相互運用)

## 17. KDF 設定の保持と Argon2 の相互運用

2026-11-10 に、§12 の両コミットで KDBX 4.0・4.1 の合成ファイルを各24ケース検証した。上流ビルドは Botan 3.10.0 を使用した。比較用の試験ドライバーでは、既存の `libargon2.so.1`（`0~20190702+dfsg-5`）の `argon2_hash` にヘッダーのバージョンを渡し、KDF だけを切り替えた。ファイルの読み書き、ヘッダー HMAC の生成・照合と暗号化は、どちらも既存の KeePassXC コードを通した。試験は合成データと少量の計算資源で実施し、全ケースで元ファイルの SHA-256 は変化しなかった。 [libargon2 の公開 API](https://github.com/P-H-C/phc-winner-argon2/blob/20190702/include/argon2.h#L295-L302)

### Argon2 のバージョン

KeePass の仕様は Argon2 のバージョンとして 1.0（`0x10`）と 1.3（`0x13`）を定義する。次の結果は Argon2d と Argon2id の両方で、KDBX 4.0 と 4.1 に共通した。 [KeePass の KDBX 仕様](https://keepass.info/help/kb/kdbx.html)

| ファイル作成時の鍵導出 | 上流 reader で解除 | バージョンを明示した比較用 reader で解除 |
| --- | --- | --- |
| `libargon2` で 1.0 を指定 | ヘッダー HMAC 不一致で失敗 | 成功 |
| 上流コアでヘッダーに 1.0 を指定 | 成功。別名保存したコピーも成功 | 元とコピーの両方でヘッダー HMAC 不一致 |
| 1.3 を指定（上流コア、`libargon2` の双方） | 成功 | 成功 |

**確認済み。** 上流 reader は鍵なしのヘッダー読み込みで `0x10` を受け入れ、モデルに保持した。上流コアで作成した `0x10` のファイルを別名保存しても、ヘッダーには `0x10` が残った。読み込みと書き込みが同じコアを使う往復だけでは、この互換性の差を検出できない。コードでは `processParameters` が版を読み、`writeParameters` がその値を出力する一方、`transform` が Botan に渡すのはメモリ量・反復回数・並列度などで、版は渡さない。今回の上流コアとライブラリの組み合わせでは、この差がヘッダー HMAC 不一致に表れた。根拠は `src/crypto/kdf/Argon2Kdf.cpp:48`、`:104`、`:140`、`:164` と `src/format/Kdbx4Reader.cpp:53`、`:77`。

### KDF 辞書の保存

| 合成ファイルの入力 | 別名保存して再読込した結果 |
| --- | --- |
| AES-KDF 辞書に試験用の追加キー | エントリーを保持し、追加キーは消失 |
| Argon2id 辞書に試験用の追加キーと `K`、`A` を記録 | エントリーを保持し、3項目は消失 |
| Argon2id の `M` を 32,769 バイトに設定 | エントリーを保持し、`M` は 32,768 バイトに変化 |
| Argon2id の `V` を文字列 `"19"` に設定 | エントリーを保持し、`V` は数値 19 の `UInt32` に変化 |

**確認済み。** 鍵付きの読み込みに成功した後も、KDF の型別モデルには入力辞書の全項目が残らない。試験用の `K` と `A` は上流 writer が鍵導出に使わない値として埋め込んだ。これらの値を実際に鍵導出へ使ったファイルの互換性は今回の範囲外。根拠は `src/format/Kdbx4Reader.cpp:207`、`src/format/KeePass2.cpp:70`、`:94`、`src/crypto/kdf/AesKdf.cpp:39`、`:51`、`src/crypto/kdf/Argon2Kdf.cpp:96`、`:140`。

**設計評価。** KDF 辞書をモデルへ変換する際に、アルゴリズムと版に加えて、未使用項目、型の変換、値の丸めを診断できる接点が必要。Argon2 1.0 の相互運用性は書き込み可能な版の合格条件に含める。版を正しく扱う上流修正を評価する際は、1.0 の参照実装で生成したファイルと、1.0 と記録して現行コアで生成したファイルの両方を用意し、読み込みと移行の扱いを検証する。今回確認したのは合成ファイルと固定した二つのビルドでの相互運用で、他の KeePass 実装や実在のファイル、修正後の互換性は未検証。試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-kdf-compat.9I2R4uEP/prototype/fault_probe.cpp:57`、`:80`、`:179`、`:284`、`:327`。同ディレクトリの両ビルドの `*-test/run.log` に結果を記録した。上流・既存試作には変更を加えず、追加のパッケージは導入しなかった。

## 18. 認証前の KDF 入力と計算量の制限

2026-11-10 に、§12 の両コミットで各26ケースを検証した。低負荷の AES-KDF（KDBX 3.1・4.0・4.1）と Argon2id（4.0・4.1）の対照ファイルを上流 writer で生成し、メモリ内のコピーのヘッダーを変更した。試験用 reader は上流パーサーが実際に読んだヘッダーと KDF モデルを受け取る。高負荷・不正値のケースでは、KDF 呼び出しを数えて直ちに失敗を返す試験用オブジェクトを使った。計算を実行したのは、低負荷の対照と認証エラーの順序を確認したケースだけ。

| 合成ヘッダーの条件 | 上流が鍵なしで読み取った値と、鍵付きの処理順序 |
| --- | --- |
| AES-KDF の反復 100,000,000 回（3.1・4.0・4.1） | その回数を受け入れ、鍵付きの処理は KDF 呼び出しに到達 |
| Argon2id のメモリ 1 GiB、反復 100,000 回、並列度8を個別に指定（4.0・4.1） | 各値を受け入れ、KDF 呼び出しに到達。1 GiB のケースは公開 SHA-256 を再計算した |
| Argon2id のメモリ 128 KiB に並列度32（4.0・4.1） | 個々の値を受け入れ、KDF 呼び出しに到達。組み合わせは Argon2 の最小メモリ条件を満たさない |
| 反復回数を64ビットで `2^32 + 1,000`（AES-KDF）または `2^32 + 2`（Argon2id）に設定 | 対象の全形式で、それぞれモデル内の `1,000`、`2` に変化し、KDF 呼び出しに到達 |
| AES-KDF の反復0回（3.1） | モデル内では1回になり、KDF 呼び出しに到達 |
| 低負荷の入力で SHA-256 または HMAC の1バイトだけを変更（4.0・4.1） | 実際の KDF を1回実行した後、それぞれの不一致で拒否 |

**確認済み。** 上流のヘッダー処理を通った高負荷・不正値の17ケースでは、試験用の判定を有効にすると KDF 呼び出しは0回になった。低負荷の正常な5ケースは通過し、合成エントリーを認証・復号できた。上流では KDBX 3.1 の反復を `quint64` から `int` に変換し、`setRounds` の失敗を伝播しない。KDBX 4 の AES-KDF は `toInt` を使い、Argon2 の反復も `int` を受け取る `setRounds` に渡す。判定には変換前の型と値が必要。根拠は `src/format/KdbxReader.cpp:194`、`src/crypto/kdf/Kdf.cpp:44`、`src/crypto/kdf/AesKdf.cpp:39`、`src/crypto/kdf/Argon2Kdf.cpp:96`、`:114`、`src/crypto/kdf/Kdf.h:36`。 [KeePass の KDBX 仕様](https://keepass.info/help/kb/kdbx.html) [RFC 9106 の Argon2 入力条件](https://www.rfc-editor.org/rfc/rfc9106.html#section-3.1)

**判定を置く位置。** KDBX 4 の上流 reader は `Database::setKey` の後で SHA-256 と HMAC を照合する。KDBX 3.1 でも鍵導出が復号済みの開始バイトや内部のヘッダーハッシュの照合に先行する。公開 SHA-256 は再計算できるため、照合を前に移しても計算量の制限は必要。判定は、実際に解除する読み込みが受け取ったヘッダーの値に適用する。現行試作はヘッダーと鍵付き読み込みでパスを別々に開くため、先に読んだ値だけで後の読み込みを許可する方式には、ファイルが差し替わる余地がある。根拠は `src/format/KdbxReader.cpp:88`、`:99`、`src/format/Kdbx3Reader.cpp:50`、`:78`、`:123`、`src/format/Kdbx4Reader.cpp:53`、`:64`、`:77`、`src/core/Database.cpp:977` と作業用 `gtk-legacy-cross-filesystem/prototype/CoreSession.cpp:673`、`:679`。 [KeePass のヘッダー認証仕様](https://keepass.info/help/kb/kdbx.html)

**設計評価。** 上流パーサーで元の値を検証してから数値を変換し、同じ読み込みの鍵導出前に計算量の上限を適用する方式を候補とする。入力不正と、正当な設定が実行環境の上限を超えた場合を区別する。試験の上限は停止動作を確認するための例で、製品の上限値、複数の解除要求が重なったときの上限、ロック時の停止は未検証。今回の判定はヘッダーが読み終わった後に入るため、ヘッダーの長さ指定そのものに対するメモリ制限も別途必要。試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-kdf-limits.V27vyg9t/prototype/fault_probe.cpp:161`、`:191`、`:222`、`:333`。同ディレクトリの両ビルドの `*-test/run.log` に記録した。上流・既存試作には変更を加えず、追加のパッケージは導入しなかった。 [外側ヘッダーの読み込み上限の試験](#19-認証前のヘッダー読み込み上限)

## 19. 認証前のヘッダー読み込み上限

2026-11-10 に、§12 の両コミットと Qt 6.10.2 で各36ケースを検証した。KDBX 3.1・4.0・4.1 の対照は上流 writer で作成した。多数のフィールドを持つ対照は、上流 writer がヘッダーの認証情報を作る前に試験用フィールドを入れた。切り詰めた入力と不正な長さはメモリ内の合成ヘッダーで作成した。上流が Qt に渡す読み込み長を計測し、256 KiB を超える要求は試験用の安全措置として Qt へ渡す前に遮断した。

| 合成入力 | 上流が読み込みを要求した内容 | 試験用の判定 |
| --- | --- | --- |
| 外側フィールドの宣言長100、実データ4バイト | 3形式とも100バイトを要求し、短い読み込みの後で拒否 | 残量を確認し、要求を Qt へ渡す前に停止 |
| 外側フィールドの宣言長65,535（3.1）、`0x7fffffff`・`0xffffffff`（4.0・4.1）、実データ4バイト | 宣言どおりの長さを要求。4系の大きな要求は安全措置で遮断 | 1フィールドの上限で停止 |
| KDF 辞書と公開カスタム辞書の名前・値の宣言長100または`0x7fffffff`、残り4バイト（4.0・4.1） | どちらの辞書でも宣言どおりの長さを要求。大きな要求は安全措置で遮断 | 辞書内の残量の確認で停止 |
| 4 KiB のフィールドを5件追加 | 上流で認証・復号に成功 | 1件ごとの上限内でも、ヘッダー合計の上限で停止 |
| 外側ヘッダーのフィールド総数32件と33件 | 上流では両方とも認証・復号に成功 | 32件は許可し、33件目を読む前に停止 |

**確認済み。** 1フィールド8 KiB、ヘッダー全体16 KiB、総数32件を試験値として使い、30ケースを鍵導出・認証処理に入る前に止めた。対照6ケースは認証・復号に成功し、4系の対照では2 KiB の公開カスタム値も維持した。KDBX 4 の仕様が外側フィールド長を符号付き32ビットとして定義する一方、今回の上流 reader は `quint32` として読み、最上位ビットを立てた入力も正の長さで Qt に渡そうとした。根拠は `src/format/Kdbx3Reader.cpp:148`、`:156`、`src/format/Kdbx4Reader.cpp:167`、`:175`、`:330`、`:338`、`:347`、`:355`。 [KeePass の KDBX 仕様](https://keepass.info/help/kb/kdbx.html)

**確保の位置。** Qt 6.10.2 の `QIODevice::read(qint64)` は、今回のように要求と同じサイズの内部バッファーを再利用できない経路では、実データを読む前に要求サイズへ戻り値の配列を拡張する。上流の読み取り長の照合はその後。外側ヘッダーは `StoreDataStream` でも蓄積し、鍵付き読み込みのヘッダー認証に使う。したがって、短い読み込みでエラーにするだけでは、事前の確保やヘッダー全体の蓄積を制限できない。巨大な領域の確保や実メモリ消費量の測定は実施していない。根拠は `src/streams/StoreDataStream.cpp:41`、`src/format/KdbxReader.cpp:77`、`:104`。 [Qt 6.10.2 の読み込み処理](https://github.com/qt/qtbase/blob/v6.10.2/src/corelib/io/qiodevice.cpp#L1101-L1133)

**設計評価。** 上流のヘッダー処理で、型ごとの固定長、入力の残量が分かる場合の残量、フィールド単体とヘッダー全体のバイト数・件数を、データ読み込み前に確認する。辞書の名前・値は辞書自身の残量で照合し、上限超過と入力の欠落を区別して返す。上限値は試験用であり、実データとの互換性と辞書の項目数上限は未検証。 [復号後の読み込み上限の追加試験](#20-暗号化データブロックと復号後の読み込み上限) 試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-header-bounds.s9XUl6w7/prototype/fault_probe.cpp:138`、`:201`、`:236`、`:263`、`:306`。同ディレクトリの両ビルドの `*-test/run.log` に結果を記録した。既存の上流・試作コードとパッケージを使用した。

## 20. 暗号化データブロックと復号後の読み込み上限

2026-11-11 に、§12 の両コミット、Qt 6.10.2、zlib 1.3.1 で各40ケースを検証した。KDBX 3.1・4.0・4.1 の圧縮あり・なしの合成ファイルを上流 writer で生成した。対照は8 KiB の添付とエントリーを含み、制限を適用しても内容を読み取れた。試験用フックでブロックと内側フィールドの読み込み長、zlib が出力した総量、XML パーサーへ渡した量を計測した。32 KiB の展開・XML 上限、16 KiB の内側フィールド・合計上限、64 KiB のブロック上限は停止動作を確認するための試験値。

| 合成入力 | 確認した結果 |
| --- | --- |
| ヘッダーを維持し、最初の HMAC ブロック長だけを `0x7fffffff` に変更（4.0・4.1、圧縮あり・なし） | 上流は約2 GiB の読み込みを要求。試験用の判定で Qt に渡す前に停止した |
| ブロック長を負に変更、または最初のブロックの暗号文1バイトを変更（4.0・4.1、圧縮あり・なし） | 上流が拒否し、圧縮展開量は0、XML パーサーも未呼び出し |
| 内側の添付長を `0x7fffffff` にして上流 writer で暗号化・認証（4.0・4.1、圧縮あり・なし） | 上流は約2 GiB を要求。内側フィールドの判定で確保前に停止した |
| 32 KiB の添付1件、8 KiB の異なる添付3件（4.0・4.1、圧縮あり・なし） | 通常は内容を読み取れた。単体上限と、内側ヘッダー全体の上限でそれぞれ停止した |
| 128 KiB の Notes（3.1・4.0・4.1、圧縮あり・なし） | 通常は内容を読み取れた。XML パーサーに渡す量を32 KiB 以下に抑えて拒否した |
| 圧縮した128 KiB の Notes と、3.1 の圧縮した64 KiB の添付 | zlib の出力先容量を制限すると、展開総量32 KiB 以下で拒否した。3.1 は外側の Gzip と XML 内添付の Gzip を合算した |

**ブロックの認証。** 4系の reader はヘッダー HMAC を検証してからブロックストリームを開く。しかし各ブロックは、その内容を読み込んだ後にブロック自身の HMAC を照合する。ヘッダーを変更しない長さの改変でも、その照合前の読み込み要求を制限する必要がある。巨大な要求は今回も安全措置で捕捉し、実際には確保していない。根拠は `src/format/Kdbx4Reader.cpp:77`、`:82` と `src/streams/HmacBlockStream.cpp:124`、`:142`、`:149`、`:162`。 [KeePass の HMAC ブロック仕様](https://keepass.info/help/kb/kdbx.html)

**展開と XML の量。** XML パーサーへ渡す量だけを32 KiB に制限した試験では、圧縮ありの4系で zlib の出力が49,152バイトになった。内側ヘッダー・添付の処理や圧縮層の先読みもあるため、XML の配送量と展開総量は別に数える。3.1 の添付は XML 内の圧縮データをさらに展開する経路を持つ。根拠は `src/format/Kdbx4Reader.cpp:104`、`:119`、`:146`、`src/streams/qtiocompressor.cpp:525`、`:553`、`src/format/KdbxXmlReader.cpp:1165` と `src/core/Tools.cpp:172`。 [zlib の展開 API](https://zlib.net/manual.html)

**設計評価。** 制限を置く候補は、ブロック長を読む箇所、内側フィールドの確保前、展開出力を作る箇所、XML の値やオブジェクトを作る箇所。展開・添付の総量は解除セッション内で合算し、失敗時の部分モデルを画面へ渡さない。試験の小さなブロック上限は製品値にできないため、通常の1 MiB ブロックを含む既存ファイルとの互換性を調べて上限を決める。件数と階層は次節の試験で扱う。残る課題は、製品の上限値、値単体の長さ、計算時間・並列要求と組み合わせた上限、ロック時の停止、実メモリの最大使用量。グループは再帰的に読み、履歴もエントリーのパーサーを使う。根拠は `src/format/KdbxXmlReader.cpp:491`、`:590`、`:689`、`:973`。 [KeePass のブロックサイズの説明](https://keepass.info/help/kb/kdbx.html)

試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-payload-bounds.4dlc5moD/prototype/fault_probe.cpp:194`、`:229`、`:247`、`:270`、`:283`、`:340`、`:373`。同ディレクトリの両ビルドの `*-test/run.log` に結果を記録した。ランダムな暗号化値による圧縮ファイル長の差を除き、判定結果と計測値は一致した。調査では既存の上流・試作コードと導入済みライブラリを使用した。

## 21. XML の階層とオブジェクト件数

2026-11-11 に、§12 の両コミットで各84ケースを検証した。KDBX 3.1・4.0・4.1 の圧縮あり・なしで、上流 writer が作った小さな合成ファイルを使用した。試験用ドライバーは復号後の XML を256バイトずつ検査してから上流 reader に渡し、4系の添付データは内側ヘッダーの ID を読んだ時点で件数を数えた。暗号・KDBX・モデルの読み込みは上流の既存処理を使った。製品では、この追加の XML パーサーに代えて、上流がモデルを作る箇所に判定を置く設計を評価する。

| 試験用の制限 | 上限ちょうどの合成入力 | 超過させた合成入力 |
| --- | --- | --- |
| グループの階層4（ルートを含む） | 深さ4 | 深さ5 |
| グループ総数8（ルートを含む） | 8グループ | 9グループ |
| 現在のエントリー総数8 | 8件 | 9件 |
| エントリーごとの履歴4 | 1エントリーに4件 | 1エントリーに5件 |
| DB 全体の履歴8 | 2エントリーに各4件 | 3エントリーに各3件、合計9件 |
| 異なる添付データ8 | 内容が異なる添付8件 | 内容が異なる添付9件 |
| XML 内の添付参照8 | 同じデータを参照する添付名8件 | 同じデータを参照する添付名9件 |

**確認済み。** 制限を適用しない読み込みでは、全入力のグループ、現在のエントリー、履歴とその合成パスワード、添付の内容を照合できた。制限を適用した読み込みでは、上限ちょうどの42ケースを許可し、超過の42ケースを拒否した。試験用セッションは reader が成功したときだけモデルを公開し、拒否時は候補 DB の破棄を `QPointer` で確認した。各拒否の後に同じセッションで正常な入力を開き直し、ロック後にその DB オブジェクトも破棄された。これは DB オブジェクトの寿命の試験で、パーサー内部の一時オブジェクトと秘密のコピーの全消去は未検証。根拠は作業用 `/tmp/keepassxc-phase2.YxadbPNq/kdbx-object-limits.t1Rvty9I/prototype/fault_probe.cpp:125`、`:312`、`:348`、`:385`。

**件数の分け方。** 添付名9件が同じ内容を参照する入力では、実体の添付データは1件だった。実体の件数だけでは XML 内の参照とモデルの項目数を制限できない。履歴は現在のエントリーとは別のオブジェクトを作り、1エントリーあたりの上限内でも DB 全体では増える。根拠は `src/format/KdbxXmlReader.cpp:689`、`:759`、`:973`、`src/format/Kdbx4Reader.cpp:292`、`src/format/Kdbx4Writer.cpp:214`、`src/format/KdbxXmlWriter.cpp:460`。 [KeePass の添付参照の仕様](https://keepass.info/help/kb/kdbx.html)

**設計評価。** グループの再帰処理に入る前、エントリー・履歴を作る前、添付データと参照を保持する前に、解除セッション内の予算を確認する。さらに上流は、メタデータのグループ参照や最終選択エントリーから仮のオブジェクトを作る。グループ・エントリーのタグの数だけでは、これらの確保を数え切れない。上流の `getGroup`、`getEntry` が未登録の UUID からオブジェクトを作る経路と、一時オブジェクトも含めた評価が必要。根拠は `src/format/KdbxXmlReader.cpp:281`、`:491`、`:586`、`:689`、`:1184`、`:1202`。

**試験の限界と末尾の扱い。** 件数の上限は試験値で、すべての制限を同時に有効にした場合の互換性、処理時間と実メモリの最大値は未検証。検査は上流 reader が処理する `KeePassFile` 要素の終わりまでに限定した。検査をそれより先へ進めた初回の試験では、上流 writer で作った非圧縮 KDBX 4.0 の復号ストリームに、閉じタグと改行の後で `0x01` が1バイト残り、追加の XML パーサーがエラーにした。既存 reader と追加検査の終了位置の差として記録した。4系の暗号ストリーム末尾、パディング、終端ブロックは次節で追加検証した。根拠は `src/format/KdbxXmlReader.cpp:111`、`:216`、`src/streams/SymmetricCipherStream.cpp:124`、`:152`、`src/streams/HmacBlockStream.cpp:258` と、作業用の `upstream-build-test/diagnostic-stderr.log`。 [Qt の逐次 XML 解析](https://doc.qt.io/qt-6/qxmlstreamreader.html#incremental-parsing)

同ディレクトリの両ビルドの `*-test/run.log` に84ケースの結果を記録し、一致を確認した。上流・既存試作には変更を加えず、既存のビルド環境を使用した。

## 22. XML 終端と暗号ストリームの検証完了

2026-11-11 に、§12 の両コミットで各112ケースを検証した。KDBX 4.0・4.1、AES-CBC・ChaCha20、圧縮あり・なしを対象とした。上流 writer が作る通常の合成ファイルと、XML のルート要素の後に約256 KiB の正当なコメントを加えたファイルを用意した。コメントも上流の圧縮・暗号化・HMAC の経路に通した。全入力は1 MiB 未満。

比較用ドライバーは、既知の合成キーと上流の `HmacBlockStream` で終端まで読み、その後に上流の `SymmetricCipher::finish` で暗号処理を完了させた。ヘッダーとその HMAC は対照と同じバイト列を維持し、終端ブロック、パディング、余剰バイトの状態を比較した。この比較は小さな合成入力用で、製品の読み込み経路には組み込んでいない。

| 変更した箇所 | 大きな末尾コメントなし | 大きな末尾コメントあり | 終端までの比較用検査 |
| --- | --- | --- | --- |
| 正常な対照 | 読み込み成功 | 読み込み成功 | 正常 |
| 終端ブロックの HMAC の1バイト | 拒否 | **読み込み成功** | HMAC エラー |
| 終端ブロック全体を削除 | 拒否 | **読み込み成功** | 終端欠落のエラー |
| 終端ブロックの最後の1バイトを削除 | 拒否 | **読み込み成功** | 終端のサイズエラー |
| 最初のデータブロックの HMAC の1バイト | 拒否 | 拒否 | HMAC エラー |
| 正常な終端の後に1バイトを追加 | 読み込み成功 | 読み込み成功 | 余剰バイトを検出 |
| AES のパディングだけを不正にしてデータブロックの HMAC を再生成 | **読み込み成功** | **読み込み成功** | パディングのエラー |

**確認済み。** 上流 reader は成功を返したケースで、合成のタイトルとパスワードを保持していた。末尾コメントがある24ケースでは、破損・欠落した終端の HMAC を読む前に成功を返した。AES のパディング試験8ケースは、上流の暗号処理で得たパディング付きの合成平文の最後の1バイトだけを0にし、既存の暗号処理と HMAC writer で包み直した。データブロックの HMAC は正常で、`finish` は拒否するが、通常の reader は成功した。鍵を知らずにパディングを改変できることを試したケースではない。

**コード上の接点。** XML reader は `KeePassFile` の子要素を読み終えると戻る。AES の読取ストリームは、基底ストリームの `atEnd()` が真のときに `finish` を呼ぶ一方、HMAC ストリームの `atEnd()` はゼロ長の終端ブロックを読んでから真になる。今回の AES の正常対照8ケースでも、通常の読み込みでは `SymmetricCipher::finish` の呼び出しを観測しなかった。読み取り側の `close` は内部状態をリセットする。これらは §21 の末尾1バイトが残った観測と整合する。根拠は `src/format/KdbxXmlReader.cpp:111`、`:212`、`src/streams/SymmetricCipherStream.cpp:79`、`:124`、`:152`、`src/streams/HmacBlockStream.cpp:124`、`:171`、`:258` と `src/crypto/SymmetricCipher.cpp:99`。 [KeePass の終端ブロック仕様](https://keepass.info/help/kb/kdbx.html) [Botan の暗号処理の完了操作](https://botan.randombit.net/handbook/api_ref/cipher_modes.html)

**既存アダプターでの確認。** AES の末尾コメント付き入力を使い、4.0・4.1 と圧縮あり・なしで、終端欠落と不正パディングの計8ケースを解除し、別名保存できた。元の合成ファイルのバイト列は維持され、保存したコピーは終端・HMAC・パディングの比較用検査に通った。入力の検証が完了していないことは、別名保存の成功だけでは分からない。根拠は作業用 `gtk-legacy-cross-filesystem/prototype/CoreSession.cpp:664`、`:732`。

**設計評価。** モデルを公開する前に、XML の完了、各データブロックと終端の検証、暗号処理の完了を確認する接点が必要。検証のために残りを読む場合にも §18〜§21 の計算量・読み込み量・件数の上限を維持する。既存の上流ストリームに検証完了を明示して返す変更を評価し、アダプターはその結果を受け取る方式を候補とする。末尾の余剰データの扱い、KDBX 3.1、Gzip の終端と CRC、通常のブロック境界を多数またぐ入力、ロックによる中断と同時に起きる場合は未検証。試験用ドライバーは合成キーと復号データを一時保持したため、製品のメモリ管理方式も別途必要。

試験コードは `/tmp/keepassxc-phase2.YxadbPNq/kdbx-stream-finalization.dMVXkt00/prototype/fault_probe.cpp:199`、`:248`、`:266`、`:286`、`:354`。両ビルドの `*-test/run.log` の112ケースの観測は一致した。今回の調査では、上流・既存試作を変更せずに問題を記録した。追加のパッケージは導入していない。
