# 06. Git properly

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

Git is a content-addressed database of snapshots with a graph of pointers on top. Every commit is
a complete snapshot of your tree (stored efficiently), named by the SHA-1 of its contents, pointing
at its parent(s). A branch is a movable sticky note with a commit's name on it. `HEAD` is a sticky
note saying which branch you are on. Once you see Git this way, every command is "move a sticky
note", "make a new snapshot", or "show me the graph", and nothing is scary: a commit that exists is
almost impossible to lose.

You use Git already. This module is about using it the way a senior engineer at a company with code
review does: small commits with good messages, rebase to keep history readable, bisect to find the
commit that broke things, reflog to recover from mistakes, and a GitHub profile that makes a hiring
manager stop scrolling.

Analogy: a commit is a save point in a game. A branch is a label on a save point that moves forward
as you keep playing. Rebasing is replaying your last five moves on top of someone else's save.
Reflog is the list of every save point your console ever pointed at, even ones you "deleted".

## 2. The real thing

### 2.1 The object model

Four object types live in `.git/objects/`, each named by the SHA-1 of its content:

```
blob    file contents (no name, no mode)
tree    a directory: list of (mode, type, hash, name) entries pointing at blobs and trees
commit  tree hash + parent hash(es) + author + committer + message
tag     (annotated) a named, signed pointer at a commit with a message
```

```
                 refs/heads/main --> commit c3
                                       |  tree t3
                                       |  parent c2
                                       v
   c1 <------- c2 <------- c3           t3 ---> blob "store.cpp v3"
                                           ---> tree src/ ---> blob "main.cpp"
   HEAD --> refs/heads/main
```

Because a commit's hash covers its tree and its parent's hash, changing anything anywhere in
history changes every hash after it. That is why "rewriting history" means creating new commits,
never editing old ones, and why a hash is a tamper-evident identifier.

```bash
git cat-file -p HEAD           # the commit object: tree, parent, author, message
git cat-file -p HEAD^{tree}    # the tree: mode type hash name
git cat-file -t <hash>         # type of any object
git rev-parse HEAD             # the full hash HEAD points at
git log --oneline --graph --all --decorate   # the picture above, from the real repo
```

Same content means same hash: a file copied to ten directories is stored once. Git stores loose
objects zlib-compressed, then periodically packs them into `.git/objects/pack/*.pack` with delta
compression (`git gc`).

### 2.2 The three areas

```
working tree  --git add-->  index (staging area)  --git commit-->  HEAD (repository)
     ^                            |                                   |
     +------- git restore --------+------ git restore --staged -------+
```

- **Working tree**: the files you edit.
- **Index**: the exact snapshot the next commit will contain. `git add -p` stages hunks
  interactively so one editing session can become two clean commits.
- **HEAD**: the current commit.

```bash
git status                     # working tree vs index vs HEAD
git diff                       # working tree vs index (unstaged changes)
git diff --staged              # index vs HEAD (what the commit will contain)
git add -p                     # stage hunk by hunk: y/n/s(split)/e(edit)
git restore file               # discard working-tree change (DESTRUCTIVE, no undo)
git restore --staged file      # unstage, keep the edit
git commit -v                  # editor shows the diff below the message
git commit --amend --no-edit   # fold staged changes into the last commit (local only)
```

### 2.3 Branching

A branch is a 41-byte file `.git/refs/heads/<name>` containing a commit hash. Creating one is free.

```bash
git switch -c feature/parser-crlf        # create and switch (replaces `checkout -b`)
git switch main
git branch -vv                           # local branches with upstream and ahead/behind
git branch -d feature/x                  # delete merged branch; -D forces
git push -u origin feature/parser-crlf   # publish and set upstream
git fetch --prune                        # update remote-tracking refs, drop deleted remote branches
```

`origin/main` is a **remote-tracking branch**: your last-known position of `main` on the server.
`git fetch` updates it; `git pull` is `fetch` + `merge` (or + `rebase` with
`pull.rebase=true`, which you want).

### 2.4 Merge vs rebase

Two ways to bring `main`'s new commits into your feature branch:

```
Before:          A---B---C  main
                  \
                   D---E    feature

Merge:           A---B---C-------  main
                  \       \
                   D---E---M        feature   (M has two parents; history shows the fork)

Rebase:          A---B---C          main
                          \
                           D'--E'   feature   (D and E REPLAYED on C: new hashes, linear history)
```

```bash
git switch feature
git merge main                    # creates merge commit M
git rebase main                   # rewrites D,E as D',E' on top of C
git rebase -i main                # interactive: reorder, squash, reword, drop (see 2.6)
git push --force-with-lease       # after rebasing a branch you already pushed
```

**Rule**: rebase your own unpublished or personal-branch commits to keep them clean; merge (or
squash-merge) into shared branches; **never rebase a branch others have based work on** because
their commits point at the old hashes. `--force-with-lease` refuses to overwrite if someone pushed
to the branch since your last fetch; plain `--force` does not check. Always `--force-with-lease`.

Fast-forward: if `main` has not moved since you branched, merging your branch just moves the
`main` pointer forward; no merge commit. `git merge --no-ff` forces a merge commit (some teams want
the "bubble" to show feature boundaries); `--ff-only` refuses unless fast-forward is possible.

Squash merge (GitHub "Squash and merge"): all feature commits become one commit on `main`. Clean
`main`, but the feature branch's individual commits are gone from `main`'s history. Good default
for small PRs; bad when the PR is a series of meaningful steps.

### 2.5 Resolving conflicts

A conflict happens when both sides changed the same lines. Git stops and marks the file:

```
<<<<<<< HEAD
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
=======
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
>>>>>>> feature/strip-all
```

```bash
git status                         # "both modified: src/protocol.cpp"
git diff                           # shows conflict hunks with both sides
# edit the file: keep one side, the other, or write the combination; delete the markers
git add src/protocol.cpp           # marks resolved
git merge --continue               # or: git rebase --continue
git merge --abort                  # or: git rebase --abort   -> back to before you started
git checkout --ours file / --theirs file   # take one side wholesale (careful: in a rebase,
                                           # "ours" is the branch being rebased ONTO)
git mergetool                      # launches configured tool (meld, vimdiff, VS Code)
```

During a **rebase**, conflicts can occur once per replayed commit; fix, `git add`, `git rebase
--continue`, repeat. Enable `git config --global rerere.enabled true` and Git remembers how you
resolved a conflict and reapplies it if the same conflict appears again (common during long rebases).

Reduce conflicts: small PRs, rebase often, do not reformat whole files in the same commit as a
logic change, agree on `.clang-format` so formatting is never a diff.

### 2.6 Interactive rebase (described, no editor needed)

`git rebase -i <base>` opens an editor with one line per commit, oldest first:

```
pick a1b2c3 Add CRLF stripping to parser
pick d4e5f6 fix typo
pick 789abc Add tests for CRLF
```

You change the verb at the start of each line: `pick` keep, `reword` keep but edit message,
`edit` stop to amend, `squash` meld into previous keeping both messages, `fixup` meld into previous
discarding this message, `drop` delete, and reordering lines reorders commits. Then save; Git
replays accordingly.

To do this **without an interactive editor** (scripts, CI, or just preference):

```bash
# squash the last 3 commits into one with a new message
git reset --soft HEAD~3 && git commit -m "Add CRLF handling to parser with tests"

# fixup workflow: commit small fixes marked for a target commit, then autosquash
git commit --fixup=a1b2c3                 # message "fixup! Add CRLF stripping to parser"
GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash main   # applies all fixup! lines, no editor

# reword the last commit
git commit --amend -m "New message"

# drop a commit in the middle
git rebase --onto <commit-before-bad> <bad-commit>   # replays everything after bad onto before
```

`GIT_SEQUENCE_EDITOR=true` makes the "editor" a no-op that accepts the generated todo list; with
`--autosquash` that list already has `fixup!` commits moved and marked. This is the technique to
know for interviews: "I use `--fixup` and `--autosquash` to keep PR history clean".

### 2.7 Finding the breaking commit: bisect

Tests passed at `v1.2.0`, fail on `main`, 60 commits in between. Binary search:

```bash
git bisect start
git bisect bad                  # current commit is bad
git bisect good v1.2.0          # this tag was good
# Git checks out the midpoint. Build and test. Then:
git bisect good    # or
git bisect bad
# ~6 steps for 60 commits. At the end: "<hash> is the first bad commit"
git bisect reset                # back to where you were
```

Automated: `git bisect run ./scripts/bisect_test.sh` where the script exits 0 for good, 1-124
or 126-127 for bad, 125 for "cannot test this commit, skip". Example script:

```bash
#!/usr/bin/env bash
cmake --preset debug >/dev/null && cmake --build --preset debug >/dev/null || exit 125
ctest --preset debug -R 'ParseCommand.TrailingCarriageReturnIsStripped' >/dev/null
```

Bisect is why small, buildable commits matter: a commit that does not compile is a hole in the
search space.

### 2.8 Stash, reflog, and getting out of trouble

```bash
git stash                       # shelve working tree + index; tree becomes clean
git stash -u                    # include untracked files
git stash list; git stash show -p stash@{0}
git stash pop                   # reapply and drop (conflicts possible)
git stash apply                 # reapply and keep
```

**Reflog** records every position `HEAD` and each branch has had, for 90 days by default. This is
the undo button for almost everything.

```bash
git reflog                      # HEAD@{0}: commit: ..., HEAD@{1}: rebase: ..., HEAD@{2}: reset: ...
git reflog show feature/x       # per branch
```

Recovery recipes:

| Mistake | Fix |
|---|---|
| `git reset --hard` lost commits | `git reflog`, find the hash before the reset, `git reset --hard <hash>` or `git branch rescue <hash>` |
| Deleted a branch | `git reflog` shows its last commit; `git branch name <hash>` |
| Rebase went wrong | `git reflog`, `git reset --hard HEAD@{n}` where n is before the rebase started; or `git rebase --abort` if still in progress |
| Amended the wrong commit | `git reflog`, the pre-amend commit is `HEAD@{1}`; `git reset --soft HEAD@{1}` |
| Committed to `main` instead of a branch | `git branch feature`, `git reset --hard origin/main`, `git switch feature` |
| Committed a secret | rotate the secret first; then `git filter-repo` (not `filter-branch`) to remove it from history; force-push; everyone re-clones |
| Detached HEAD with commits | `git switch -c save-me` |

`git fsck --lost-found` finds dangling commits not in any reflog. Objects are only deleted by
`git gc` when unreachable and older than the reflog expiry; in practice, a commit you made this
month is recoverable.

### 2.9 `.gitignore`, attributes, config

```gitignore
# .gitignore for a CMake C++ project
build/
build-*/
cmake-build-*/
CMakeUserPresets.json
compile_commands.json
.cache/            # clangd
*.o
*.a
*.so
*.so.*
core
core.*
*.log
.vscode/
.idea/
__pycache__/
*.pyc
.env
```

Rules: patterns are relative to the `.gitignore`'s directory; a trailing `/` means directory
only; `!pattern` re-includes; `**` matches across directories. A file already tracked is not
affected by `.gitignore`: `git rm --cached file` untracks it. Global ignores (editor swap files)
go in `~/.config/git/ignore`, not in every project.

`.gitattributes` for line endings and diff behaviour:

```
* text=auto eol=lf
*.sh text eol=lf
*.bat text eol=crlf
*.png binary
```

`eol=lf` for shell scripts is important: a script with CRLF fails with `bad interpreter` (module 01).

Config worth setting once:

```bash
git config --global user.name "Shakir ..."
git config --global user.email "rajaman.ar3@gmail.com"
git config --global init.defaultBranch main
git config --global pull.rebase true
git config --global push.default current
git config --global push.autoSetupRemote true
git config --global fetch.prune true
git config --global rerere.enabled true
git config --global core.autocrlf input       # on WSL/Linux: convert CRLF to LF on commit
git config --global diff.algorithm histogram   # better diffs for moved blocks
git config --global merge.conflictstyle zdiff3 # shows the common ancestor in conflict hunks
git config --global rebase.autosquash true
git config --global rebase.autostash true
git config --global alias.lg "log --oneline --graph --decorate --all"
git config --global alias.st "status -sb"
```

### 2.10 Commit messages

```
Strip trailing CR in protocol parser

Windows clients and `nc -C` send CRLF. The parser treated "PING\r" as an
unknown command. Strip a single trailing '\r' before tokenising.

Only one CR is stripped on purpose: "PING\r\r" is still malformed.

Fixes #42
```

- Subject: imperative mood ("Add", "Fix", "Strip"), 50 characters or so, no trailing period.
  It completes the sentence "If applied, this commit will ...".
- Blank line, then body wrapped at 72: **why**, not what (the diff shows what). What was wrong,
  why this fix, what alternatives were rejected, side effects.
- Reference issues. Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`,
  `ci:`, `build:`, `perf:`, `chore:`) is a common prefix scheme that lets tools generate
  changelogs and compute the next version; use it if the project does.
- One logical change per commit. "Fix parser and update CI and rename variable" is three commits.

### 2.11 Pull request workflow

```bash
git switch main && git pull
git switch -c fix/parser-crlf
# ... work, commit in small steps ...
cmake --preset asan && cmake --build --preset asan && ctest --preset asan    # before pushing
git push -u origin fix/parser-crlf
gh pr create --fill --base main           # GitHub CLI; or use the web UI
# review comments -> fix -> git commit --fixup=<hash> -> rebase --autosquash -> push --force-with-lease
gh pr checks; gh pr merge --squash --delete-branch
```

A good PR: under ~400 changed lines (reviewers lose accuracy beyond that, typical finding from
code-review studies), one purpose, description states the problem, the approach, how it was
tested, and anything the reviewer should look at first. Screenshots or log excerpts where relevant.
Draft PRs for early feedback. Link the issue.

### 2.12 Code review etiquette

As author: respond to every comment (fix, or explain why not); do not take it personally; make
the reviewer's job easy with a clear description and small diffs; never force-push during an
active review without saying so (comments lose their anchor).

As reviewer: review within a working day; comment on the code, not the person ("this loop
re-locks per iteration" not "you re-lock per iteration"); distinguish blocking issues from
nits (prefix `nit:`); ask questions when unsure; approve when it is better than what exists, not
when it is perfect; praise good things, briefly. Look for: correctness, error paths, tests,
naming, concurrency, resource ownership. Do not bikeshed formatting if `clang-format` exists.

### 2.13 Protecting `main`

In GitHub: Settings -> Branches -> Add rule for `main`: require a pull request before merging,
require 1 approval, require status checks to pass (your CI jobs, module 09), require branches to
be up to date, require linear history (forces squash or rebase merges), do not allow force
pushes, do not allow deletions. Optionally require signed commits. `CODEOWNERS` file assigns
reviewers by path. Azure DevOps has the same under Branch Policies.

Result: nothing reaches `main` without building clean under `-Werror`, passing tests, passing
ASan, and a human reading it. That sentence is a strong interview answer.

### 2.14 Tags and semantic versioning

```bash
git tag -a v1.2.0 -m "tcpkv 1.2.0: CRLF support, TSan-clean store"   # annotated: has author, date, message
git tag v1.2.0-rc1                                                     # lightweight: just a pointer
git push origin v1.2.0        # tags are not pushed by default; --tags pushes all
git describe --tags           # v1.2.0-3-g1a2b3c4: 3 commits after v1.2.0 at hash 1a2b3c4
git tag -l 'v1.*'
```

**Semantic versioning** `MAJOR.MINOR.PATCH`: PATCH for bug fixes with no API change, MINOR for
backward-compatible additions, MAJOR for breaking changes. Pre-release `-rc1`, build metadata
`+build.5`. For a library this is a contract about source and ABI compatibility (module 02
soname: the soname major usually tracks the MAJOR). For a server it tells operators whether an
upgrade needs a config migration. CMake's `project(tcpkv VERSION 0.1.0)` should come from the tag:
`git describe` in a configure step, or a release script that updates both.

Release flow: tag on `main`, CI builds artefacts for the tag (module 09), GitHub Release with
changelog, Docker image tagged `tcpkv:1.2.0` and `tcpkv:1.2` (module 08).

### 2.15 Making a GitHub profile look strong

Hiring managers spend about a minute. Make it count:

- **Profile README** (`<username>/<username>` repo): two lines about what you do (C on Windows
  for core banking, 2.5 years; learning Linux C++ systems programming), three links to your best
  repos, your email. No wall of badges.
- **Pin 4-6 repos**, each with: a one-paragraph README describing what it is and why, a build
  badge that is green, a "Build and run" section with exact commands, a short architecture note,
  and tests visible in the tree. tcpkv is the showcase: README, CI badge, `cmake --preset`,
  Dockerfile, a diagram of the connection model.
- **Commit history that looks like work**: regular commits over weeks with real messages beat
  one 10,000-line "initial commit". Reviewers click into commits.
- **Topics and description** set on each repo so they are searchable.
- **Issues and PRs on your own repos**: open an issue for the next feature, close it with a PR.
  Shows process.
- **A LICENSE** file (MIT is fine) so the code is usable.
- **No secrets, no `build/` committed, no 500 MB binaries**. A `.gitignore` that works is itself a signal.
- Contributing a small fix to a well-known project (documentation counts) and linking the merged PR
  is worth more than another toy repo.

## 3. Code

No C++ in this module. Build a repository for `code/tcpkv` and practise the graph:

```bash
cp -r code/tcpkv ~/projects/tcpkv && cd ~/projects/tcpkv
git init -b main
cat > .gitignore <<'EOF'
build/
CMakeUserPresets.json
compile_commands.json
.cache/
EOF
git add . && git commit -m "Initial tcpkv skeleton: CMake, library, server, GoogleTest"
git lg

# a feature branch with three commits, then clean them up without an editor
git switch -c feature/strip-all-eol
echo "// change 1" >> src/protocol.cpp && git commit -am "Strip all trailing EOL chars"
echo "// typo"     >> src/protocol.cpp && git commit -am "fix typo"
echo "// tests"    >> tests/test_protocol.cpp && git commit -am "Add EOL tests"
git commit --allow-empty --fixup=HEAD~2        # mark a fixup for the first commit
GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash main
git lg                                          # fixup folded in; three commits remain

# simulate main moving, then rebase and resolve a conflict
git switch main
echo "// main change" >> src/protocol.cpp && git commit -am "Main: tweak protocol"
git switch feature/strip-all-eol
git rebase main                                 # CONFLICT in src/protocol.cpp
git diff                                        # read the <<<< ==== >>>> hunk (zdiff3 shows base too)
# edit src/protocol.cpp to keep both lines, then:
git add src/protocol.cpp && GIT_EDITOR=true git rebase --continue
git lg

# break something and recover with reflog
git reset --hard HEAD~2                         # "lost" two commits
git reflog | head -5
git reset --hard 'HEAD@{1}'                     # back
git lg

# bisect practice
for i in 1 2 3 4 5 6 7 8; do echo "// c$i" >> src/store.cpp; git commit -qam "step $i"; done
echo "BUG" >> src/store.cpp && git commit -qam "step 9 introduces BUG"
for i in 10 11 12; do echo "// c$i" >> src/store.cpp; git commit -qam "step $i"; done
git bisect start HEAD HEAD~12
git bisect run sh -c '! grep -q BUG src/store.cpp'   # exit 0 (good) when BUG absent
git bisect reset
```

## 4. Under the hood (senior-level)

**Content addressing.** `git hash-object file` computes `sha1("blob " + size + "\0" + content)`.
`.git/objects/ab/cdef...` is that object zlib-compressed. Trees hash their entries, commits hash
their tree and parents. The whole history is a Merkle DAG; a single hash authenticates everything
reachable from it. Git is migrating to SHA-256 (`git init --object-format=sha256`), but SHA-1 with
collision detection remains the default in 2026.

**Packfiles and deltas.** Loose objects are full copies. `git gc` (run automatically) writes a
packfile where similar objects (versions of the same file) are stored as deltas against a base,
plus an `.idx` for lookup. This is why a repository with 10,000 versions of a 1 MB file can be
50 MB. Clones transfer packs; `--depth 1` requests only the tips (CI does this, module 09).

**What `rebase` actually does.** For each commit to replay, Git computes the diff from that
commit's parent to the commit, applies it onto the new base with a three-way merge (base =
original parent, ours = new base, theirs = the commit), and creates a new commit object with the
new parent and the same author/message. New hash every time, even if the content is identical,
because the parent changed. Rerere stores (conflict-hunk-hash -> resolution) in `.git/rr-cache`.

**Index format.** `.git/index` is a binary file listing every tracked path with its blob hash,
mode, size and mtime/ctime. `git status` compares stat data first and hashes content only for
files whose stat changed, which is why it is fast and why touching 10,000 files makes the next
`git status` slow once. On WSL `/mnt/c`, every stat is a 9P round trip; another reason to keep
repos in the Linux filesystem (module 01). `core.fsmonitor=true` uses a daemon to avoid the stats.

**Refs and reflog.** A ref is a file (or a line in `.git/packed-refs`). Updating a ref appends to
`.git/logs/<ref>`: old hash, new hash, who, when, why. That log is the reflog. `gc.reflogExpire`
is 90 days (30 for unreachable entries). Objects reachable only from the reflog survive `gc` until
the entry expires. This is the entire basis of "Git never loses committed work".

**Merge strategies.** Default `ort` (since 2.34) replaced `recursive`: it finds the merge base(s),
does a three-way merge per path with rename detection, and handles criss-cross merges by merging
the bases first. `-X ours`/`-X theirs` choose a side for conflicts only; `-s ours` ignores the
other side entirely. `zdiff3` conflict style shows the base version between the two sides, which
turns "which one is right?" into "what did each side change?".

**Why SHA-1 commits still matter for security.** Signed tags (`git tag -s`) and signed commits
(`commit.gpgsign`, or SSH signing with `gpg.format=ssh`) sign the hash; because the hash covers
the whole reachable history, a signed tag authenticates every ancestor. Release pipelines verify
`git verify-tag`.

## 5. In production

- **History is documentation that cannot go stale.** Six months from now, `git log -S
  'remove_suffix' --oneline src/protocol.cpp` and `git blame -w -C` on a line will tell you why a
  decision was made, if the commit message said why. Write for that reader.
- **Small commits are a debugging tool.** `git bisect run ctest` finds a regression in minutes
  only if each commit builds. Squash fixups; do not squash meaningful steps.
- **Never force-push shared branches.** `--force-with-lease` protects against overwriting a
  colleague; branch protection protects `main`. If you must rewrite shared history (a leaked
  secret), announce it, use `git filter-repo`, and have everyone re-clone.
- **Secrets**: rotate first, rewrite second. A key that was ever pushed to a public repo is
  compromised regardless of history rewriting (mirrors, caches). Prevent with a pre-commit hook or
  `gitleaks` in CI (module 09).
- **Large binaries** (test fixtures, dumps) go in Git LFS or an artifact store, not the repo.
  A 200 MB core dump committed once is downloaded by every clone forever.
- **Your current environment:** if your employer uses Azure DevOps Repos or TFVC, the PR workflow,
  branch policies and `azure-pipelines.yml` (module 09) are the same ideas with different buttons.
  Being able to say "we used feature branches with required reviewers and build validation
  policies" maps one to one onto GitHub's protected branches.
- **3 AM:** production broke after a deploy; 40 commits went out. `git log v1.2.0..v1.3.0
  --oneline -- src/store/` narrows to the subsystem in seconds; `git bisect run` with the failing
  integration test finds the commit while you make coffee. Then `git revert <hash>` (a new commit
  that undoes it: safe on shared branches) ships the fix, and the real fix follows in daylight.

## 6. Try it

1. **(easy)** In any repo, run `git cat-file -p HEAD`, then `git cat-file -p` on its tree hash,
   then on one blob hash. Done when you can draw the commit -> tree -> blob chain for one file.
2. **(easy)** Set every config line from section 2.9. Make a change to a tracked file, stage half
   of its hunks with `git add -p`, and commit; then commit the rest. Done when `git log -p` shows
   two commits touching the same file with different hunks.
3. **(medium)** Create three commits on a branch, mark the second as a fixup of the first using
   `--fixup`, and fold it in with `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash`. Done when
   `git lg` shows two commits and `git show` of the first contains both changes.
4. **(medium)** Produce a conflict by editing the same line on `main` and a branch, rebase the
   branch with `merge.conflictstyle=zdiff3`, and resolve it. Then `git reset --hard` the branch to
   before the rebase using reflog. Done when you can explain each of the three regions in the
   zdiff3 hunk.
5. **(hard)** Write `scripts/bisect_test.sh` for tcpkv that configures, builds (exit 125 on build
   failure) and runs one ctest. Introduce a bug in `Store::del` ten commits ago (use a loop of
   trivial commits), then `git bisect run` it. Done when bisect names the right commit without
   you building anything by hand.
6. **(hard)** Set up the tcpkv GitHub repository: README with build badge placeholder, LICENSE,
   `.gitignore`, `.gitattributes`, branch protection on `main` requiring the CI check from module
   09, a `v0.1.0` annotated tag pushed, and the repo pinned on your profile. Done when a PR you
   open cannot be merged until CI passes.

## 7. Interview questions

**Q: Explain Git's object model.**
Blobs hold file contents, trees hold directory listings pointing at blobs and subtrees, commits
point at one tree and their parent commit(s) with author and message, tags name a commit. Every
object is named by the SHA-1 of its content, so history is a Merkle DAG: one hash authenticates
all ancestors, and identical content is stored once.

**Q: Merge vs rebase: when do you use which?**
Merge preserves the true history with a merge commit and is safe on shared branches. Rebase
replays my commits onto the new base, producing linear history with new hashes, and is right for
my own feature branch before review. Never rebase commits others have built on; after rebasing a
pushed branch, use `--force-with-lease`.

**Q: You ran `git reset --hard` and lost a day's commits. What now?**
`git reflog` lists every position HEAD had, including the commit before the reset. `git reset
--hard HEAD@{1}` or `git branch rescue <hash>` restores it. Committed objects are only garbage
collected after the reflog entry expires, 90 days by default.

**Q: How does `git bisect` work and what does it require?**
Binary search over the commit range between a known good and a known bad commit; log2(n) steps.
`git bisect run <script>` automates it using the script's exit code: 0 good, non-zero bad, 125
skip. It requires every commit to build, which is why I keep commits small and never commit broken
intermediate states.

**Q: What is a fast-forward merge?**
When the target branch has no commits the source lacks, Git moves the target pointer to the
source's tip without creating a merge commit. `--ff-only` enforces it, which some teams use to keep
`main` linear together with rebase-before-merge.

**Q: How do you keep a PR's history clean without an interactive editor?**
Commit review fixes with `git commit --fixup=<target>`, then
`GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash main`, which folds every fixup into its
target automatically. For squashing the last N commits, `git reset --soft HEAD~N` and one new
commit.

**Q: What does semantic versioning promise?**
MAJOR changes may break compatibility, MINOR adds features compatibly, PATCH fixes bugs with no
interface change. For a C or C++ library the MAJOR usually matches the shared library soname so
the dynamic loader enforces the contract.

**Q: What should a commit message contain?**
An imperative subject under about 50 characters that completes "this commit will ...", a blank
line, and a body explaining why the change was made, what alternatives were rejected and any
side effects. The diff shows what changed; the message must preserve the reasoning.

## 8. Recap

- Objects: blob, tree, commit, tag; content-addressed by SHA-1; a branch is a pointer, HEAD points at a branch.
- Working tree -> index (`add -p`) -> commit; `diff` vs `diff --staged`.
- Merge keeps the fork; rebase replays with new hashes; never rebase shared history; `--force-with-lease`.
- Conflicts: edit, remove markers, `add`, `--continue`; `--abort` to bail; `zdiff3` and `rerere` help.
- `--fixup` + `--autosquash` with `GIT_SEQUENCE_EDITOR=true` cleans history without an editor.
- `git bisect run script` finds the breaking commit automatically; needs buildable commits.
- Reflog is the undo for reset, rebase, amend and deleted branches.
- `.gitignore` for `build/`, `.gitattributes` for `eol=lf` on scripts, `core.autocrlf input` on WSL.
- Subject imperative and short, body explains why; one logical change per commit; small PRs.
- Protect `main`: PR required, review required, CI required, no force push; tag releases with semver.
