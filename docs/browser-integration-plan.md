# ブラウザー連携の実装調査（2026-11-19）

KeePassXC GNOME v0.2.0を基準に調査した。最初の実装は、公式KeePassXC-Browser拡張から、すでに解除した一つのDBに対して接続を承認し、対象サイトのユーザー名とパスワードを選んで入力するところまでを推奨する。検証はT16にも導入済みのChrome（deb）から始める。新規保存・更新、パスキー、TOTP、Auto-Type、HTTP認証、ブラウザーからの解除要求は別の段階で扱う。

## 本家の構成と再利用の境界

公式拡張は `org.keepassxc.keepassxc_browser` という固定の名前でNative Messagingホストに接続する。ブラウザーは登録マニフェストで起動先と許可された拡張を決め、補助プロセスを標準入出力で呼び出す。本家の `keepassxc-proxy` はアプリのローカルソケットに中継し、アプリ側がブラウザーとの鍵交換、DBごとの接続登録、資格情報の選別と返答を担う。標準入出力はUTF-8 JSONとバイト長で区切られる。本家ソケット側の区切りとは別に扱う。[公式拡張の接続先](https://raw.githubusercontent.com/keepassxreboot/keepassxc-browser/1.10.4.1/keepassxc-browser/background/client.js)・[ChromeのNative Messaging仕様](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging)・[FirefoxのNative Messaging仕様](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging)。

| 対象 | 確認した根拠 | 方針 |
| --- | --- | --- |
| 接続、暗号化、応答形式 | `upstream/keepassxc/src/browser/BrowserMessageBuilder.{h,cpp}`、`BrowserShared.h`、`BrowserAction.cpp` | 暗号化・復号・nonce処理は、固定済み2.8.0の `BrowserMessageBuilder` と既存のBotanを使う。動作確認した範囲のアクションだけを処理する。 |
| 接続仲介と登録 | `upstream/keepassxc/src/proxy/NativeMessagingProxy.cpp`、`src/browser/BrowserHost.cpp`、`BrowserShared.cpp`、`NativeMessageInstaller.cpp` | 本家を仕様の参照にし、補助プロセスとアプリ間にはGNOME専用のソケットを使う。上流のソケットパス関数には旧パスのリンクを変更する処理があるため、候補コードからそのまま呼ばない。 |
| 承認とDB操作 | `upstream/keepassxc/src/browser/BrowserService.cpp`、`BrowserEntryConfig.cpp`、`src/gui/UrlTools.cpp` | `BrowserService` はQtのMainWindow・ダイアログ・DBタブと結合している。承認画面はGTKに接続し、設定データの読み取りとURLの解析は既存コードの再利用を検討する。 |
| ビルド | ローカル `CMakeLists.txt`、`cmake/Frontend.cmake`、上流 `CMakeLists.txt` と `src/browser/CMakeLists.txt` | 現状の `KPXC_MINIMAL=ON` はブラウザー機能を無効にする。上流のbrowserターゲット全体はQt Widgetsをリンクするため、必要なファイルを個別のターゲットへ接続する。 |

既存SDKのBotan 3.10.0に `botan/sodium.h` とSodium互換APIが含まれることを確認した。固定済み2.8.0の `BrowserMessageBuilder.cpp` を変更せずにQt CoreとBotanへリンクし、合成JSONを2組の鍵で暗号化・復号できた。 `BrowserEntryConfig.cpp`、`BrowserShared.cpp`、`UrlTools.cpp` も単体の構文検査を通過した。これは既存依存での再利用の見通しであり、公式拡張との通信・配布パッケージの依存解決は実装時に検証する。[BotanのSodium互換API](https://botan.randombit.net/handbook/api_ref/sodium.html)・[KeePassXC 2.8.0のリリース](https://github.com/keepassxreboot/keepassxc/releases/tag/2.8.0)。

## ブラウザーと本家との併存

T16のパッケージ情報ではChromeとBraveはdeb、FirefoxのdebはSnapへの移行パッケージだった。ブラウザーの既存プロファイルや登録ファイルは読み取っていない。

| 環境 | 検証順序 | 登録の確認先 |
| --- | --- | --- |
| Chrome（deb） | 初回候補。Ubuntu 26.04 VMの隔離した利用者環境で公式拡張を検証し、T16で最終確認する。 | 利用者用 `~/.config/google-chrome/NativeMessagingHosts/` とシステム領域の優先関係。 |
| Brave（deb） | 同じChromium系として、Chromeが通った後に実ブラウザーで確認する。 | 上流の利用者用パス `~/.config/BraveSoftware/Brave-Browser/NativeMessagingHosts/`。 |
| Firefox（非Snap） | Firefoxも対象にする場合の別シナリオ。 | `~/.mozilla/native-messaging-hosts/` と `allowed_extensions`。 |
| Firefox（Snap） | ブラウザーの配布形態としての対応は別途判断する。 | 制限されたFirefoxからホスト側を起動するPortalまたはNative Messaging proxy。T16で利用できる経路と許可手順は未検証。 |

Chrome系は `allowed_origins`、Firefoxは `allowed_extensions` で公式拡張だけに絞る。マニフェストの `path` はGNOME側の専用補助プログラムの絶対パスを指定する。Snap / Flatpak版の本アプリを配布する話と、deb版の本アプリをSnapブラウザーから利用する話は別の判断になる。[Chromeの登録パス](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging)・[本家の登録ファイル例](https://github.com/keepassxreboot/keepassxc-browser/wiki/Native-messaging-script-files)・[Mozillaの隔離環境の設計](https://firefox-source-docs.mozilla.org/toolkit/components/extensions/webextensions/native-messaging-portal-design.html)。

同じ設定検索範囲では、公式拡張の固定ホスト名に対応する起動先は一つ。GNOME側でホスト名だけを変更しても公式拡張からは見つからない。初期実装は既定で無効にし、明示的に有効化したときだけ登録する。既存ファイルやシステムの登録と競合する場合は変更を止めて案内を出す。VMの別ユーザー・別ブラウザーで検証し、単に新しいブラウザープロファイルを作れば登録先も分かれるとは仮定しない。無効化は自アプリが所有する登録だけを撤去する。同じブラウザーで本家と明示的に切り替える仕組みが必要なら、既存連携の復元方法を含めて別途設計する。[拡張の固定ホスト名](https://raw.githubusercontent.com/keepassxreboot/keepassxc-browser/1.10.4.1/keepassxc-browser/background/client.js)・[本家の連携手順](https://keepassxc.org/docs/KeePassXC_UserGuide#_browser_integration)。

## 実装順序と確認項目

1. **公式拡張での接続を合成応答で確認する。** 補助プロセスのパッケージ名と実行ファイル名を本家と分け、GNOME専用のUnixソケットを使う。ログイン利用者のruntimeディレクトリ内で所有者・権限・接続元UIDを確認し、TCP待受や本家への自動転送は追加しない。UIDで確認できるのはOS利用者までであり、資格情報の開示には後段の接続登録と承認が必要になる。アプリは現在 `G_APPLICATION_NON_UNIQUE` なので一つのインスタンスだけが接続を受け付け、競合時は明示する。短い読み取りや複数フレーム、UTF-8、入出力の長さ、接続数と待ち時間を制限する。接続できた段階では実DBや秘密を返さない。
2. **鍵交換と接続登録をworkerへ接続する。** `BrowserAction.cpp` と公式拡張の1.10.4.1を基準に、`change-public-keys`、`get-databasehash`、`associate`、`test-associate` の動作を確認する。接続情報はソケットごとに持ち、名前やIDだけで認証しない。初回は解除済みDBと接続名を示してGTKで承認し、同名の既存登録を上書きしない。本家のDBメタデータ `KPXC_BROWSER_*` を読み書きする場合は既存の編集・保存・復旧と同じ経路を使い、暗号化チェックポイントを作れた後に成功を返す。既存の本家ペアリングは保持し、GNOMEでの初回接続は明示承認する。アプリ表示版はそのままにし、拡張が返答の版情報から機能を判定するため、どの版情報で未対応機能が表示されるかも公式拡張で確認する。[拡張の版判定](https://raw.githubusercontent.com/keepassxreboot/keepassxc-browser/1.10.4.1/keepassxc-browser/background/keepass.js)。
3. **承認済みサイトへの入力を追加する。** `get-logins` は現在の一つのDBに限定する。ページとフォーム送信先を正規化したうえで、初期候補はスキーム・ホスト・実効ポートが一致するHTTPSに絞る。似た名前のドメイン、異なるポート、送信先違い、クロスオリジンのiframe、期限切れ、ごみ箱、非表示・拒否・ペアリング制限のあるエントリーを検証する。意味をまだ扱えない許可設定は情報を返さない方向で判断し、元の設定を保持する。GUIには選択のための名称やサイトだけを渡し、workerが承認と接続・要求・DBの世代を再照合してから、資格情報を暗号化した応答を返す。初期は都度の明示承認、拡張での選択による入力まで。自動送信や曖昧なURL一致は別の検証項目にする。
4. **ロックと終了を連携に接続する。** `src/main.cpp` の `Window::conceal()`、`lock()`、`automaticLock()`、`src/gnome/SessionGuard.cpp` とworker停止を既存の境界にする。パスワード・キーファイルによる解除が完了するまでは資格情報を返さない。画面ロック・スリープ・非アクティブ・DB切替・終了・連携無効化・worker異常時は承認待ちを閉じ、保留中の結果とセッション鍵を失効させる。既出の情報はブラウザー側の管理になるため、拡張へロック通知を送り、キャッシュの無効化と再接続時の再確認を実ブラウザーで検証する。実装できる場合は `lock-database` を同じロック経路へ接続する。
5. **梱包と互換性を確認する。** `src/gnome/SessionProtocol.h` の私有GUI–worker通信は外部ソケットへ公開せず、ブラウザー専用の操作を検証する。補助プログラムを `cmake/Frontend.cmake`、`packaging/Install.cmake`、`tools/build-deb.py` の配置とmanifest検査へ加える。公式拡張の版を固定したテスト、合成HTTPSフォームの実ブラウザー試験、無権限・誤照合・ロック中の要求・承認中ロック・中断・応答上限の回帰を通す。続けてKDBX3.1/4.0/4.1の保存と復旧、本家設定の往復、VMでの導入・更新・削除、T16での実機確認を行う。

Chromeのホストからの応答上限は1 MB。本家は暗号化前のサイズにも上限を設けている。上限は暗号データのBase64化と外側のJSONを含めて検証する。固定上流2.8.0に含まれるブラウザー連携の修正を保持し、取り込み時に最新の上流セキュリティ情報と照合する。[Chromeのサイズ制限](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging#native-messaging-protocol)・[本家の上限に関する修正情報](https://github.com/keepassxreboot/keepassxc/security/advisories/GHSA-3448-gh8j-3227)。

## 第1段階の接続試験の再現手順

Ubuntu 26.04.1 amd64 の隔離VMで、Chrome for Testing 158.0.8127.36 と公式 KeePassXC-Browser 1.10.4.1 のChromium版ZIPを使用した。ZIPのSHA-256は `3ccda612a951dd4788b424cd41adced832922d4e482ea3ee03c6013390f45842`。ZIPに拡張IDを固定するキーが含まれないため、展開先の絶対パスからChromiumが生成するIDを登録に使用した。登録先は今回の一時プロファイル直下の `NativeMessagingHosts/`。[Chrome for Testingの取得元](https://googlechromelabs.github.io/chrome-for-testing/)・[公式拡張のリリース](https://github.com/keepassxreboot/keepassxc-browser/releases/tag/1.10.4.1)・[登録ファイルの検索仕様](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging#native-messaging-host-location)。

既存の `build/native` を準備済みの開発環境で、次のターゲットを明示的にビルドする。

```bash
source tools/dev-env.sh
cmake --build build/native --target gnome_browser_connection_probe
```

VMに `build/bin/gnome_browser_connection_probe` と `tests/browser_connection_probe.py` をコピーする。VMにあるQt 6 CoreとBotan 3のランタイムを使用できる。UbuntuのAppArmorが一時配置のChromeをブロックする場合は、VM内でrootが管理するディレクトリにChromeを置き、そのChrome実行ファイルの絶対パスだけに `userns` を許可する一時プロファイルを `apparmor_parser -r` で読み込む。終了時は `apparmor_parser -R` で取り除く。ChromeのサンドボックスとVM全体のユーザー名前空間制限は有効のまま試験する。[ChromiumのAppArmor手順](https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md)。

VMの一般ユーザーで引数に展開済みファイルを指定する。

```bash
timeout 35s python3 browser_connection_probe.py \
  /path/to/chrome-linux64/chrome /path/to/extension /path/to/gnome_browser_connection_probe
```

`tests/browser_connection_probe.py` は一時プロファイル・専用Unixソケット・Native Messagingホストを作成し、Chromeの標準入出力とソケットの間でフレームを中継する。拡張の呼び出し元とソケット双方のUIDを照合し、フレームを64 KiBに制限する。拡張の起動時の鍵交換と、固定の合成ハッシュを復号できたことをDevToolsパイプで確認する。`tests/browser_connection_probe.cpp` は上流の `BrowserMessageBuilder.cpp` を使用し、接続ごとの鍵を持つ。ソケットは試験中だけ存在する。製品ビルドの標準ターゲットやdebには試験用実行ファイルを含めない。

## 通常のChromeとWeb Store版での接続試験（2026-11-20）

Ubuntu 26.04.1 amd64 VMで、Google公式の `google-chrome-stable` 158.0.8127.36-1 のdebを `dpkg-deb -x` で展開した。展開先Chromeだけに一時AppArmorプロファイルを適用し、Chromeのサンドボックスは有効。テスト用のmount namespaceでのみ一時ディレクトリを `/etc/opt` にbind mountした。管理ポリシー `ExtensionInstallForcelist` に `oboonakemofpalcgghocfoadofidjkkk;https://clients2.google.com/service/update2/crx` を設定してWeb Storeから導入した。試験中のDevTools観測のため、同じ一時ポリシーに `DeveloperToolsAvailability: 1` を設定した。[ChromeのLinuxポリシー](https://support.google.com/chrome/a/answer/9027408)・[拡張のインストールポリシー](https://chromeenterprise.google/policies/#ExtensionInstallForcelist)・[公式KeePassXC-Browserのストア掲載](https://chromewebstore.google.com/detail/keepassxc-browser/oboonakemofpalcgghocfoadofidjkkk)。

`tests/browser_connection_probe.py` の拡張ディレクトリ引数を省略し、展開済みChromeと合成レスポンダーを指定する。スクリプトは一時プロファイルの登録で拡張のストアIDを唯一の `allowed_origins` に指定する。

```bash
timeout 75s python3 browser_connection_probe.py \
  /path/to/extracted/opt/google/chrome/chrome /path/to/gnome_browser_connection_probe
```

公式拡張1.10.4.1が合成ハッシュを復号した。同じポートからの `associate`、`test-associate`、`get-logins` は、今回の合成レスポンダーでいずれも拒否した。製品に必要な登録先の所有権確認や既存登録との競合は、アプリ側の登録機能を接続してから別に検証する。管理ポリシーはこの自動試験でのストア導入と観測に使用した。

## GTKでの接続承認の設計とworkerの境界（2026-11-20）

1. アプリの明示操作でChrome用の登録を有効にする。Chromeの標準検索先とシステム側の同名ホストを確認し、既存登録と競合する場合は案内を出して止める。所有するマニフェストが想定内容から変更された場合は無効化時にも削除しない。listenerは利用者のruntimeディレクトリ内の専用ソケットを使い、ブラウザーの公開フレームは上限64 KiB、専用ホストとlistenerで同じUIDを検証する。公開ソケットからworkerの私有プロトコル `src/gnome/SessionProtocol.h` に直接接続できる構成を避ける。
2. 接続IDはアプリ側が新規接続ごとに払い出し、ブラウザーの `clientID` は信頼境界に使わない。workerの `src/gnome/BrowserSession.cpp` が上流の `BrowserMessageBuilder.cpp` で接続ごとの鍵交換と暗号化を行う。暗号化された `associate` を確認した後、GTKに返すのは一回限りの承認トークンだけ。承認待ちは全接続で1件、60秒で失効する。接続が閉じた場合、鍵が交換された場合、DBがロックされた場合もトークンは無効。照合できない `test-associate` と未対応の `get-logins` は拒否する。
3. GTKは解除済みで保存先のあるDBの表示名と、利用者が接続名を入力できる日英のダイアログを表示する。既定応答は「キャンセル」。許可の直前と結果の送出前に `src/main.cpp` の `Window::generation`、接続の生存、画面ロックの状態を再確認する。`Window::conceal()` で公開ソケット側の保留要求とダイアログも閉じる。既存ダイアログや保存処理で承認できない場合は拒否し、ブラウザーの要求だけでDBを解除しない。
4. `prototype/CoreSession.cpp` は上流KDBXのCustomDataにGNOME固有のプレフィックスで識別キーを格納する。DBの識別ハッシュもGNOME用に区別し、同じDBを使う本家ペアリングのキーリングを共有しない。同名を上書きせず、承認の成功応答は `src/gnome/CheckpointStore.h` による暗号化チェックポイントの確定後。通常の保存で元DBへ反映する。GTKは成功時に未保存表示を更新する。[公式拡張の登録・照合処理](https://raw.githubusercontent.com/keepassxreboot/keepassxc-browser/1.10.4.1/keepassxc-browser/background/keepass.js)。

workerと私有RPCの試験は `tests/browser_pairing_tests.cpp` に含まれる。

## 開発版での接続登録

1. Chrome Stable（deb）に公式KeePassXC-Browserを追加する。
2. KeePassXC GNOMEで保存済みDBを解除し、データベースメニューの「ブラウザー連携」から「Chrome との連携を有効にする」を選ぶ。
3. 拡張で接続を選び、KeePassXC GNOMEに戻る。対象DBを確認して接続名を入力し、「接続を許可」を選ぶ。DBを保存すると、ロック解除後やアプリ再起動後も拡張が接続を照合できる。
4. 連携を止める場合は同じメニューで無効にする。アンインストール前にもこの操作で利用者の登録ファイルを撤去できる。

`src/gnome/BrowserIntegration.cpp` がChrome標準の利用者用マニフェストとUnixソケットを管理する。既存のシステム・利用者用マニフェストを上書きせず、無効化は実行中のインスタンスが管理する同一内容のマニフェストだけを削除する。アプリが終了するとsocketが閉じる。パッケージは `packaging/Install.cmake` に従って補助実行ファイルを配置し、マニフェストは利用者の明示操作で作成する。[Chromeの登録仕様](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging#native-messaging-host-location)。

`src/gnome/browser_native_host.cpp` がWeb Store版の呼び出し元とsocketのUIDを検査し、各方向64 KiBまでのNative Messagingフレームを中継する。`src/main.cpp` がDBの状態・GTKの承認・応答の生存確認を担当する。アプリのロックやブラウザーの切断は接続と承認待ちを取り消す。

## 開発版でのログイン入力

1. 接続登録済みのChromeで、エントリーのURLと同じHTTPSオリジンのログインページを開き、公式拡張のユーザー名・パスワード入力を使う。
2. KeePassXC GNOMEの承認画面に表示された宛先を確認し、入力するエントリーの「入力を許可」ボタンを選ぶ。拡張の自動取得が有効な場合は、ページを開いた時点で承認画面が出る。
3. ブラウザーで入力内容を確認して、ログインフォームを送信する。アプリは拡張が受け取るエントリーに自動送信を抑止する指定を付ける。

`src/gnome/BrowserSession.cpp` は、同じソケットで照合した接続と `get-logins` 要求の識別キーを照合する。`prototype/BrowserAccess.cpp` はQtのURL解析を使い、ページURL、フォーム送信先、エントリー本体のURLでHTTPSのホストと実効ポートが一致する場合に候補を返す。期限切れ、ごみ箱、検索対象外、非表示、HTTP認証専用、拒否・対象外の許可リスト、本家の接続キーで制限されたエントリーは除外する。追加URLでの一致、認証領域、未知のブラウザー許可設定、資格情報にプレースホルダーが含まれるエントリーは対象外。KDBX内の設定は保持する。[本家の照合と制限処理](https://github.com/keepassxreboot/keepassxc/blob/2.8.0/src/browser/BrowserService.cpp)。

GTKにはオリジン・表示名・承認用の識別子を渡す。workerは承認時にも接続、DBと編集の世代、エントリーの適格性を照合してから、選ばれた1件のユーザー名とパスワードを暗号化して返す。公式拡張の手動入力が取消直後に同じ要求を再発行する場合は、同じ接続・ページ・フォームの要求を2秒間、画面を再表示せず拒否する。フォームに入力された値はそのページの管理になる。別オリジンiframeの判定は公式拡張の標準設定に依存する。Native Messagingの要求には最上位フレーム情報が含まれず、拡張で手動設定したiframe例外はアプリ側から判別できない。[公式拡張のサイト設定とiframe判定](https://github.com/keepassxreboot/keepassxc-browser/blob/1.10.4.1/keepassxc-browser/background/page.js)・[公式拡張の入力処理](https://github.com/keepassxreboot/keepassxc-browser/blob/1.10.4.1/keepassxc-browser/content/fill.js)。

隔離VMでの通し試験には、Web Store版を一時的に導入する上記のChromeポリシー、展開済みの開発用deb、`gnome_browser_pairing_tests` を使用する。英語に加えて日本語で試す場合は `KPXC_BROWSER_TEST_LANGUAGE=ja` を設定する。試験は一時証明書の公開鍵を指定したローカルHTTPSフォームを使い、AT-SPIでGTK画面を操作して公式拡張による入力を確認する。

```bash
bash tools/test-browser-ui.sh \
  /path/to/staged/usr/libexec/keepassxc-gnome/keepassxc-gnome \
  /path/to/gnome_browser_pairing_tests /path/to/google/chrome/chrome
```
