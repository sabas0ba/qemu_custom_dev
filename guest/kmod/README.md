# guest/kmod

ゲストカーネルモジュール `ivshmem_rproto`（Phase 3）。QEMU の
ivshmem-doorbell デバイス（PCI 1af4:1110）を掴み、MSI-X 割り込みと共有
メモリをユーザ空間に見せる。

- `ivshmem_rproto.c` — ドライバ本体（PCI ドライバ + キャラクタデバイス）
- `ivshmem_rproto.h` — ユーザ空間との共有 ABI。ゲストユーザ空間
  （`guest/user/`）はこのヘッダを直接 include するので定義がずれない
- `Makefile` — kbuild 用。単体で使うより `scripts/build-kmod.sh` 経由が想定

設計判断（`uio_pci_generic` を使わない理由、UIO ではなく通常の PCI
ドライバにした理由、ユーザ空間 API）は
[docs/doorbell-transport.md](../../docs/doorbell-transport.md) を参照。

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
`ivshmem_rproto.c` は `MODULE_LICENSE("GPL")` を宣言している。

これは選択の余地がある事項ではない: Linux カーネルモジュールはカーネルの
内部 API を使うため GPL である必要があり、`MODULE_LICENSE` が GPL 系で
ないモジュールはカーネルから GPL 限定シンボルの使用を拒否され、
ロード時にカーネルを taint する。

`ivshmem_rproto.h` はゲストユーザ空間（`guest/user/`）からも直接
include される ABI ヘッダで、こちらも同じく GPL-2.0-only。この波及も
あってプロジェクト全体を GPL-2.0-only に揃えている
（ルートの [LICENSE](../../LICENSE) と
[README](../../README.md#ライセンス) を参照）。
