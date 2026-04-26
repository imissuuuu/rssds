---
id: RDD-phase1
title: フィード管理UI
status: approved
created: 2026-04-26
---

# RDD: フィード管理UI（Phase 1）

## 目的

アプリ内でフィードの追加・削除・並び替えを行えるようにする。  
現状は `feeds.json` を手動編集する必要があり、アプリ内で完結しない。

## スコープ外

- QRコードによるURL取得（別フェーズ）
- フィード名のRename
- fetch_full_text のトグル

---

## 機能要件

### FR-1: FeedList 項目順の変更

現状の並びを以下に変更する。

```
[Feed 1]
[Feed 2]
...（フィード群）
★ Bookmarks
[Add Feed]
[Settings]
```

### FR-2: フィード追加（Add Feed）

- `[Add Feed]` 行で A を押すと Swkbd（3DS ソフトウェアキーボード）が開く
- URL を入力して確定するとフィード末尾に追加
- 空入力またはキャンセル時は何もしない
- バリデーション: `http://` または `https://` で始まるか確認、不正なら却下（エラー表示）
- RSSかどうかの検証は行わない（ロード時にエラーが出る）
- FeedList の Add Feed: 追加後、`feeds.json` に即時書き込み
- ManageFeeds の Add Feed: メモリに追加のみ、Save で書き込み

### FR-3: SELECT ポップアップ（フィード行のみ）

- フィード行で SELECT を押すとポップアップを表示
- 非フィード行（Bookmarks / Add Feed / Settings）では SELECT を無視

ポップアップ項目:

| 項目 | 動作 |
|------|------|
| Move | Up/Down でリスト内移動、A で確定、B でキャンセル |
| Delete | 確認ダイアログ（A:削除 / B:キャンセル） |

### FR-4: Move 操作（挿入ライン方式）

- ポップアップで Move を選択すると「移動モード」に入る
- 移動元フィードはグレーアウト（CLR_HINT）でその場に残す
- 挿入ライン（水平線）が表示され、Up/Down で移動先を示す
  - ラインはフィード群の間隙（N+1箇所）を移動する
  - 上端・下端でループ
  - 長押しリピート対応（速度: `settings.scrollRepeatDelayMs` / `settings.scrollRepeatIntervalMs`）
- A で確定: 移動元を挿入ライン位置に移動 → `feeds.json` 即時書き込み
- B でキャンセル: 元の位置に戻す（変更なし）

### FR-5: Delete 操作

- ポップアップで Delete を選択すると確認ポップアップを表示
  ```
  ┌─────────────────────┐
  │  Delete this feed?  │
  │  A: Yes   B: Cancel │
  └─────────────────────┘
  ```
- A で削除 → `feeds.json` 即時書き込み
- B でキャンセル

### FR-6: Settings に「Manage Feeds」追加

- Settings 画面に `Manage Feeds` 項目を追加
- A 押しで ManageFeeds 画面へ遷移

**ManageFeeds 画面仕様:**
- フィード一覧を表示（FeedListと同じデータだが、A押しでフィードに入らない）
- **A（フィード行）** → ポップアップ表示（Move / Delete）— SELECT ではなく A
- **Move / Delete の挙動** → FR-4 / FR-5 と同じ
- **Save** 項目: 変更を feeds.json に書き込み → Settings に戻る
- **Exit** 項目:
  - 変更あり → 確認ポップアップ「Discard changes? A:Yes B:Cancel」
  - 変更なし → 確認なしで Settings に戻る
- **Add Feed**: URLをSwkbdで入力 → フィード末尾に追加（即時反映、Saveで保存）
- 変更は Manage Feeds 内ではメモリのみ保持、Save で feeds.json に書き込み

### FR-7: feeds.json 書き込み

- 追加・削除・並び替えのたびに `feeds.json` を即時上書き保存
- 書き込み失敗時は statusMsg にエラー表示

---

## 非機能要件

- **操作感**: 既存の BookmarkList 削除確認ダイアログと同じUXパターンを踏襲
- **安全性**: Delete は必ず確認ダイアログを挟む
- **即時永続化**: 操作のたびに `feeds.json` を保存（アプリ終了時のみ保存しない）
- **スレッド安全**: feeds.json 書き込みはメインスレッドから行う（ネットワークスレッドとの競合なし）

---

## 画面遷移への影響

```
FeedList
  ├─ A([Add Feed]) → Swkbd → FeedList（フィード追加済み）
  ├─ SELECT(フィード行) → ポップアップ
  │     ├─ Move → 移動モード → A:確定 / B:キャンセル
  │     └─ Delete → 確認ダイアログ → A:削除 / B:キャンセル
  └─ （既存遷移は変更なし）

Settings
  └─ A(Manage Feeds) → ManageFeeds画面（FeedList同等）
        └─ B → Settings
```
