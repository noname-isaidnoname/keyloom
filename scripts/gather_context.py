#!/usr/bin/env python3
"""Gather context from AGENTS.md, the docs/ directory, and the codebase.

Given a query/topic via arguments, searches AGENTS.md, documentation files
in the docs/ directory, and source code files in the project, returning
nicely-formatted results showing where the topic was found.

Codebase discovery uses `git ls-files` when available so gitignored files
and directories are skipped; otherwise it falls back to a filesystem walk
with the built-in exclusion list.
"""

import argparse
import fnmatch
import glob
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

DEFAULT_MAX_FILE_SIZE = 1024 * 1024  # 1 MiB: skip oversized/generated files by default

DEFAULT_CODEBASE_INCLUDE = "*.hpp,*.h,*.cpp,*.cc,.c"


def find_doc_files(include_pattern="*.md"):
    """Find all documentation files under the docs/ directory matching include_pattern."""
    base = Path(__file__).resolve().parent.parent / "docs"
    docs = []
    if base.exists():
        for p in sorted(base.rglob(include_pattern)):
            if p.is_file():
                docs.append(p)
    return docs


# Files and directories to exclude when searching the codebase
CODEBASE_EXCLUDE_DIRS = {
    "build",
    ".git",
    ".venv",
    "venv",
    "env",
    "local_macros",
}

# Patterns matched against the relative path (directory prefixes containing '/')
# or the bare file name (filename globs without '/').
CODEBASE_EXCLUDE_PATTERNS = {
    "*.o",
    "*.a",
    "*.so",
    "*.so.*",
    "*.elf",
    "*.exe",
    "*.pkg.tar.*",
    "*.deb",
    "*.rpm",
    "*.AppImage",
    "loomwright-*-x86_64.tar.gz",
    "loomwright-*-aarch64.tar.gz",
    "packaging/arch/pkg",
    "packaging/arch/src",
}


def should_exclude_path(p: Path, base: Path) -> bool:
    """Check if a path should be excluded from codebase search."""
    try:
        rel = p.relative_to(base)
    except ValueError:
        rel = p
    # Exclude by directory name anywhere in the path
    for part in rel.parts:
        if part in CODEBASE_EXCLUDE_DIRS:
            return True
    rel_str = str(rel)
    for pattern in CODEBASE_EXCLUDE_PATTERNS:
        if "/" in pattern:
            # Directory-prefix pattern: match the path itself or anything beneath it
            if rel_str == pattern or rel_str.startswith(pattern + "/"):
                return True
        else:
            # Filename glob
            if fnmatch.fnmatch(p.name, pattern):
                return True
    return False


def normalize_exts(include_pattern):
    """Turn a comma-separated include pattern into a set of lower-cased extensions.

    Accepts bare suffixes (`.c`, `.hpp`) and globs (`*.hpp`).
    """
    exts = set()
    for chunk in include_pattern.split(","):
        chunk = chunk.strip().lower()
        if not chunk:
            continue
        if chunk.startswith("*."):
            chunk = chunk[1:]  # '*.hpp' -> '.hpp'
        exts.add(chunk)
    return exts


def matches_ext(p: Path, exts) -> bool:
    """Return True if file extension matches the include set.

    Files without an extension are always included (e.g. Makefile, PKGBUILD).
    """
    ext = p.suffix.lower()
    return not ext or ext in exts


def git_files(base: Path):
    """List tracked and untracked-but-not-gitignored files via git.

    Returns absolute Path objects, or None if git is unavailable/fails.
    """
    try:
        out = subprocess.run(
            ["git", "ls-files", "-z", "-c", "-o", "--exclude-standard"],
            cwd=base,
            capture_output=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    files = []
    for chunk in out.split(b"\0"):
        if not chunk:
            continue
        p = base / chunk.decode("utf-8", errors="surrogateescape")
        if p.is_file():
            files.append(p)
    return files


def walk_files(base: Path):
    """Fallback filesystem walk with exclusion-list pruning."""
    files = []

    def onerror(_err):
        pass  # skip unreadable directories

    for dirpath, dirnames, filenames in os.walk(base, onerror=onerror):
        dirnames[:] = [d for d in dirnames if d not in CODEBASE_EXCLUDE_DIRS]
        for name in filenames:
            files.append(Path(dirpath) / name)
    return files


def find_codebase_files(include_pattern=DEFAULT_CODEBASE_INCLUDE):
    """Find all source code files under the project root, excluding build artifacts.

    Prefers `git ls-files` so gitignored files are skipped; falls back to a
    filesystem walk with the built-in exclusion list otherwise.
    Returns absolute Path objects, sorted for deterministic output.
    """
    base = Path(__file__).resolve().parent.parent
    exts = normalize_exts(include_pattern)
    candidates = git_files(base)
    if candidates is None:
        candidates = walk_files(base)
    files = [
        p for p in candidates
        if not should_exclude_path(p, base) and matches_ext(p, exts)
    ]
    return sorted(files)


def file_size(p: Path) -> int:
    """Size of p in bytes, or -1 if it cannot be stat'ed."""
    try:
        return p.stat().st_size
    except OSError:
        return -1


def scan_file(filepath, query, case_sensitive=False):
    """Read a file and return (matching line numbers (1-indexed), lines, error).

    ``lines`` is the full line list (empty on read error); matching line numbers
    are sorted ascending. Context expansion and merging happen downstream so each
    source line is emitted at most once per file.
    """
    try:
        content = filepath.read_text(encoding="utf-8")
    except Exception as e:
        return [], [], str(e)
    lines = content.splitlines()
    if case_sensitive:
        q = query
        matches = [i for i, line in enumerate(lines, start=1) if q in line]
    else:
        q = query.lower()
        matches = [i for i, line in enumerate(lines, start=1) if q in line.lower()]
    return matches, lines, None


def context_ranges(match_lines, total_lines, context_lines):
    """Merge overlapping/adjacent match context windows into (start, end) ranges.

    Ranges are 1-indexed and inclusive. Overlapping or directly adjacent windows
    are coalesced so every source line is printed at most once per file.
    """
    if not match_lines:
        return []
    windows = []
    for ln in match_lines:
        start = max(1, ln - context_lines)
        end = min(total_lines, ln + context_lines)
        windows.append((start, end))
    windows.sort()
    merged = []
    cur_start, cur_end = windows[0]
    for start, end in windows[1:]:
        if start <= cur_end + 1:  # overlapping or adjacent -> coalesce
            if end > cur_end:
                cur_end = end
        else:
            merged.append((cur_start, cur_end))
            cur_start, cur_end = start, end
    merged.append((cur_start, cur_end))
    return merged


def search_parallel(files, query, case_sensitive=False, max_workers=None,
                    max_results=None, max_file_size=DEFAULT_MAX_FILE_SIZE):
    """Search multiple files in parallel using ThreadPoolExecutor.

    Returns a dict mapping absolute filepath -> (matches, lines, error) for files
    that have matches or errors. Capped at ``max_results`` files when provided;
    pending work is cancelled once the cap is reached.
    """
    results = {}
    if not files:
        return results
    workers = max_workers or min(32, (os.cpu_count() or 1) + 4)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_to_file = {}
        for filepath in files:
            size = file_size(filepath)
            if max_file_size and size > max_file_size:
                continue
            future = executor.submit(scan_file, filepath, query, case_sensitive)
            future_to_file[future] = filepath

        for future in as_completed(future_to_file):
            filepath = future_to_file[future]
            try:
                matches, lines, error = future.result()
                if matches or error:
                    results[str(filepath)] = (matches, lines, error)
            except Exception as e:
                results[str(filepath)] = ([], [], str(e))
            if max_results is not None and len(results) >= max_results:
                for pending in future_to_file:
                    if not pending.done():
                        pending.cancel()
                break

    return results


def resolve_file_patterns(patterns, base):
    """Expand --file patterns into a sorted, de-duplicated list of files.

    Patterns are globs relative to the project root by default; absolute
    paths and ~-prefixed patterns are also accepted.
    """
    resolved = set()
    for pattern in patterns:
        expanded = os.path.expanduser(pattern)
        if os.path.isabs(expanded):
            matches = glob.glob(expanded, recursive=True)
        else:
            matches = glob.glob(str(base / expanded), recursive=True)
        for m in matches:
            p = Path(m)
            if p.is_file():
                resolved.add(p)
    return sorted(resolved)


def main():
    parser = argparse.ArgumentParser(
        description="Gather context from AGENTS.md, docs/, and the codebase."
    )
    parser.add_argument("query", help="The query or topic to search for")
    parser.add_argument(
        "--context",
        type=int,
        default=2,
        help="Number of surrounding lines to show per match (default: 2)",
    )
    parser.add_argument(
        "--include",
        default="*.md",
        help="File pattern to match when searching docs/ (default: *.md)",
    )
    parser.add_argument(
        "--case-sensitive",
        action="store_true",
        help="Perform case-sensitive search",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output results in JSON format",
    )
    parser.add_argument(
        "--files-only",
        "-l",
        action="store_true",
        help="Print only matching file paths, no context lines",
    )
    parser.add_argument(
        "--max-results",
        type=int,
        default=None,
        help="Maximum number of matching files to report (default: unlimited)",
    )
    parser.add_argument(
        "--max-file-size",
        type=int,
        default=DEFAULT_MAX_FILE_SIZE,
        help="Skip files larger than N bytes (0 = unlimited; default: 1 MiB)",
    )
    parser.add_argument(
        "--file",
        default=None,
        action="append",
        nargs="+",
        metavar="PATTERN",
        help="Search in specific file(s) or glob pattern(s), relative to the "
             "project root (absolute and ~-prefixed paths also work). "
             "Overrides the default AGENTS.md/docs/codebase search. "
             "Can be specified multiple times.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=None,
        help="Maximum number of worker threads for parallel codebase search (default: auto)",
    )
    args = parser.parse_args()

    query = args.query
    if not query.strip():
        print("Error: query cannot be empty", file=sys.stderr)
        sys.exit(1)
    if args.context < 0:
        parser.error("--context must be >= 0")
    if args.max_results is not None and args.max_results < 0:
        parser.error("--max-results must be >= 0")
    if args.max_file_size < 0:
        parser.error("--max-file-size must be >= 0")
    if args.workers is not None and args.workers < 1:
        parser.error("--workers must be >= 1")

    base = Path(__file__).resolve().parent.parent
    max_file_size = args.max_file_size

    results = []  # list of (label, matches, lines, error)

    def label_for(p):
        try:
            return str(p.relative_to(base))
        except ValueError:
            return str(p)

    def consider_file(p):
        """Scan a single file and record it if it has matches or an error."""
        size = file_size(p)
        if max_file_size and size > max_file_size:
            return False
        matches, lines, error = scan_file(p, query, args.case_sensitive)
        if matches or error:
            results.append((label_for(p), matches, lines, error))
            return True
        return False

    if args.file:
        patterns = [pat for group in args.file for pat in group]
        for p in resolve_file_patterns(patterns, base):
            consider_file(p)
            if args.max_results is not None and len(results) >= args.max_results:
                break
    else:
        # Always search AGENTS.md, docs/, and codebase source files
        agents_md = base / "AGENTS.md"
        if agents_md.exists():
            consider_file(agents_md)

        for doc_file in find_doc_files(args.include):
            if args.max_results is not None and len(results) >= args.max_results:
                break
            consider_file(doc_file)

        budget = None
        if args.max_results is not None:
            budget = args.max_results - len(results)
        if budget is None or budget > 0:
            codebase_files = find_codebase_files()
            search_results = search_parallel(
                codebase_files,
                query,
                case_sensitive=args.case_sensitive,
                max_workers=args.workers,
                max_results=budget,
                max_file_size=max_file_size,
            )
            # Sort by path so output order is deterministic
            for filepath, (matches, lines, error) in sorted(search_results.items()):
                results.append((label_for(Path(filepath)), matches, lines, error))
                if budget is not None:
                    budget -= 1
                    if budget <= 0:
                        break

    # Display results
    if not results:
        if args.json:
            if args.files_only:
                print(json.dumps({"query": query, "files": [], "total": 0}))
            else:
                print(json.dumps({"query": query, "results": [],
                                  "total_matches": 0, "total_files": 0}))
        else:
            print(f"No matches found for query: '{query}'")
        sys.exit(0)

    if args.json:
        if args.files_only:
            files = [label for label, _, _, _ in results]
            print(json.dumps({"query": query, "files": files, "total": len(files)}))
        else:
            output_results = []
            total_matches = 0
            for label, matches, lines, error in results:
                match_entries = []
                for start, end in context_ranges(matches, len(lines), args.context):
                    for ln in range(start, end + 1):
                        match_entries.append({"line": ln, "content": lines[ln - 1]})
                total_matches += len(matches)
                entry = {"file": label, "match_lines": len(matches),
                         "matches": match_entries}
                if error:
                    entry["error"] = error
                output_results.append(entry)
            print(json.dumps({"query": query, "results": output_results,
                              "total_matches": total_matches,
                              "total_files": len(output_results)}))
    else:
        if args.files_only:
            for label, _, _, _ in results:
                print(label)
        else:
            print(f"=== Search results for: '{query}' ===\n")
            for label, matches, lines, error in results:
                print(f"--- {label} ---")
                if error:
                    print(f"  Error: {error}")
                else:
                    for start, end in context_ranges(matches, len(lines), args.context):
                        if start == end:
                            print(f"  Line {start}:")
                        else:
                            print(f"  Lines {start}-{end}:")
                        for ln in range(start, end + 1):
                            print(f"    {lines[ln - 1]}")
                print()

    sys.exit(0)


if __name__ == "__main__":
    main()
