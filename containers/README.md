# containers

コンテナ定義の置き場。どちらも Ubuntu 24.04 をダイジェスト固定で使い、
ゲストイメージのカーネル（6.8.0-134-generic）に合わせてある。

| ファイル | 用途 | 実行する側 |
|----------|------|------------|
| `dev.Dockerfile` | 開発・実行環境一式（ツールチェーン + QEMU + ゲスト用カーネルヘッダ + ISO ツール） | `scripts/dev-container.sh` |
| `kmod-build.Dockerfile` | ゲストカーネルモジュールのビルドだけ | `scripts/build-kmod.sh` |

## `dev.Dockerfile`

ホスト環境を汚さずに、ビルドからゲストのブートまで全部やるための
イメージ。パッケージ一覧は `.github/workflows/ci.yml` が runner に
入れているものと同一にしてあり、**CI で `tests/vm-e2e.sh` が通っている
のと同じ環境**になる。どちらかを変えたらもう一方も合わせること。

```console
$ scripts/dev-container.sh                   # 対話シェル
$ scripts/dev-container.sh make test         # ビルドしてテスト
$ scripts/dev-container.sh tests/vm-e2e.sh   # ゲストをブートして全 Phase
```

- リポジトリだけを `/src` にマウントし、呼び出したユーザの uid/gid で
  実行する（`build/` や `.ko` が root 所有にならない）
- `/dev/kvm` と `/dev/vhost-vsock` は**あれば**渡す。無ければ QEMU は
  TCG にフォールバックし、vsock のテストはスキップされる（残りは動く）
- コンテナ内に docker は無いので、`KMOD_MODE=direct` を既定にしてある

CI の `dev-container` ジョブがこのイメージをビルドし、実際に
`make test` とモジュールビルドまで通している。

## `kmod-build.Dockerfile`

`scripts/build-kmod.sh` がカーネルモジュールだけをビルドするときに使う、
より小さいイメージ。ホストにカーネルヘッダを入れたくない場合の既定経路。
`dev.Dockerfile` の中で作業しているときは docker が入れ子にならないよう
`--direct`（`KMOD_MODE=direct`）が使われる。

## 更新するとき

カーネルバージョンは 3 か所に書いてある。ゲストイメージのスナップショット
を更新するときは同時に直すこと。

- `containers/dev.Dockerfile` の `ARG KVER`
- `containers/kmod-build.Dockerfile` の `ARG KVER`
- `scripts/build-kmod.sh` の `KVER`
