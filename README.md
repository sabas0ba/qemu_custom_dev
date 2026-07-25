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
| 1 | vsock（`vhost-vsock-pci`） | 標準ソケット API | **実装中（本リポジトリの現状）** |
| 2 | ivshmem-plain 共有メモリ | `uio_pci_generic` + ユーザ空間ドライバ、ポーリング | 未着手 |
| 3 | ivshmem-doorbell | 自作カーネルモジュール、割り込み駆動 | 未着手 |

## リポジトリ構成

```
proto/          プロトコル定義とコーデック（輸送層非依存）
host/           ホストデーモン renderd（PPM 出力の簡易レンダラ）
guest/user/     ゲスト側クライアントライブラリとデモ（Phase 1-2）
guest/kmod/     ゲストカーネルモジュール（Phase 3、未着手）
scripts/        QEMU 起動スクリプト
containers/     カーネルモジュールビルド用コンテナ定義（Phase 3、未着手）
tests/          単体テストと E2E テスト
docs/           仕様・設計資料
.tmp/           gitignore 対象の作業ディレクトリ
```

## ビルドとテスト

```console
$ make          # build/ に renderd, demo, test_proto, ppm_check を生成
$ make test     # プロトコル単体テスト + AF_UNIX 輸送での E2E テスト
```

E2E テストは vsock の代わりに AF_UNIX ソケットを使うため QEMU なしで
完結する（CI でも実行される）。プロトコル層が輸送層に依存しないことの
検証も兼ねている。

## Phase 1 を実機（QEMU ゲスト）で動かす

1. ホストで vhost-vsock を有効化: `sudo modprobe vhost_vsock`
2. ホストでデーモンを起動:

   ```console
   $ build/renderd --vsock 5000 --out .tmp/frames
   ```

3. ゲストを起動（ゲストイメージは別途用意）:

   ```console
   $ IMG=/path/to/guest.qcow2 scripts/run-qemu.sh
   ```

4. ゲスト内で `guest/user/` をビルドし、ホスト（CID 2）へ接続:

   ```console
   guest$ ./demo --vsock 2 5000
   ```

5. ホストの `.tmp/frames/frame-000001.ppm` に描画結果が出力される。

## 開発ルール

- コミットは [Conventional Commits](https://www.conventionalcommits.org/) に従う。
- `main` への merge は PR 経由で行う。
- 一時ファイルはリポジトリ内の `.tmp/`（gitignore 済み）に置く。
