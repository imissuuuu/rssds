# 3DS RSS Reader - AI Agent Context Handoff
> Last updated: 2026-04-10

## Project Overview
Nintendo 3DS向けのRSSリーダー homebrew アプリ。C++ / libctru / citro2d を使用。
**動機**: 3DSブラウザがTLS非対応で使えなくなったため、libcurl+mbedtlsでHTTPSを解決しウェブ記事を読む手段として開発。RSSは記事一覧の管理機構として位置づける。

## Build

ツールチェーン: devkitPro (`C:\devkitPro\`)、MSYS2 bash経由で実行。

```bash
# ビルド
/c/devkitPro/msys2/usr/bin/bash -l /c/projects/rssds/build.sh

# クリーンビルド（ヘッダのstruct変更後は必須）
/c/devkitPro/msys2/usr/bin/bash -l /c/projects/rssds/build.sh clean
```

出力: `rssds.3dsx`, `rssds.smdh`

SDカード準備:
- `sdmc:/3ds/rssreader/cacert.pem` — Mozilla cacert (https://curl.se/docs/caextract.html)
- `sdmc:/3ds/rssreader/feeds.txt` — 1行1URL（省略時はHacker News / Ars Technicaを使用）

## Current State
MVP実装・実機テスト完了。以下が動作確認済み：
- HTTPS通信（libcurl + mbedtls）
- RSS 2.0 / Atom / RSS 1.0（RDF）パース
- 日本語表示（UTF-8文字境界での折り返し）
- 本文フェッチ（記事URLのHTML取得 → script/style除去 → テキスト化）
- citro2d UI（フィード一覧 → 記事一覧 → 記事本文、スクロール）

## Next Steps
- コードレビュー・コミット（現セッション分）
- 要件定義の本格化（MVPで判明した制約をベースに）
  - 本文抽出精度の向上（`<article>`/`<main>`以外のパターン対応）
  - HN型（本文なし）フィードへの表示改善
  - フィードURL管理のUX改善

---
> 過去の開発履歴: `HANDOFF/history.md`
> アーキテクチャ詳細: `HANDOFF/architecture.md`
