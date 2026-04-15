# テスト用サンプル HTML

Phase 6.5 Readability 移植の回帰検証用サンプル。

| ファイル | 取得元 | 取得日 | 備考 |
|---|---|---|---|
| `itmedia_sample.html` | https://www.itmedia.co.jp/news/articles/2604/15/news113.html | 2026-04-15 | ノイズ除去確認用 |
| `gigazine_sample.html` | https://gigazine.net/news/20260415-google-app-for-desktop/ | 2026-04-15 | `<div id="article">` 構造確認用 |
| `qiita_sample.html` | https://qiita.com/masa20057/items/44b4b16a441f494a2307 | 2026-04-15 | `<main>` タグ・タグリスト確認用 |

## 検証方法
3DS 実機で各記事を開いて `extract_log.txt` を確認。
- winner タグが正しいか
- len が妥当か（ノイズ込みの旧値と比較）
- FALLBACK が出ていないか
