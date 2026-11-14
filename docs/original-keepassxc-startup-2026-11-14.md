# 本家Flatpak版の起動失敗の調査

2026-11-14。UI修正の作業中、利用者から本家KeePassXCが起動できないとの報告を受け、優先して読み取りで調査した。今回のUI変更は作業ツリーのソース差分だけで、ビルド・導入前だった。

## 確認した事実

- 本家はAPTの `keepassxc` ではなく、systemインストールのFlatpak `org.keepassxc.KeePassXC` 2.8.0。commitは `eddd9c1e1d5b574bfb550ac50c69d55a9ec219e6492e4ae2a5a4e00009329f0f`。以前のAPTパッケージ照会だけでは、この本家の稼働状態・Flatpak状態の不変を検証したことにならない。
- 通常起動のjournalには10:31〜10:32 JSTに `bwrap: Can't find source path /run/user/1000/doc/by-app/org.keepassxc.KeePassXC: No such file or directory`。KeePassXC本体の開始前の失敗。
- document portalは11月12日21:54からactiveのままで、D-BusのGetMountPointは `/run/user/1000/doc` を返したが、通常ホストのmountinfo / findmntには該当FUSEマウントがなく、by-appのパスも存在しなかった。
- 通常ログインのsystemdユーザー環境は `/run/user/1000`・通常バス・`wayland-0`・日本語LANGで、私有GUI試験のパスへの置換は確認されなかった。
- リポジトリの試験・製品コードにはdocument portalの再起動やfusermount / umount処理は見つからなかった。これだけで間接的影響が無いと証明したとは扱わない。

## 利用者承認後の対応

共有サービスへの影響を説明し、利用者がportal再起動を許可した後に `systemctl --user restart xdg-document-portal.service` だけを実行した。daemon-reload、他portalの再起動、本家の再インストール、設定変更、既存DBの操作は行っていない。

再起動後、サービスactive、`/run/user/1000/doc fuse.portal` のマウントを確認し、`flatpak run --system --command=keepassxc org.keepassxc.KeePassXC --version` が終了0で `KeePassXC 2.8.0` を返した。DBを開かない版照会であり、通常GUIの操作確認とは区別する。利用者が通常アイコンから本家のウィンドウが表示されたと回答し、GUI起動の復旧も確認した。DBの解除・内容確認は行っていない。

## 未確定事項

マウントが消失した時刻・原因、および本プロジェクトの過去の導入・私有GUI検証との因果関係は未特定。起動失敗の直接原因と復旧成功を、根本原因の特定や無関係の証明に読み替えない。今後の私有GUI検証では通常ログイン側のportalマウントが前後で維持されることも確認する。
