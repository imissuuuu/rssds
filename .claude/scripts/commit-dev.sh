#!/usr/bin/env bash
# Usage: bash .claude/scripts/commit-dev.sh "Add feature X"
#
# Commits on the branch required by .claude/harness-config.json `commit.branch`.
# If that value ends with `/` (prefix mode, e.g. "feature/"), pushes the branch
# and creates a PR to `commit.pr_base` (default "dev") unless one already exists.
set -e

if [ -z "$1" ]; then
  echo "ERROR: Commit message required." >&2
  echo "Usage: bash .claude/scripts/commit-dev.sh \"Add feature X\"" >&2
  exit 1
fi

# Auto-detect git root: first try CWD, then search one level of subdirectories
GIT_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || true
if [ -z "$GIT_ROOT" ]; then
  for d in */; do
    if [ -d "$d" ] && git -C "$d" rev-parse --show-toplevel &>/dev/null 2>&1; then
      GIT_ROOT=$(git -C "$d" rev-parse --show-toplevel)
      break
    fi
  done
fi

if [ -z "$GIT_ROOT" ]; then
  echo "ERROR: No git repository found in current directory or immediate subdirectories." >&2
  exit 1
fi

cd "$GIT_ROOT"

CONFIG=".claude/harness-config.json"
ALLOWED=""
PR_BASE="dev"
if [ -f "$CONFIG" ]; then
  ALLOWED=$(jq -r '.commit.branch // ""' "$CONFIG")
  PR_BASE=$(jq -r '.commit.pr_base // "dev"' "$CONFIG")
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)

# Validate branch against commit.branch setting
if [ -n "$ALLOWED" ]; then
  case "$ALLOWED" in
    */)
      case "$BRANCH" in
        "${ALLOWED}"*) : ;;
        *)
          echo "ERROR: Branch '$BRANCH' does not match required prefix '$ALLOWED'" >&2
          exit 1
          ;;
      esac
      ;;
    *)
      if [ "$BRANCH" != "$ALLOWED" ]; then
        echo "ERROR: Not on '$ALLOWED' branch (current: $BRANCH)" >&2
        exit 1
      fi
      ;;
  esac
fi

# clang-format チェック（インストールされている場合のみ）
if command -v clang-format >/dev/null 2>&1; then
  MODIFIED_CPP=$(git diff --name-only HEAD -- '*.cpp' '*.c' '*.h' '*.hpp' 2>/dev/null || true)
  if [ -n "$MODIFIED_CPP" ]; then
    echo "[commit-dev] clang-format check..."
    if ! echo "$MODIFIED_CPP" | xargs clang-format --dry-run --Werror -style=file 2>&1; then
      echo "ERROR: clang-format violations. Fix with: echo \"<files>\" | xargs clang-format -i -style=file" >&2
      exit 1
    fi
    echo "[commit-dev] clang-format OK"
  fi
else
  echo "[commit-dev] clang-format not found in PATH — skipping format check" >&2
fi

git add -u
# 新規ファイルも追加（git add -u は未追跡ファイルをステージしないため）
git add source/ assets/ 2>/dev/null || true
git add .github/ .clang-format .clang-tidy 2>/dev/null || true
git commit -m "$1

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"

# Push to remote
case "$ALLOWED" in
  */)
    # prefix mode: push + PR
    if ! command -v gh >/dev/null 2>&1; then
      echo "[commit-dev] gh CLI not found — skipping PR creation." >&2
      exit 0
    fi

    if git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1; then
      git push
    else
      git push -u origin "$BRANCH"
    fi

    EXISTING=$(gh pr list --head "$BRANCH" --base "$PR_BASE" --state open --json number --jq '.[0].number' 2>/dev/null || echo "")
    if [ -n "$EXISTING" ]; then
      echo "[commit-dev] PR #$EXISTING already open for $BRANCH -> $PR_BASE. Skipping create."
    else
      gh pr create --base "$PR_BASE" --head "$BRANCH" --fill
    fi
    ;;
  *)
    # exact branch mode: push + PR to pr_base (if set and gh available)
    if git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1; then
      git push
    else
      git push -u origin "$BRANCH"
    fi

    if [ -n "$PR_BASE" ] && command -v gh >/dev/null 2>&1; then
      EXISTING=$(gh pr list --head "$BRANCH" --base "$PR_BASE" --state open --json number --jq '.[0].number' 2>/dev/null || echo "")
      if [ -n "$EXISTING" ]; then
        echo "[commit-dev] PR #$EXISTING already open for $BRANCH -> $PR_BASE. Skipping create."
      else
        gh pr create --base "$PR_BASE" --head "$BRANCH" --fill
      fi
    fi
    ;;
esac
