# 共有メモリ輸送層（Phase 2: ivshmem-plain）

Phase 1 のプロトコル（[protocol.md](protocol.md)）を、ivshmem-plain の
共有メモリ上のリングで運ぶ輸送層。メッセージのワイヤ形式（12 バイト
ヘッダ + ペイロード）は**ストリーム輸送と完全に同一**で、差し替わるのは
運び方だけ。通知はポーリング。Phase 3 では同じリングのまま通知だけを
ivshmem-doorbell の割り込みに差し替える（[doorbell-transport.md](doorbell-transport.md)）。

実装: `proto/rproto_shm.h` / `proto/rproto_shm.c`

## 全体構成

```
ホスト                                     ゲスト
renderd --shm FILE                        demo --shm-pci
  |  mmap(FILE)                             |  /sys/bus/pci/.../resource2 を mmap
  v                                         v
  +--------- 同一物理メモリ (ivshmem-plain BAR2) ---------+
  | rshm_hdr | g2h ring | h2g ring | (fb 予約領域)        |
  +------------------------------------------------------+
```

- ホスト: `renderd --shm FILE` がファイルを作成・初期化。QEMU には
  `-object memory-backend-file,share=on,mem-path=FILE`＋
  `-device ivshmem-plain` で渡す（`run-qemu.sh` の `IVSHMEM=` が設定）。
- ゲスト: PCI バスから ivshmem（vendor:device = 1af4:1110）を探し、
  sysfs の `resource2` を mmap する**ユーザ空間ドライバ**（root 必要、
  カーネルコード不要）。BAR2 全体が共有メモリ。

> 補足: 引き継ぎ資料では `uio_pci_generic` の bind を想定していたが、
> `uio_pci_generic` は BAR の mmap 領域を提供しない（INTx 割り込みの
> ハンドリングのみ）。割り込みを使わない Phase 2 では sysfs `resourceN`
> の mmap が最短・標準の方法のためこちらを採用した。UIO は割り込みが
> 必要になる Phase 3 で本題になる。

## レイアウト

すべて領域先頭からのオフセット。既定は 4 MiB（ivshmem の BAR サイズは
2 の冪である必要がある）。

| 領域 | 内容 |
|------|------|
| `0x0000` | `struct rshm_hdr`（magic, version, 各領域のオフセット・サイズ） |
| `g2h_off` | ゲスト→ホスト（要求）リング: `rshm_ring_hdr` + データ 256 KiB |
| `h2g_off` | ホスト→ゲスト（応答）リング: 同上 |
| `fb_off` | staging 領域。クライアントがピクセルを直接書き、`BLIT` でホストが取り込む（プロトコル v0.2。既定で約 3.5 MiB） |

初期化の公開順序: ホストは全フィールドを書いてから **release ストアで
magic を最後に書く**。ゲストは magic を acquire ロードでポーリングして
からレイアウトを信用する。

## リング（SPSC、virtio vring 参考）

各リングは単一生産者・単一消費者のバイトリング。

```
struct rshm_ring_hdr {
    u32 prod;   // 生産者のみ書く。free-running（mod しない）
    u8  pad[60];
    u32 cons;   // 消費者のみ書く。free-running
    u8  pad[60];
    // データ本体（2 の冪サイズ）が続く
};
```

- `prod`/`cons` は free-running な u32 カウンタ。実オフセットは
  `idx & (size-1)`。空き = `size - (prod - cons)`（u32 ラップアラウンド
  でも差分は正しい）。
- 別キャッシュラインに置き false sharing を回避。

### メモリバリア（必須）

ホストとゲストは同一物理メモリを見るが、コンパイラ・CPU のリオーダは
通常の SMP スレッド間と同様に起こる。

- **生産者**: データ本体を先に memcpy → `prod` を **release ストア**で
  更新。release により、データの書き込みがインデックス更新より先に
  可視化されることが保証される。
- **消費者**: `prod` を **acquire ロード** → その後にデータを読む。
- `cons` の更新・参照も対称に release / acquire（空き領域の再利用が
  データ読み出し完了後になることを保証）。

### メッセージの原子性

1 メッセージ（ヘッダ + ペイロード）はリングに**まるごと書いてから 1 回の
`prod` 更新で公開**する。したがって消費者が「ヘッダ 12 バイト以上ある」
と観測した時点で、そのメッセージの全体が読める。部分メッセージは
観測されない。

## セッション運用（v0）

- 1 接続 = 1 セッション。サーバはセッション終了後にリングをリセットする
  （その前に h2g が消費されるのを待つ。最後の STATUS がクライアントに
  届く前に消さないため）。
- クライアントの送受信タイムアウトは 10 秒（`RC_SHM_IO_TIMEOUT`）。
  ポーリング間隔は 50 µs。

## 動かし方

ローカル（VM なし、ファイル共有で 2 プロセス間）:

```console
$ build/renderd --shm .tmp/shm.bin --out .tmp/frames &
$ build/demo --shm-file .tmp/shm.bin
```

実ゲスト（PCI 経由）:

```console
$ build/renderd --shm .tmp/ivshmem.bin --out .tmp/frames &
$ IMG=... SEED=... IVSHMEM=.tmp/ivshmem.bin scripts/run-qemu.sh
guest$ sudo /mnt/repo/build/demo --shm-pci
```

どちらの経路も自動テストで検証される: `make test`（ファイル共有）、
`tests/vm-e2e.sh`（実ゲスト・PCI。ivshmem は vhost 不要のため CI・
コンテナでも動く）。

## staging 領域とゼロコピー（v0.2）

リングを流れるのはコマンドだけで、ピクセルは `fb_off` 以降の staging
領域に置いて `BLIT` で参照する。共有メモリを使う本来の理由がここにある
（コマンド 1 個は数十バイトだが、1 フレームは数百 KB〜数 MB）。
形式と検証規則は [protocol.md](protocol.md) の BLIT 節を参照。

`bench --blit` でフレーム毎のスループットを測れる。ホスト内 2 プロセス
（ファイル共有）では 640x480 RGBA でおおむね 3 GiB/s 前後 —
実質ホスト側の memcpy と R,G,B,A → 0xRRGGBBAA 変換のコストで、
リング経由のコマンドは 1 フレームあたり 1 通しか流れない。
