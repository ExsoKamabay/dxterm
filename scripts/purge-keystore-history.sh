#!/usr/bin/env bash
#
# Remove keystore/ from the working tree and from every commit in history.
#
# READ THIS BEFORE RUNNING. The rewrite changes every commit hash after the first
# commit that touched keystore/. Anyone who has cloned the repository will need to
# re-clone. The script never force-pushes for you; it prints the command and stops.
#
# Prerequisite: git-filter-repo  (pip install --user git-filter-repo)
#
set -euo pipefail

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m==>\033[0m %s\n' "$*"; }

command -v git >/dev/null || die "git not found"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository"

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

# --- preconditions ----------------------------------------------------------

if [ -n "$(git status --porcelain)" ]; then
    die "working tree is dirty. Commit or stash first; a rewrite over uncommitted work loses it."
fi

if ! command -v git-filter-repo >/dev/null 2>&1 && ! python3 -c 'import git_filter_repo' 2>/dev/null; then
    die "git-filter-repo not found. Install it:  pip install --user git-filter-repo"
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)
REMOTE_URL=$(git remote get-url origin 2>/dev/null || echo "")

note "repository : $REPO_ROOT"
note "branch     : $BRANCH"
note "origin     : ${REMOTE_URL:-<none>}"
echo

if ! git log --all --oneline -- keystore/ | head -1 | grep -q .; then
    note "keystore/ does not appear anywhere in history. Nothing to purge."
    note "Still make sure the file is gone from the working tree and ignored."
    exit 0
fi

note "commits touching keystore/:"
git log --all --oneline -- keystore/ | sed 's/^/    /'
echo

printf 'Rewrite history to remove keystore/ ? This cannot be undone locally. [type REWRITE] '
read -r CONFIRM
[ "$CONFIRM" = "REWRITE" ] || die "aborted."

# --- backup -----------------------------------------------------------------

BACKUP="../$(basename "$REPO_ROOT")-backup-before-purge.bundle"
note "writing a full backup bundle to $BACKUP"
git bundle create "$BACKUP" --all
note "backup written. If anything goes wrong: git clone $BACKUP restored-repo"
echo

# --- remove from the working tree -------------------------------------------

if [ -e keystore ]; then
    note "moving keystore/ out of the repository to ../dracxterm-keystore-quarantine/"
    mkdir -p ../dracxterm-keystore-quarantine
    mv keystore/* ../dracxterm-keystore-quarantine/ 2>/dev/null || true
    rmdir keystore 2>/dev/null || true
    git rm -r --cached --ignore-unmatch keystore >/dev/null 2>&1 || true
    git commit -m "Remove signing keystore from the repository" >/dev/null 2>&1 || true
fi

# --- rewrite ----------------------------------------------------------------

note "rewriting history"
if command -v git-filter-repo >/dev/null 2>&1; then
    git filter-repo --invert-paths --path keystore --force
else
    python3 -m git_filter_repo --invert-paths --path keystore --force
fi

note "expiring reflogs and repacking"
git reflog expire --expire=now --all
git gc --prune=now --aggressive >/dev/null

# --- verify -----------------------------------------------------------------

echo
note "verification"
if git log --all --oneline -- keystore/ | head -1 | grep -q .; then
    die "keystore/ is STILL reachable in history. Do not push; investigate."
fi
if git rev-list --objects --all | grep -qi keystore; then
    die "an object still references 'keystore'. Do not push; investigate."
fi
printf '    keystore/ is absent from all refs and all reachable objects.\n'

# --- next steps -------------------------------------------------------------

cat <<EOF

$(note "history rewritten locally. Nothing has been pushed.")

git filter-repo removes 'origin' on purpose, so a rewritten history cannot be pushed by
accident. Restore it and force-push yourself, only once you have read the diff above:

    git remote add origin ${REMOTE_URL:-<your-remote-url>}
    git push --force --all origin
    git push --force --tags origin

Then, from a scratch directory:

    git clone --no-local ${REMOTE_URL:-<your-remote-url>} fresh-check
    cd fresh-check && git log --all -- keystore/     # must print nothing

Remember: the rewrite does not make the leaked key safe again. Generate a new key
(see docs/SECURITY-KEY-ROTATION.md step 1) and ask GitHub Support to purge cached
objects for the removed path.
EOF
