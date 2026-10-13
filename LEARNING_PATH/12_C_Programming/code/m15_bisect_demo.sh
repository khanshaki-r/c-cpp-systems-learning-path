#!/usr/bin/env bash
# m15_bisect_demo.sh - practise "git bisect run" on a throwaway repository.
#
# It builds a tiny C project with 8 commits. Commit 5 is a "harmless refactor" of a fee
# formula that actually changes the result (it divides before multiplying). A test script
# exits 0 for good, 1 for bad, and 125 for "cannot build, skip this commit". git bisect run
# then finds commit 5 in 3 steps instead of you reading 8 diffs.
#
# Run:   bash m15_bisect_demo.sh
# Needs: git, gcc. Everything happens in a temporary directory that is removed at the end.
set -euo pipefail

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/repo"
cd "$work/repo"
git init -q
git config user.name "Bisect Demo"
git config user.email "demo@example.invalid"

commit_version() {   # $1 = version number, $2 = fee expression, $3 = commit message
    cat > fee.c <<EOF
/* fee.c - version $1 */
#include <stdio.h>
#include <stdlib.h>

/* 0.5% fee on an amount in paise, rounded down */
long fee_paise(long amount_paise) { return $2; }

int main(int argc, char **argv)
{
    long amount = argc > 1 ? atol(argv[1]) : 1999;
    printf("%ld\n", fee_paise(amount));
    return 0;
}
EOF
    git add fee.c
    git commit -q -m "$3"
}

commit_version 1 "amount_paise * 5 / 1000" "v1: initial fee calculation"
commit_version 2 "amount_paise * 5 / 1000" "v2: add usage comment"
commit_version 3 "(amount_paise * 5) / 1000" "v3: parenthesise for readability"
commit_version 4 "(amount_paise * 5) / 1000" "v4: bump version string"
commit_version 5 "amount_paise / 1000 * 5" "v5: avoid overflow in fee formula"
commit_version 6 "amount_paise / 1000 * 5" "v6: tidy includes"
commit_version 7 "amount_paise / 1000 * 5" "v7: update banner"
commit_version 8 "amount_paise / 1000 * 5" "v8: release 2.3.1"

# The test lives OUTSIDE the repository so checking out old commits never changes it.
cat > "$work/test.sh" <<'EOF'
#!/bin/sh
gcc -std=c17 -O2 -o fee fee.c 2>/dev/null || exit 125   # 125: skip, cannot build
[ "$(./fee 1999)" = "9" ]                                # 1999 * 0.005 = 9.995 -> 9
EOF
chmod +x "$work/test.sh"

echo "== history (newest first)"
git log --oneline
echo "== bisect"
first=$(git rev-list --max-parents=0 HEAD)
git bisect start HEAD "$first" >/dev/null
git bisect run "$work/test.sh" | grep -E "is the first bad commit|^    v|running"
git bisect reset >/dev/null 2>&1
echo "== the guilty change"
git log -1 --format='%h %s' "$(git rev-list HEAD --grep='v5:')"
git diff "$(git rev-list HEAD --grep='v4:')" "$(git rev-list HEAD --grep='v5:')" -- fee.c | grep '^[-+]long'
