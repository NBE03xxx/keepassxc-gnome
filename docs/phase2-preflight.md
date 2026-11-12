# Phase 2 — 基準ビルドの事前確認

確認日 2026-11-07。 [Phase 1 報告書の試作計画](feasibility-report.md#10-phase-2-の最小試作計画)に沿い、上流コアのビルドとテストを最初の作業とする。

> この文書は導入前の記録です。実行結果は [Phase 2 検証結果](phase2-results.md) を参照してください。

## 確認結果

- Ubuntu 26.04.1、GNOME Shell 50.1、Wayland セッション。C++ コンパイラー、Ninja、pkg-config、zlib と xkbcommon の開発ファイルは利用できる。GTK4 と libadwaita のランタイムは導入済み。
- 取得済みの公式タグ `2.8.0` を再確認した。コミットは `1c2f23ef5914c0f442a00533def1689857afb5a7` で、チェックアウトの作業ツリーはクリーン。 [固定ソース](https://github.com/keepassxreboot/keepassxc/tree/1c2f23ef5914c0f442a00533def1689857afb5a7)
- CMake と下表の開発パッケージは不足している。Ubuntu のローカル APT インデックスで候補があることを確認した。依存解決のシミュレーションでは、基準ビルド用の指定が推移依存込みで 36 パッケージ、GTK 試作分を含めると計 42 パッケージ。いずれも更新・削除は 0 件だった。実際のビルドで必要な範囲は構成時に確認する。
- 利用可能な KVM デバイスは確認できず、非特権ユーザー名前空間を作る試行も拒否された。最初のビルドは作業専用ディレクトリに Ubuntu の公開 deb を取得・展開し、そのパスをビルドと検証プロセスにだけ渡す方法を候補とする。この方式は開発ファイルの配置を分けるもので、仮想マシンによる隔離とは異なる。

## 導入の相談対象

| 段階 | パッケージ候補 | 用途と根拠 |
| --- | --- | --- |
| 基準ビルド | `cmake` | 上流の構成と CTest。 `CMakeLists.txt:17-23`、`CMakeLists.txt:598-605` |
| 基準ビルド | `qt6-base-dev`、`qt6-svg-dev`、`qt6-tools-dev` | Core、Concurrent、DBus、Test、Widgets、SVG、LinguistTools とコード生成。上流 CMake はコアだけをビルドする場合も GUI モジュールを検出する。 `CMakeLists.txt:444-461`、`src/CMakeLists.txt:346-364`、`share/translations/CMakeLists.txt:17-26` |
| 基準ビルド | `libbotan-3-dev`、`libminizip-dev`、`libpcsclite-dev`、`libusb-1.0-0-dev`、`libkeyutils-dev`、`libqrencode-dev` | 暗号、ZIP、ハードウェア鍵、Linux Quick Unlock、QR の既存ビルド依存。 `CMakeLists.txt:550-584`、`src/qrcode/CMakeLists.txt:16-24` |
| GTK 試作 | `libgtk-4-dev`、`libadwaita-1-dev` | GTK4 と libadwaita のヘッダーとリンク情報。上流コアの基準結果を得てから使用する |

導入方法は Ubuntu のパッケージを `/tmp` の作業専用ディレクトリに展開し、システムのパッケージデータベースを更新せず、試作用の CMake・ヘッダー・ライブラリ検索パスをプロセス単位で設定する。移設できないパッケージや生成ツールが見つかった場合は、必要な隔離環境を改めて判断する。AGENTS.md の依存追加ルールに従い、取得・展開は相談後に行う。

## 導入後の最初の検証

上流ソースを読み取り元として使い、生成物は別ディレクトリに出す。上流のインストール処理は使わず、設定・キャッシュ・状態用の XDG ディレクトリと `KPXC_CONFIG` / `KPXC_CONFIG_LOCAL` を検証プロセス専用にする。テストデータは上流の公開 fixture または合成したものを使用する。 `src/core/Config.cpp:559-655`

```bash
cmake -S "$kpxc_source" -B "$kpxc_build" -G Ninja \
  -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF -DKPXC_FEATURE_DOCS=OFF \
  -DWITH_X11=OFF -DWITH_TESTS=ON -DWITH_GUI_TESTS=OFF
cmake --build "$kpxc_build" --target keepassxc_core keepassxc-cli \
  testkdbx3 testkdbx4 testkeys testcryptohash testsymmetriccipher \
  testentrysearcher testpasswordgenerator testtotp testdatabase
ctest --test-dir "$kpxc_build" --output-on-failure \
  -R '^(testkdbx3|testkdbx4|testkeys|testcryptohash|testsymmetriccipher|testentrysearcher|testpasswordgenerator|testtotp|testdatabase)$'
```

上記は依存の展開と検索パスの設定後に実行する予定のコマンド。ターゲット名は `tests/CMakeLists.txt:63-85`、`tests/CMakeLists.txt:118-155` で確認済み。基準ビルドとテストは未実行。成功後は、コアを別ターゲットへリンクして合成 DB を解除・編集・Atomic 保存・再解除できるかを確認し、続いて GTK の最小画面に接続する。
