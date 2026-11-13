# Vulkan 単独対照の128 bytes

2026-11-13。長時間検証に付随する追加調査。アプリ本体やドライバーの変更・インストールは行わない。

## 確認した事実

`tests/vulkan_memory_control.cpp` はGTK・Qt・KeePassXCを使わず、Vulkan instance / deviceを作り、全ハンドルを破棄する。以前の終了時LSanは128 bytes / 1件を報告し、module unload後のためcallerを特定できていなかった。

今回、instance破棄前後の `/proc/self/maps` を対照プロセス自身から保存した。実体は `/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so`。私有prefixの同名ファイルもこの実体へ解決され、SHA256とBuild IDは既存の `tests/font-offset-audit.json` と一致する。新しいドライバーを導入したわけではない。

対応するUbuntuデバッグ情報で解決したcallerは以下。Build IDは `fa4e884aa7a23eaf80dc04ea27eb5da408865051`。

| ELF内オフセット | 関数 |
|---|---|
| `0x3404f2` | `get_cpu_topology` |
| `0x3409d6` | `_util_cpu_detect_once` |
| `0x33e93c` | `util_queue_thread_func` |
| `0x37475b` | `impl_thrd_routine` |

公式の[Ubuntu Mesa 26.0.8ソース](https://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_26.0.8.orig.tar.xz)は[対応dsc](https://archive.ubuntu.com/ubuntu/pool/main/m/mesa/mesa_26.0.8-1ubuntu0.3.dsc)のSHA256と照合済み。必要なソースだけを `.cache/gui-memory-sources/mesa-26.0.8/` に展開した。

- `src/util/u_cpu_detect.c:100–107`：moduleのstaticなCPU情報とonce状態。
- `src/util/u_cpu_detect.c:563–579`：L3キャッシュごとのCPU affinity表をreallocし、static情報に保持。
- `src/util/u_cpu_detect.c:932–935`：結果をglobal状態へ移し、初期化完了を公開。
- `src/util/u_cpu_detect.h:168–171`：同じmodule内ではcall_onceにより再初期化を防ぐ。

## 対照結果

`tools/test-vulkan-lifetime.sh` は固定したドライバーのハッシュを確認し、次を同じプロセスで実行する。各シナリオ自体は別のプロセスで、他シナリオの状態を引き継がない。

| シナリオ | 生LSan | 終了コード |
|---|---:|---:|
| 作成・破棄1回、loaderによるmodule unloadを許可 |128 bytes / 1件 |1 |
| 同10回 |1,280 bytes / 10件 |1 |
| 診断用dlopen参照でmoduleを保持し、同10回 |0件 |0 |

module保持の対照は意図的に追加のdlopen参照を終了まで保持する。これは寿命の違いを確認する操作であり、unload時の解放不足を修正したことにはしない。製品に同じ操作を追加しない。library全体のLSan suppressionやignore APIも使わない。

生ログ・アンロード前後のmap・終了コードとcaller対応は `build/vulkan-lifetime/{unload-1,unload-10,hold-10}/`、集計は `build/vulkan-lifetime/result.json`。対応するデバッグ情報は `.cache/gui-memory-symbols/radeon.debug`。一部DWARFの補助型情報は未取得だが、関数対応と該当命令は確認した。

## 判定とアプリへの影響

**確認済み：この128 bytesはMesaのCPU構成表で、module unload後に報告が残る。1回のmodule常駐中に10回deviceを作成・破棄する対照では増加・未解放報告はない。** アンロードを繰り返す使用法では実際に128 bytesずつ残るため、単に「LSanの誤検出」とは扱わない。

**技術的推論：** module内のonce初期化とglobal参照が続くGTKの描画経路では、同じCPU表を利用するため、エントリー操作の回数に比例してこの128 bytesが増える経路ではないと考えられる。プロセス終了時のOSによる回収と、moduleだけのunload時の解放不足は区別する。

**アプリの実測確認：** 同一GUIの反復3,601秒・718巡とロック待機900秒を完了し、各測定時のMesa mappingは存在した。GUI終了時の所有関係照合は未説明0 bytesで、この128 bytesを含む再報告もなかった。したがって今回のGUIでは反復回数に比例する同種の残留を検出しなかった。拡縮フォントのキャッシュ増加は別に検出しており、全体のメモリ安定性合格とはしない。詳しくは [長時間試験](gui-soak-results.md)。サンプリング間の瞬間的なmodule unload/reloadを完全監視する試験ではないため、mappingの存在だけで一度もunloadされなかったと断定しない。

この外部依存の解放不足を修正するために、GTKやMesaの寿命管理へ独自パッチを加えない。別のドライバー・Mesa更新では同じ結果を仮定せず、対照を再実行する。

```bash
bash tools/test-vulkan-lifetime.sh
```

私有プロセスでLSanの動作が可能な環境を使う。追加パッケージは不要。
