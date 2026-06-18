# Discutere — ドキュメントサイト (GitHub Pages)

`spec/` の設計資料と `src/` の実装をレビューした結果を、静的サイトとして公開するための成果物。

## ページ構成

| ページ | ファイル | 内容 |
|---|---|---|
| 概要 | `index.html` | サービスの役割 / 特徴 / 設計（3 軸対話・データの流れ・4 フロー・認証モデル・技術スタック） |
| ドメイン関連図 | `graph.html` | 主要ドメイン / 機能のリストと依存関係をインタラクティブグラフ（Cytoscape.js）で表示 |
| API | `api.html` | HTTP ルート全 71 件。トグル展開でパラメータ / ボディ / レスポンスを確認 |
| 仕様レビュー | `review.html` | `spec/` 全 32 ドキュメントと実装の対応・乖離レポート |

純粋な静的 HTML/CSS/JS。ビルド不要。`graph.html` のみ CDN（unpkg）から Cytoscape.js を読み込む。

## ローカル確認

```sh
cd site && python3 -m http.server 8080   # → http://localhost:8080
```

## 公開 (GitHub Pages)

`.github/workflows/pages.yml` が `site/` を Pages にデプロイする。
**初回のみ** リポジトリ設定で Pages のソースを有効化する必要がある:

> Settings → Pages → Build and deployment → **Source: GitHub Actions**

設定後、`site/**` への push（または Actions の手動実行 `workflow_dispatch`）で自動デプロイされる。
公開 URL は `https://<org>.github.io/<repo>/`。
