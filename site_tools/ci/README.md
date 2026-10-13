# CI workflows (not active yet)

These two GitHub Actions workflows are ready but not enabled, because pushing files into
`.github/workflows/` needs the `workflow` permission on your GitHub CLI login.

- `pages.yml` rebuilds and publishes the site on every push to `main`.
- `code-check.yml` compile-checks every C and C++ example and runs the tcpkv tests (debug + ASan).

To enable them (one time, opens a browser to approve):

```bash
gh auth refresh -h github.com -s workflow
mkdir -p .github/workflows
git mv site_tools/ci/pages.yml site_tools/ci/code-check.yml .github/workflows/
git commit -m "Enable CI: publish site and compile-check examples"
git push
gh api -X PUT repos/khanshaki-r/c-cpp-systems-learning-path/pages -f build_type=workflow
```

Until then, publish the site from your machine after editing content:

```bash
python site_tools/build_config.py
mkdocs gh-deploy --force      # builds the site and pushes it to the gh-pages branch
```
