# Gathering context

You can use the `scripts/gather_context.py` script to search through AGENTS.md, the docs/ directory, and the codebase (C++ sources and extensionless files; gitignored files are skipped):

```bash
python3 scripts/gather_context.py "<query>"
```


Options:
- `--context N`: Number of surrounding lines to show per match (default: 2)
- `--include PATTERN`: File pattern to match when searching docs/ (default: *.md); codebase extensions are not affected
- `--case-sensitive`: Perform case-sensitive search
- `--json`: Output results in JSON format (use this for special cases where you need to parse the results programmatically)
- `--files-only` / `-l`: Print only matching file paths, no context lines (biggest token saver)
- `--max-results N`: Maximum number of matching files to report (default: unlimited)
- `--max-file-size N`: Skip files larger than N bytes (0 = unlimited; default: 1 MiB)
- `--file PATTERN [PATTERN ...]`: Search only the given file(s)/glob pattern(s), relative to the project root (absolute and `~`-prefixed paths also work). Overrides the default AGENTS.md/docs/codebase search. Can be specified multiple times
- `--workers N`: Maximum number of worker threads for parallel codebase search (default: auto)