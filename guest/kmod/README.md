# guest/kmod

Phase 3（ivshmem-doorbell + 割り込み駆動）で実装するゲストカーネル
モジュールの置き場。未着手。

- doorbell は既定で MSI-X のため `uio_pci_generic`（INTx のみ）では
  受けられない。MSI-X 対応の自作 UIO ドライバか通常の PCI ドライバの
  どちらにするかは未決定（docs/handoff-2026-07-24.md §8）。
- ビルドはゲスト用カーネルヘッダを含むコンテナ（`containers/`）内で行い、
  ホスト環境を汚さない。
