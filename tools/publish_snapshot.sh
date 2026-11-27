#!/bin/sh
# tekken6-nx: the published history (docs/release.md, "The repository"). GitHub gets only the branch `publish`, whose
# commits are snapshots of the working branch's tree: each one has the previous snapshot as its only parent, so none of
# the working history (test notes) is published. A local pre-push hook refuses any other branch.
#   sh tools/publish_snapshot.sh "Message of the snapshot"     then: git push origin publish:main
set -e
msg="${1:?usage: publish_snapshot.sh \"message\"}"
source_ref="${SOURCE_REF:-master}"
tree=$(git rev-parse "$source_ref^{tree}")
if git rev-parse -q --verify refs/heads/publish >/dev/null; then
  parent=$(git rev-parse publish)
  if [ "$(git rev-parse "publish^{tree}")" = "$tree" ]; then
    echo "publish already holds $source_ref's tree: nothing to do"
    exit 0
  fi
  commit=$(git commit-tree "$tree" -p "$parent" -m "$msg")
else
  commit=$(git commit-tree "$tree" -m "$msg")
fi
git branch -f publish "$commit"
git log --oneline -1 publish
echo "next: git push origin publish:main"
