# vhost-user 輸送層（Phase 4）

Phase 1〜3 は「QEMU が用意した箱（vsock / ivshmem）の中に、自分たちで
設計したリングを置く」構成だった。Phase 4 はその自作リングをやめ、
**virtio の vring そのもの**を使う。

QEMU の汎用フロントエンド `vhost-user-device-pci` を使うと、ゲストには
普通の virtio デバイスが生え、そのキューの処理は unix ソケット越しに
別プロセス（= vhost-user バックエンド）へ丸投げされる。そのバックエンドが
`host/renderd --vhost-user`、ゲスト側のドライバが
`guest/kmod/virtio_rproto` である。**QEMU 本体は無改変**という方針は
そのまま守られる。

- プロトコル本体（`proto/`）は一切変更していない。輸送層の差し替えだけで
  Phase 1〜3 と同じ描画が動く、という設計の最終確認にあたる。

## 全体構成

```
  ゲスト                       QEMU（無改変）              ホスト
 ┌──────────────────┐        ┌────────────────────┐     ┌─────────────────┐
 │ demo --virtio    │        │ vhost-user-device- │     │ renderd         │
 │   ↓ ioctl        │        │ pci                │     │  --vhost-user   │
 │ virtio_rproto.ko │        │  (vhost-user       │◀───▶│  (vhost-user    │
 │   ↓ vring        │───────▶│   フロントエンド)  │ ①  │   バックエンド) │
 └──────────────────┘  ②    └────────────────────┘     └─────────────────┘
                                       ③                        │
                                                                ▼
                                                        .tmp/frames/*.ppm
```

- ① 制御プレーン: unix ソケット上の vhost-user プロトコル。機能ネゴシ
  エーション、ゲスト RAM の fd 受け渡し、vring アドレスの通知、
  kick/call eventfd の受け渡し。`host/vhost_user.c`。
- ② データプレーン: ゲストが vring に descriptor chain を積み、kick
  eventfd を叩く。QEMU は間に入らない（ioeventfd がバックエンドへ直結）。
- ③ 完了通知: バックエンドが used ring を更新し、call eventfd を叩くと
  QEMU がゲストへ割り込みを上げる。

1 リクエスト = 1 chain で、read-only descriptor にリクエスト、
write-only descriptor に応答が入る。プロトコルが厳密に
リクエスト/レスポンス型なので、この対応は素直に一致する。

## デバイス ID 37 について

QEMU の汎用フロントエンドは `virtio-id=` を必須とし、その値は QEMU 側の
`virtio_device_names[]` に名前が登録されているものでなければならない
（未登録の ID は assert で落ちる）。つまり「未使用の適当な番号」は選べない。

そこで、**登録済みだが Linux に in-tree ドライバが存在しない ID** から
37（`VIRTIO_ID_DMABUF`）を選んでいる。これは番号を借りているだけで、
本プロジェクトは virtio-dmabuf デバイスを実装していないし、その振りも
しない（「実在ハードウェアの模倣はしない」方針は維持している）。ゲスト側
ドライバも同じ 37 に bind するだけで、DMABUF のセマンティクスには一切
触れない。

`VHOST_USER_VIRTIO_ID` 環境変数で変更できるが、QEMU 側の制約と
`guest/kmod/virtio_rproto.c` の `vrp_id_table` を両方揃える必要がある。

## ハンドシェイクの実際の順序

`VU_DEBUG=1` を付けて `renderd --vhost-user` を起動すると、受信した
リクエストが全部出る。QEMU 8.2 での実際の流れ:

デバイス realize 時（ゲストがまだ動く前）:

```
GET_FEATURES(1) → GET_PROTOCOL_FEATURES(15) → SET_PROTOCOL_FEATURES(16)
→ SET_OWNER(3) → GET_FEATURES(1) → SET_VRING_CALL(13) → SET_VRING_ERR(14)
```

ゲストドライバが DRIVER_OK を立てた時（= `vhost_dev_start`）:

```
SET_FEATURES(2) → SET_MEM_TABLE(5) → SET_VRING_NUM(8) → SET_VRING_BASE(10)
→ SET_VRING_ADDR(9) → SET_VRING_KICK(12) → SET_VRING_CALL(13)
→ SET_VRING_ENABLE(18) → GET_FEATURES(1)
```

バックエンドが返す機能は最小限:

- `GET_FEATURES` = `VIRTIO_F_VERSION_1` | `VHOST_USER_F_PROTOCOL_FEATURES`。
  `INDIRECT_DESC` と `EVENT_IDX` を出さないので、vring の走査は素直な
  直接 descriptor だけを考えればよい。
- `GET_PROTOCOL_FEATURES` = 0。単一キューに必要なものは何もない。

## ゲスト RAM をどうやって読むか

バックエンドはゲストの物理メモリを直接読み書きする。そのために
`SET_MEM_TABLE` でリージョンごとに fd（`SCM_RIGHTS`）が渡ってきて、
それを `mmap` する。これが成立するには **ゲスト RAM が共有可能な
オブジェクトで backing されている**必要があり、`scripts/run-qemu.sh` は
vhost-user 使用時だけメモリ backing を差し替える:

```
-object memory-backend-memfd,id=vumem,size=$MEM,share=on
-machine memory-backend=vumem
```

これを忘れると QEMU 側は fd を渡せず、バックエンドはゲストメモリを
一切参照できない。

アドレスは 2 種類が混在するので使い分けが要る:

| 何のアドレスか | 種類 | 変換 |
|----------------|------|------|
| vring 本体（desc / avail / used）| QEMU のユーザ空間アドレス | `from_qva()` |
| descriptor の `addr` | ゲスト物理アドレス | `from_gpa()` |

`SET_MEM_TABLE` の各リージョンが両方（`userspace_addr` と
`guest_phys_addr`）を持っているので、同じテーブルを 2 通りに引くだけで
済む。

## ゲストドライバ

`guest/kmod/virtio_rproto.c`。virtio_pci が PCI 探索・MSI-X・vring 確保を
すべて済ませてくれるので、ドライバ本体はデバイス固有の意味論だけになる。

- `virtio_find_single_vq()` でキューを 1 本取る。
- `/dev/virtio-rproto`（miscdevice）に ioctl `VIRTIO_RPROTO_IOC_XFER` を
  1 つだけ生やす。リクエストとレスポンスのバッファを受け取り、
  `virtqueue_add_sgs(vq, sgs, 1 /*out*/, 1 /*in*/, ...)` で chain を積み、
  kick して完了を待ち、`virtqueue_get_buf()` の長さを応答長として返す。
- バッファは DMA API に渡るので `kmalloc` 系（`devm_kmalloc`）で確保する。
  `vmalloc` では駄目。

Phase 3 のドライバ（`ivshmem_rproto`）と違い、共有メモリ窓を mmap で
ユーザ空間へ見せていない。したがって **BLIT（staging 領域からのゼロコピー）
はこの輸送では使えない**（`rc_staging()` が NULL を返し、`rc_blit()` は
拒否する）。ピクセルも含めて全部が descriptor chain を通る。

### 応答が返らなかったとき

タイムアウト（30 秒）やシグナルで待ちを抜けても、バッファの所有権は
まだデバイス側にある。再利用すると壊れるので、その場合はデバイスを
broken 状態にして以後の転送を `-EIO` で拒否する。中途半端に続行しない。

## 動かし方

```console
$ make
$ scripts/build-kmod.sh                       # virtio_rproto.ko を作る
$ scripts/make-guest-image.sh
```

ホスト側でバックエンドを起動（**ゲストより先に**。QEMU はクライアント
として接続しに行くので、ソケットが無いと起動に失敗する）:

```console
$ build/renderd --vhost-user .tmp/vu.sock --out .tmp/frames
```

ゲストを起動:

```console
$ IMG=.tmp/guest/disk.qcow2 SEED=.tmp/guest/seed.iso \
    VHOST_USER=.tmp/vu.sock scripts/run-qemu.sh
```

ゲスト内:

```console
guest$ sudo insmod /mnt/repo/guest/kmod/virtio_rproto.ko
guest$ sudo /mnt/repo/build/demo-static --virtio
```

`.tmp/frames/frame-000001.ppm` が出れば成功。`tests/vm-e2e.sh` の 3 番目の
ブートがこの手順をそのまま無人で実行している。

## つまずきどころ（実際に踏んだもの）

**`-chardev ...,reconnect=1` を付けると `Failed to set msg fds.`**
`reconnect` を指定すると QEMU の socket chardev は**非同期**接続になる。
デバイス realize の時点でまだ接続が完了しておらず、`SCM_RIGHTS` を伴う
最初のリクエストで `vhost_backend_init failed: Protocol error` になる。
vhost-user では reconnect を付けない（バックエンドを先に起動する）。

**`SET_VRING_ENABLE` に応答しないと QEMU が止まる**
QEMU はこのリクエストを「バックエンドの ack を待ってから」次へ進む。
`VHOST_USER_PROTOCOL_F_REPLY_ACK` を出していないバックエンドに対しては、
代わりに直後へ `GET_FEATURES` を送って同期を取ってくる。いずれにせよ、
`NEED_REPLY` フラグが立っているリクエストには必ず u64 を返す必要がある
（`handle_message()` の末尾で一括して処理している）。

**キューサイズは 4 で固定**
QEMU 8.2 の `vhost-user-device` は `virtio_add_queue(vdev, 4, ...)` と
ハードコードしている。1 リクエストあたり descriptor 2 本なので同時に
2 リクエストしか積めないが、本プロトコルは同期的な 1 往復なので支障は
ない（ドライバも mutex で 1 本に直列化している）。

**接続断は「切断」ではないことがある**
バックエンドのログに `frontend disconnected` が出た場合、QEMU が
ハンドシェイク途中で諦めたのか、単に QEMU プロセスが終了しただけなのかは
ログの時刻を見ないと区別できない。切り分けにはタイムスタンプを付けて
QEMU 側のコンソールログと突き合わせるのが早い。

## 制約・注意

- キューは 1 本、同時実行は 1 リクエスト。並列化やバッチングはしていない。
- `VIRTIO_F_ACCESS_PLATFORM`（vIOMMU）は非対応。descriptor のアドレスは
  ゲスト物理アドレスとしてそのまま解釈する。
- マイグレーション非対応（ダーティログを実装していない。QEMU 側も
  `VHOST_USER_PROTOCOL_F_LOG_SHMFD` が無いことを理由に blocker を立てる）。
- バックエンドの再接続には対応していない。1 セッション = 1 接続。
- `vhost-user-device` は QEMU 側でも開発・実験用途の汎用スタブという
  位置づけである。本番のデバイスを作るなら、専用のフロントエンドを
  持つのが本来の姿。ここでは「QEMU 無改変で virtio デバイスを生やす」
  という目的にちょうど合うので使っている。
