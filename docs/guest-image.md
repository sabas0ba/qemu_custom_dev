# ゲスト環境の準備

Phase 1（vsock）を実機の QEMU ゲストで動かすためのゲスト環境の決定事項と
手順。スクリプトは `scripts/make-guest-image.sh` と `scripts/run-qemu.sh`。

## 決定事項

| 項目 | 決定 | 理由 |
|------|------|------|
| ディストリビューション | Ubuntu 24.04 LTS (noble) cloud image | CI（ubuntu-24.04）と揃う。vsock / 9p / cloud-init が既定で使える。LTS でカーネルが安定 |
| カーネル | 24.04 GA カーネル（6.8 系） | vsock（AF_VSOCK）・9p は標準で有効 |
| イメージ作成方法 | 公式 cloud image + cloud-init（NoCloud seed ISO） | 手作業なしで再現可能。ベースイメージは SHA256 で固定 |
| ベースイメージの固定 | `releases/noble/release-20260705` の `ubuntu-24.04-server-cloudimg-amd64.img`、SHA256 をスクリプトに埋め込み | 開発方針「バージョンを SHA 等で固定」に準拠 |
| リポジトリのゲストへの共有 | 9p（virtfs、read-only、タグ `repo`） | QEMU 無改変・ネットワーク不要でソースを渡せる。ro なのでホスト側を汚さない |
| ログイン | シリアルコンソール、`dev` / `dev`（変更可）。SSH 公開鍵は任意 | ローカル実験専用。外部公開しない前提 |

ベースイメージの qcow2 は直接使わず **overlay（backing file）** を作るため、
ベースは常にクリーンに保たれる。壊れたら overlay を作り直すだけでよい。

## 手順

ホスト側の依存: `curl`, `sha256sum`, `qemu-img`,
`cloud-localds`（または genisoimage / xorriso / mkisofs）, `qemu-system-x86_64`。
Ubuntu なら: `sudo apt install qemu-system-x86 qemu-utils cloud-image-utils`

```console
$ scripts/make-guest-image.sh
$ sudo modprobe vhost_vsock
$ build/renderd --vsock 5000 --out .tmp/frames &
$ IMG=.tmp/guest/disk.qcow2 SEED=.tmp/guest/seed.iso scripts/run-qemu.sh
```

初回ブートは cloud-init が走り、`build-essential` のインストールが入る
（ユーザモード NAT でネットワーク接続が必要）。2 回目以降は `SEED=` を
省略してよい（cloud-init は instance-id が同じなら再実行しない）。

ゲスト内（シリアルコンソールに `dev` / `dev` でログイン）:

```console
guest$ ls /mnt/repo            # 9p でホストのリポジトリが見える（ro）
guest$ cp -r /mnt/repo ~/work && cd ~/work && make
guest$ ./build/demo --vsock 2 5000
```

ホスト側の `.tmp/frames/frame-000001.ppm` に描画結果が出力されれば成功。
終了は `sudo poweroff`（または QEMU モニタ `Ctrl-a x`）。

## ベースイメージの更新方法

1. https://cloud-images.ubuntu.com/releases/noble/ から新しい
   `release-YYYYMMDD` を選ぶ。
2. そのディレクトリの `SHA256SUMS` から
   `ubuntu-24.04-server-cloudimg-amd64.img` のハッシュを取る。
3. `scripts/make-guest-image.sh` の `SNAPSHOT` と `BASE_SHA256` を更新して
   コミットする（更新は意図的な変更としてレビューする）。

## 制約・注意

- `GUEST_PASS` は既定 `dev`。ローカルのシリアルコンソール実験専用であり、
  このゲストをネットワークに公開しないこと。SSH を使う場合は
  `SSH_PUBKEY` で公開鍵を渡す。
- 9p 共有は read-only。ゲスト内でのビルドは書き込み可能な場所へコピーして
  行う（上記手順）。
- KVM がないホストでは `ACCEL=tcg` で起動できるが遅い。
- Phase 3 のカーネルモジュールは、このゲストのカーネル（Ubuntu 24.04 GA、
  6.8 系）向けヘッダを `containers/` のビルド環境に固定する予定。
