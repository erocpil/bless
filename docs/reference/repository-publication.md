# Public Repository Publication

`bless-private` is the canonical repository.
It retains the complete development history and is the only repository that receives normal development commits.
The public `bless` repository exposes a deliberately history-free snapshot on `main`.

## Invariants

- `bless-private/main` contains the complete history.
- `bless/main` contains exactly one root commit with no parent.
- Publish only after the private commit has passed CI.
- Never directly force-push a private commit to public `main`; Git transfers its reachable parents and would restore the complete history.
- Preserve public branches other than `main` only when explicitly required.
- Delete or retain public tags only through an explicit release decision.

## Publication Procedure

1. Commit and push the change to `bless-private/main`.
2. Wait for the private CI run to pass.
3. Record the exact private tip as `PRIVATE_SHA`.
4. Create a temporary worktree from `PRIVATE_SHA`.
5. Apply only public-repository overrides in that worktree.
6. Write its tree and create a commit with no parent.
7. Verify the public snapshot has one commit and differs from the private tree only by the approved overrides.
8. Force-push it to public `main` with a lease pinned to the observed public tip.
9. Verify the public ref, commit count, file tree, and CI run.

The following outline creates the root commit from the final public worktree.

```bash
PRIVATE_SHA=<bless-private-main-sha>
PUBLIC_OLD_SHA=<observed-public-main-sha>

git worktree add --detach /tmp/bless-publication "$PRIVATE_SHA"
cd /tmp/bless-publication

# Apply the approved public-only README badge override here.
PUBLIC_TREE=$(git write-tree)
PUBLIC_SNAPSHOT=$(git commit-tree "$PUBLIC_TREE" -m "Initial public snapshot")

git rev-list --count "$PUBLIC_SNAPSHOT"       # must print 1
git push --force-with-lease="refs/heads/main:$PUBLIC_OLD_SHA" \
  git@github.com:erocpil/bless.git "$PUBLIC_SNAPSHOT:main"
```

## Repository-Specific Overrides

The CI badge URL cannot be repository-relative.
The private README must link to `erocpil/bless-private` and the public README must link to `erocpil/bless`.
This is an intentional, approved difference between the two trees.

The container image name in CI must use `${{ github.repository }}`.
It resolves to `ghcr.io/erocpil/bless-private` in the private repository and `ghcr.io/erocpil/bless` in the public repository.
