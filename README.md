# KeePassXC GNOME Native Edition

Ubuntu 26.04 / GNOME 50 / Wayland 向けの GTK4 + libadwaita アプリの開発版。暗号、KDBX、検索、パスワード生成には KeePassXC の既存コアを使用する。

現在は合成データによる開発・検証用。終端検証と[KDF・読み込み量の暫定上限](docs/read-resource-policy.md)に加え、[XML・モデル件数の上限と保持可否の検査](docs/xml-model-preservation.md)を追加した。保持できない未知データは解除を拒否する。プロセス全体の資源上限、保存の耐障害性、画面ロックとスリープ、クリップボードの失効には製品化の合格条件が残る。実際のパスワード DB を使う段階に達していない。実装と検証の状況は [Phase 3 の記録](docs/phase3-results.md) に記載する。任意の未知構造の再保存は [検討記録のみ](docs/compatibility-policy.md) とし、実装対象・完成条件には含めない。

## ビルド

公式タグ 2.8.0 のコミット `1c2f23ef5914c0f442a00533def1689857afb5a7` を使う。ソース取得後は CMake がコミットを照合する。
アプリには [終端検証のパッチ](patches/README.md) をビルド用コピーへ適用する。相互運用試験には、別ビルドの無変更の上流 CLI を使う。

```bash
bash tools/fetch-upstream.sh
python3 tools/bootstrap-dev.py
source tools/dev-env.sh
cmake -S . -B build -G Ninja
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build
ctest --test-dir build --output-on-failure
bash tools/run-development.sh
```

`bootstrap-dev.py` は APT が解決した開発依存の deb をダウンロードし、SHA-256 を照合して `.cache/development/sysroot` へ展開する。取得したパッケージとハッシュは `.cache/development/packages.json` に記録する。この手順は Ubuntu 26.04 の既存のコンパイラー、Ninja、Git、pkg-config、gettext を使う。

日本語と英語は環境のロケールから選択する。日本語のカタログが選ばれない環境では英語を表示する。明暗は libadwaita のシステム設定追従を使う。

## GUI の検証

```bash
bash tools/test-ui.sh
```

GNOME Shell と AT-SPI のある環境で、独立した D-Bus、設定、Wayland セッションを作って日本語 UI を操作する。結果は `build/ui-test/` に記録する。実インストールと deb の確認には、先に Ubuntu 26.04 の VM を使う。

## 構成

- `src/main.cpp` に GTK4 / libadwaita の開発用 UI と Qt ワーカーを置く。
- `prototype/CoreSession.*` は既存試作から継承した KeePassXC コアの境界。
- `cmake/` で上流のターゲットに独立したフロントエンドを接続する。
- `upstream/keepassxc/` は固定した上流の参照用チェックアウト。
- `docs/` に調査、実装、検証と未完了事項を記録する。

## ライセンス

本プロジェクトのソースコードは GNU GPL version 2 または version 3 の条件で提供する。
本文は [GPL-2](LICENSE.GPL-2) と [GPL-3](LICENSE.GPL-3) を参照。
再利用する KeePassXC と依存ライブラリの著作権表示・ライセンスは各ソースに従う。
