# 割り込み駆動輸送層（Phase 3: ivshmem-doorbell）

Phase 2 の共有メモリリング（[shm-transport.md](shm-transport.md)）はそのまま
使い、**通知だけをポーリングから割り込みに差し替える**のが Phase 3。
リングのレイアウトもメッセージのワイヤ形式も無変更で、変わるのは
「相手にメッセージが来たことをどう伝えるか」だけ。

実装:
- ホスト: `host/ivshmemd.c`（ivshmem サーバ）、`host/ivshmem.c`（クライアント）、
  `renderd --ivshmem SOCKET`
- ゲスト: `guest/kmod/ivshmem_rproto.c`（カーネルモジュール）、
  `guest/user/render_client.c` の `--doorbell` 輸送
- 通知フック: `proto/rproto_shm.h` の `struct rshm_notifier`

## 全体構成

```
        ┌──────────── ivshmemd（自作 ivshmem サーバ）────────────┐
        │  共有メモリ fd と peer ごとの eventfd を仲介           │
        └───────┬───────────────────────────────┬───────────────┘
        unix socket                        unix socket (chardev)
                │                               │
        ┌───────┴────────┐              ┌───────┴──────────────┐
        │ renderd        │              │ QEMU                 │
        │ --ivshmem      │              │ -device              │
        │ (peer 0)       │              │  ivshmem-doorbell    │
        └───────┬────────┘              └───────┬──────────────┘
                │  同一物理メモリ（BAR2）        │  PCI 1af4:1110
                └───────────────┬───────────────┘
                                │
                    ┌───────────┴──────────────┐
                    │ ゲスト                    │
                    │ ivshmem_rproto.ko        │
                    │  → /dev/ivshmem-rproto   │
                    │ demo/bench --doorbell    │
                    └──────────────────────────┘
```

## ivshmem サーバを自作した理由

引き継ぎ資料では QEMU 付属の `ivshmem-server` を使う想定だったが、これは
QEMU の `contrib/` にあるツールで Ubuntu はパッケージ化していない。
QEMU をソースからビルドする依存を増やすより、プロトコル
（QEMU `docs/specs/ivshmem-spec.rst`）が小さいので自前で実装した。
**QEMU 本体は無改変**という方針は維持されている。

プロトコルは 1 メッセージ = 64bit LE 整数 1 個（+ SCM_RIGHTS で fd 1 個）:

1. プロトコルバージョン（0）
2. 接続してきたクライアント自身の peer ID
3. `-1` + 共有メモリ fd
4. 既存 peer ごとに、その ID + eventfd（ベクタ数だけ繰り返す）。
   その eventfd に書くと当該 peer に割り込みが上がる
5. 最後に自分自身の ID + 自分が待つべき eventfd。**自分の分が最後に来る**
   ことがハンドシェイク終了の合図

以降も接続は維持され、peer 参加時は同じ形式、離脱時は fd なしの ID が届く。

## ゲストドライバの形態（未決定事項 §8 の決定）

**通常の PCI ドライバ + キャラクタデバイス**を採用した。`uio_pci_generic`
はそもそも INTx 専用で doorbell（MSI-X）を受けられないため対象外。
残る選択肢は「MSI-X 対応の自作 UIO ドライバ」と「通常の PCI ドライバ」
だったが、後者を選んだ理由:

- UIO でも MSI-X 自体は扱えるが、ユーザ空間から Doorbell レジスタを叩く
  には BAR0 の mmap が必要になる。BAR0 は **256 バイト**で、ページ境界に
  配置される保証がない。UIO の mmap はページ境界を要求するため、環境に
  よっては動かない構成になる。
- レジスタ書き込みをカーネル側（ioctl）に置けばこの問題が消え、
  さらに `poll()` が自然に使える。

`/dev/ivshmem-rproto` のインタフェース（`guest/kmod/ivshmem_rproto.h`。
ゲストユーザ空間はこのヘッダを直接 include するので定義が乖離しない）:

| 操作 | 内容 |
|------|------|
| `mmap(offset=0)` | BAR2（共有メモリ）全体 |
| `read()` | 次の割り込みまでブロックし、`__u32` のイベント数を返す（UIO 互換の考え方）|
| `poll()` | 未読の割り込みがあれば `POLLIN` |
| `ioctl(IVSHM_IOC_RING, &u32)` | Doorbell レジスタに `(peer<<16)｜vector` を書く |
| `ioctl(IVSHM_IOC_GET_ID, &u32)` | IVPosition（自分の peer ID）|
| `ioctl(IVSHM_IOC_SHM_SIZE, &u64)` | 共有領域のサイズ |

ドライバ本体は MSI-X ベクタを 1 本だけ確保し（`pci_alloc_irq_vectors`
に `PCI_IRQ_MSIX`）、ハンドラはイベント数を増やして待ち行列を起こすだけ。
MSI-X ではデバイス側の ack（Interrupt Status 読み出し）は不要。

## 相手の peer ID をどう知るか

ゲストは自分の ID（IVPosition）しか読めず、ホストの peer ID はハード
ウェアからは分からない。そこで **renderd が共有メモリヘッダの
`host_peer_id` に自分の ID を書いて公開する**（`rshm_hdr`、レイアウトと
一緒に magic の前に書き込む）。ゲストはそれを読んで
`(host_peer_id << 16) | 0` を Doorbell に書く。接続順に依存しない。

この追加で共有メモリのレイアウト版数は 2 に上がっている。

## 通知フックとロストウェイクアップ

`struct rshm_notifier` が `notify` / `wait` の 2 関数を持ち、
未設定（NULL）なら Phase 2 と同じポーリングになる。

- 送信側: **リングに publish してから** `notify`
- 受信側: **必ず先にリングを見てから** `wait`

この順序により取りこぼしは起きない。加えて、待ち側の下地（ホストは
eventfd、ゲストはドライバのイベントカウンタ）はどちらも「待つ前に来た
通知」をラッチするため、`wait` に入る前に通知が来ても待ち続けることは
ない。リングの空き待ちだけはポーリングのまま（空きは相手の消費で
生まれるもので、要求応答型のこのプロトコルでは詰まらない）。

## 動かし方

ホスト側:

```console
$ build/ivshmemd --socket .tmp/ivshmem.sock --shm .tmp/doorbell.bin \
      --size 4194304 --vectors 1 &
$ build/renderd --ivshmem .tmp/ivshmem.sock --out .tmp/frames &
$ IMG=... SEED=... IVSHMEM_SOCKET=.tmp/ivshmem.sock scripts/run-qemu.sh
```

ゲスト側（root）:

```console
guest# insmod /mnt/repo/guest/kmod/ivshmem_rproto.ko
guest# /mnt/repo/build/demo --doorbell
guest# /mnt/repo/build/bench --doorbell --iters 300
```

カーネルモジュールのビルドは `scripts/build-kmod.sh`。docker があれば
`containers/kmod-build.Dockerfile` のコンテナ内で（ホスト環境を汚さない）、
無ければ導入済みのゲスト用カーネルヘッダで直接ビルドする（使い捨て環境・
CI 向け）。ゲストカーネルは 6.8.0-134-generic 固定で、
`scripts/make-guest-image.sh` の pin と揃える必要がある。

## ポーリングとの比較

`guest/user/bench.c` が 1x1 サーフェスへの空の FILL_RECT を往復させ、
レイテンシ分布を出す。`tests/vm-e2e.sh` が実ゲストで Phase 2（`--shm-pci`）
と Phase 3（`--doorbell`）の両方を測り、結果を並べて表示する。

測定値の読み方に関する注意:

- **ポーリング側は待ち方のパラメータに強く依存する**。この実装は
  50 µs 間隔の `nanosleep` でリングを見に行くため、片道あたり平均 25 µs
  程度の待ちが構造的に入る。スピンで回せばレイテンシは下がるが、その分
  CPU を焼く（本来のポーリングのトレードオフ）。
- **割り込み側は VM exit と MSI-X 配送のコストを払う**。KVM か TCG かで
  桁が変わるため、比較は必ず同じ accel・同じホストで取った値どうしで行う。
- したがって「どちらが速いか」は一意に決まらない。この比較の目的は、
  同じリング・同じプロトコルの上で**通知方式だけを差し替えたときに何が
  変わるか**を実測で押さえること。

## つまずきどころ（実際に踏んだもの）

- **`pci_set_master()` を忘れると割り込みが一切来ない。** MSI-X は
  「デバイスが発行するメモリ書き込み」なので、bus master が有効でないと
  配送されない。QEMU 側は `msix_notify()` まで進むが、書き込み先の
  address space が無効なので黙って捨てられる。症状は「doorbell 経路は
  確立するのに 1 往復が待ちタイムアウト時間ちょうどかかる」。
  `pci_enable_device()` は bus master を有効にしないので、ドライバが
  明示的に呼ぶ必要がある。
- **カーネルの `dev_info()` はシリアルコンソールに出ないことがある。**
  cloud image は `quiet` 付きで起動するため、テストの成否判定を
  `dmesg` 由来の文字列に頼らない（ユーザ空間の出力で判定する）。
- **cloud-init の `runcmd` の出力は既定でゲスト内のログにしか残らない。**
  ホスト側から結果を読むには明示的に `/dev/console` へ流す
  （`scripts/make-guest-image.sh` がそうしている）。

## 制約・注意

- ベクタは 1 本のみ（サーバの `--vectors` と QEMU の `vectors=` は一致
  させること）。
- ivshmem-plain と ivshmem-doorbell は同じ PCI ID（1af4:1110）なので、
  1 つのゲストに同時に付けるとゲスト側が区別できない。`run-qemu.sh` は
  `IVSHMEM` と `IVSHMEM_SOCKET` の同時指定をエラーにする。
  `tests/vm-e2e.sh` が 2 回に分けてブートしているのはこのため。
- モジュールは署名していない。Secure Boot 有効の環境では読み込めない
  （このプロジェクトのゲストは Secure Boot なしで起動する）。
- ゲスト側は root 必要（`/dev/ivshmem-rproto` と `insmod`）。
