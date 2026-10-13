"""MkDocs hook: send links to source code to GitHub instead of the static site.

Course pages link to files such as `code/m03_file_handle_raii.cpp` or whole folders such as
`tcpkv/code/`. Those are excluded from the built site (browsers would download a .cpp instead of
showing it), so this hook rewrites every relative link that points inside a `code/` or
`reference_code/` folder, or at any non-markdown file, to the file's page on GitHub, where it is
shown with syntax highlighting.
"""
from __future__ import annotations

import posixpath
import re
from pathlib import Path

LINK_RE = re.compile(r"(\]\()([^)\s]+)(\))")
CODE_DIRS = {"code", "reference_code"}


def _rewrite(target: str, page_dir: str, docs_dir: Path, blob: str, tree: str) -> str | None:
    if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith(("#", "/")):
        return None  # absolute URL, mailto, in-page anchor or site-absolute path
    path, _, anchor = target.partition("#")
    if not path:
        return None
    resolved = posixpath.normpath(posixpath.join(page_dir, path))
    if resolved.startswith(".."):
        return None
    parts = resolved.split("/")
    in_code = any(p in CODE_DIRS for p in parts)
    is_md = resolved.endswith(".md")
    on_disk = docs_dir / resolved
    if not on_disk.exists():
        return None
    if on_disk.is_dir():
        if in_code or not (on_disk / "README.md").exists():
            return tree + resolved.rstrip("/") + "/"
        return None
    if in_code or not is_md:
        return blob + resolved + (("#" + anchor) if anchor else "")
    return None


CODE_EXT = (".cpp", ".hpp", ".c", ".h", ".sql", ".sh", ".py", ".yml", ".gdb", ".service")
# `m03_file_handle_raii.cpp`, `code/m05_x.c`, `include/tcpkv/store.hpp` - not already a link text.
INLINE_CODE_FILE_RE = re.compile(
    r"(?<!\[)`((?:[\w.-]+/)*[\w.-]+(?:" + "|".join(re.escape(e) for e in CODE_EXT) + r"))`(?!\])")
_course_index: dict[str, list[str]] = {}


def _files_in_course(docs_dir: Path, course: str) -> list[str]:
    """Every source file of one course, as docs-relative posix paths (cached per build)."""
    if course not in _course_index:
        root = docs_dir / course
        found = []
        if root.is_dir():
            for p in root.rglob("*"):
                if p.is_file() and p.suffix in {e[e.rfind("."):] for e in CODE_EXT} and "build" not in p.parts:
                    found.append(p.relative_to(docs_dir).as_posix())
        _course_index[course] = sorted(found)
    return _course_index[course]


def _link_inline_code(line: str, page_dir: str, docs_dir: Path, blob: str) -> str:
    course = page_dir.split("/")[0] if page_dir else ""
    if not course:
        return line
    files = _files_in_course(docs_dir, course)

    def repl(m: re.Match) -> str:
        name = m.group(1).lstrip("./")
        hits = [f for f in files if f == f"{course}/{name}" or f.endswith("/" + name)]
        if len(hits) > 1:  # prefer the file nearest to this page (same sub-folder tree)
            near = [f for f in hits if f.startswith(page_dir + "/")]
            hits = near if len(near) == 1 else hits
        if len(hits) != 1:
            return m.group(0)
        return f"[`{m.group(1)}`]({blob}{hits[0]})"

    return INLINE_CODE_FILE_RE.sub(repl, line)


def on_page_markdown(markdown: str, page, config, files):  # noqa: ANN001 (MkDocs hook signature)
    docs_dir = Path(config["docs_dir"])
    blob = config["extra"]["github_blob"]
    tree = config["extra"]["github_tree"]
    page_dir = posixpath.dirname(page.file.src_uri)

    out_lines = []
    in_fence = False
    for line in markdown.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            out_lines.append(line)
            continue
        if not in_fence:
            if "](" in line:
                def repl(m: re.Match) -> str:
                    new = _rewrite(m.group(2), page_dir, docs_dir, blob, tree)
                    return m.group(0) if new is None else m.group(1) + new + m.group(3)
                line = LINK_RE.sub(repl, line)
            if "`" in line:
                line = _link_inline_code(line, page_dir, docs_dir, blob)
        out_lines.append(line)
    return "\n".join(out_lines)


def on_pre_build(config):  # noqa: ANN001 - fresh file index on every (re)build
    _course_index.clear()
