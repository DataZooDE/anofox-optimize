#!/usr/bin/env bash
# Set the Claude Code plugin's version to the newest release in CHANGELOG.md.
#
# Part of every release, right after the "## vYYYY.MM.DD" heading is written:
#   ./scripts/bump_plugin_version.sh        (or: make bump-plugin)
# It rewrites plugins/anofox-optimize/.claude-plugin/plugin.json "version",
# every SKILL.md "version:" line and every SKILL.md "**Extension:** ... vX"
# header. Prose that names a version ("since v2026.11.7") is left alone.
# `make check-docs` fails while any of them is behind, and the deploy waits
# for it, so a forgotten bump stops the release instead of shipping stale.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
plugin="$root/plugins/anofox-optimize"

version="$(sed -nE 's/^## v([0-9]{4}\.[0-9]{1,2}\.[0-9]{1,2}).*/\1/p' "$root/CHANGELOG.md" | head -1)"
if [[ -z "$version" ]]; then
  echo "no '## vYYYY.MM.DD' release heading in CHANGELOG.md" >&2
  exit 1
fi

sed -i -E 's/^(  "version": ")[^"]*(",?)$/\1'"$version"'\2/' "$plugin/.claude-plugin/plugin.json"
for f in "$plugin"/skills/*/SKILL.md; do
  sed -i -E \
    -e 's/^version: .*/version: '"$version"'/' \
    -e 's/^(\*\*Extension:\*\* `anofox_optimize` v)[0-9.]+/\1'"$version"'/' \
    "$f"
done

echo "plugin and skills set to $version"
grep -H '"version"' "$plugin/.claude-plugin/plugin.json"
grep -H -E '^version: ' "$plugin"/skills/*/SKILL.md
