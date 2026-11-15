#!/usr/bin/env bash
# One release, from PR #1's tested merge only. Invoked after all CI gates pass.
set -euo pipefail

test "$GITHUB_REPOSITORY" = kuroki727/strazh
test "$GITHUB_REF" = refs/heads/main
test "$GITHUB_EVENT_NAME" = push
version=v0.1.0

gh api "repos/$GITHUB_REPOSITORY/pulls/1" \
    --jq '[.merged, .merge_commit_sha, .head.sha, .base.ref, .head.ref] | @tsv' \
    > "$RUNNER_TEMP/strazh-release-pr.tsv"
IFS=$'\t' read -r merged merge_sha head_sha base_ref head_ref \
    < "$RUNNER_TEMP/strazh-release-pr.tsv"
if [ "$merged" != true ] || [ "$merge_sha" != "$GITHUB_SHA" ]; then
    echo 'No v0.1.0 publication: this is not the merge of PR #1.'
    exit 0
fi
test "$base_ref" = main
test "$head_ref" = release/v0.1.0
test "$(git rev-parse HEAD)" = "$GITHUB_SHA"
worktree_status=$(git status --porcelain)
test -z "$worktree_status"
test "$(gh api "repos/$GITHUB_REPOSITORY/git/ref/heads/main" --jq .object.sha)" = "$GITHUB_SHA"
test "$(gh api "repos/$GITHUB_REPOSITORY/git/commits/$head_sha" --jq .tree.sha)" = "$(git rev-parse 'HEAD^{tree}')"

# Never replace a preexisting tag or release, even on a workflow rerun.
test "$(gh api "repos/$GITHUB_REPOSITORY/git/matching-refs/tags/$version" \
    --jq '[.[] | select(.ref == "refs/tags/v0.1.0")] | length')" = 0
existing_release=$(gh api "repos/$GITHUB_REPOSITORY/releases" --paginate \
    --jq '.[] | select(.tag_name == "v0.1.0") | .id')
test -z "$existing_release"
grep -q '^## \[0.1.0\]' CHANGELOG.md

git config user.name 'github-actions[bot]'
git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git tag --annotate "$version" --message 'Strazh v0.1.0 — experimental Linux init' "$GITHUB_SHA"
test "$(git rev-parse "$version^{}")" = "$GITHUB_SHA"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push origin "refs/tags/$version"
gh release create "$version" --repo "$GITHUB_REPOSITORY" --verify-tag \
    --target "$GITHUB_SHA" --title 'Strazh v0.1.0' --notes-file CHANGELOG.md
printf 'Merge commit: %s\nAnnotated tag: %s\n' "$GITHUB_SHA" "$(git rev-parse "$version^{tag}")"
gh release view "$version" --repo "$GITHUB_REPOSITORY" --json url --jq .url
