# qemu_custom_dev

QEMU 上の Linux ゲストに対し、独自仮想デバイス経由でホスト側機能
（簡易自作レンダラ）を接続するプロジェクト。デバイスドライバ開発と
ホスト・ゲスト間通信の仕組みの学習を兼ねる。

設計の経緯と全体計画は [docs/handoff-2026-07-24.md](docs/handoff-2026-07-24.md)、
プロトコル仕様は [docs/protocol.md](docs/protocol.md) を参照。

## 方針

- QEMU 本体は改変しない（無改変で成立する方式を優先）。
- 実在ハードウェアの模倣はしない。
- プロトコル定義と輸送層を分離し、Phase ごとに輸送層だけを差し替える。
- 依存は最小限（ビルドは C11 ツールチェーンと GNU make のみ）。

## 段階的計画

| Phase | 輸送層 | ゲスト側 | 状態 |
|-------|--------|----------|------|
| 1 | vsock（`vhost-vsock-pci`） | 標準ソケット API | **完了** |
| 2 | ivshmem-plain 共有メモリ（[docs/shm-transport.md](docs/shm-transport.md)） | BAR2 を mmap するユーザ空間ドライバ、ポーリング | **完了** |
| 3 | ivshmem-doorbell（[docs/doorbell-transport.md](docs/doorbell-transport.md)） | 自作カーネルモジュール、MSI-X 割り込み駆動 | **実装中（本リポジトリの現状）** |

Phase 2 と 3 はリングもメッセージも共通で、違いは通知方式（ポーリング /
割り込み）だけ。`guest/user/bench.c` で両者を実測比較できる。

## リポジトリ構成

```
proto/          プロトコル定義とコーデック（輸送層非依存）
host/           ホストデーモン renderd と ivshmem サーバ ivshmemd
guest/user/     ゲスト側クライアントライブラリとデモ（Phase 1-2）
guest/kmod/     ゲストカーネルモジュール（Phase 3）と、そのユーザ空間 API
scripts/        QEMU 起動スクリプト
containers/     カーネルモジュールビルド用コンテナ定義
tests/          単体テストと E2E テスト
docs/           仕様・設計資料
.tmp/           gitignore 対象の作業ディレクトリ
```

## ビルドとテスト

```console
$ make          # build/ に renderd, demo, test_proto, ppm_check を生成
$ make test     # プロトコル単体テスト + AF_UNIX / TCP 輸送での E2E テスト
```

`make test` は QEMU なしで完結する（AF_UNIX・TCP・共有メモリ・doorbell の
4 輸送で同一プロトコルを検証。輸送層非依存の確認を兼ねる）。さらに
`tests/vm-e2e.sh` は実際にゲストをブートし、ゲスト内からホストの renderd
への描画を無人で検証する（CI で毎 PR 実行。カーネルモジュール経由の
doorbell 経路と、ポーリングとのレイテンシ比較を含む。`/dev/vhost-vsock`
がある環境では vsock 輸送も検証。詳細は
[docs/guest-image.md](docs/guest-image.md)）。

## Phase 1 を実機（QEMU ゲスト）で動かす

ゲストは Ubuntu 24.04 cloud image + cloud-init で自動構築する
（決定事項と詳細手順は [docs/guest-image.md](docs/guest-image.md)）。

1. ゲストイメージを作成（ベースイメージは SHA256 固定で検証）:

   ```console
   $ scripts/make-guest-image.sh
   ```

2. ホストで vhost-vsock を有効化し、デーモンを起動:

   ```console
   $ sudo modprobe vhost_vsock
   $ build/renderd --vsock 5000 --out .tmp/frames
   ```

3. ゲストを起動（リポジトリは 9p で `/mnt/repo` に read-only 共有される）:

   ```console
   $ IMG=.tmp/guest/disk.qcow2 SEED=.tmp/guest/seed.iso scripts/run-qemu.sh
   ```

4. ゲスト内（シリアルコンソール、`dev`/`dev`）でビルドし、ホスト
   （CID 2）へ接続:

   ```console
   guest$ cp -r /mnt/repo ~/work && cd ~/work && make
   guest$ ./build/demo --vsock 2 5000
   ```

5. ホストの `.tmp/frames/frame-000001.ppm` に描画結果が出力される。

Phase 2（共有メモリ輸送、ポーリング）で動かす場合はホストで
`renderd --shm` を起動し、`IVSHMEM=` を付けてゲストを起動、ゲスト内で
`sudo ./build/demo --shm-pci` を実行する
（詳細は [docs/shm-transport.md](docs/shm-transport.md)）。

Phase 3（割り込み駆動）は `ivshmemd` と `renderd --ivshmem` を起動し、
`IVSHMEM_SOCKET=` でゲストを起動、ゲスト内でカーネルモジュールを
`insmod` してから `sudo ./build/demo --doorbell`
（詳細は [docs/doorbell-transport.md](docs/doorbell-transport.md)）。
モジュールのビルドは `scripts/build-kmod.sh`。

## 開発ルール

- コミットは [Conventional Commits](https://www.conventionalcommits.org/) に従う。
- `main` への merge は PR 経由で行う。
- 一時ファイルはリポジトリ内の `.tmp/`（gitignore 済み）に置く。
