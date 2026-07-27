# guest/kmod

ゲストカーネルモジュール 2 本。どちらもキャラクタデバイスを 1 つ生やし、
ゲストユーザ空間（`guest/user/`）がそれを叩く。

## `ivshmem_rproto`（Phase 3）

QEMU の ivshmem-doorbell デバイス（PCI 1af4:1110）を掴み、MSI-X 割り込みと
共有メモリをユーザ空間に見せる。

- `ivshmem_rproto.c` — ドライバ本体（PCI ドライバ + キャラクタデバイス）
- `ivshmem_rproto.h` — ユーザ空間との共有 ABI

設計判断（`uio_pci_generic` を使わない理由、UIO ではなく通常の PCI
ドライバにした理由、ユーザ空間 API）は
[docs/doorbell-transport.md](../../docs/doorbell-transport.md) を参照。

## `virtio_rproto`（Phase 4）

QEMU の汎用 `vhost-user-device-pci` が生やす virtio デバイス
（virtio ID 37）を掴み、その virtqueue に 1 往復ぶんの descriptor chain を
積む ioctl を 1 つ提供する。リング設計もアドレス変換も virtio_pci /
virtio_ring 任せなので、ドライバはデバイス固有の意味論だけになる。

- `virtio_rproto.c` — ドライバ本体（virtio ドライバ + キャラクタデバイス）
- `virtio_rproto.h` — ユーザ空間との共有 ABI

ID 37 を選んだ理由、ホスト側バックエンドとの対応は
[docs/vhost-user.md](../../docs/vhost-user.md) を参照。

## 共通

- `Makefile` — kbuild 用。単体で使うより `scripts/build-kmod.sh` 経由が想定
- `*_rproto.h` はゲストユーザ空間がそのまま include するので、ABI 定義が
  カーネル側とずれない

ビルド:

```console
$ scripts/build-kmod.sh              # docker があればコンテナ内
$ scripts/build-kmod.sh --direct     # 導入済みのゲスト用ヘッダで直接
```

ゲストカーネルは 6.8.0-134-generic に固定。ビルド成果物 `*.ko` は
gitignore 対象。

## ライセンス

このディレクトリのコードは **GPL-2.0-only** である。ソース先頭の
`SPDX-License-Identifier: GPL-2.0-only` がそれを示し、
`ivshmem_rproto.c` と `virtio_rproto.c` は `MODULE_LICENSE("GPL")` を
宣言している。

これは選択の余地がある事項ではない: Linux カーネルモジュールはカーネルの
内部 API を使うため GPL である必要があり、`MODULE_LICENSE` が GPL 系で
ないモジュールはカーネルから GPL 限定シンボルの使用を拒否され、
ロード時にカーネルを taint する。

`ivshmem_rproto.h` と `virtio_rproto.h` はゲストユーザ空間
（`guest/user/`）からも直接 include される ABI ヘッダで、こちらも同じく
GPL-2.0-only。この波及も
あってプロジェクト全体を GPL-2.0-only に揃えている
（ルートの [LICENSE](../../LICENSE) と
[README](../../README.md#ライセンス) を参照）。
