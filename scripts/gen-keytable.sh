// Generate src/keys.inc from the kernel's input-event-codes header.
//
// The table is committed so a normal build needs no code generation; rerun this
// script only when the kernel UAPI header changes.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
header="${1:-/usr/include/linux/input-event-codes.h}"

python3 - "$header" "$root/src/keys.inc" <<'PY'
import re
import sys

header, destination = sys.argv[1], sys.argv[2]
source = open(header).read()
pairs = [
    (name, int(value, 0))
    for name, value in re.findall(r'^#define\s+KEY_([A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)', source, re.M)
]
if len(pairs) < 300:
    raise SystemExit(f"only {len(pairs)} KEY_ definitions found in {header}")

first_name_for_code = {}
for name, code in pairs:
    first_name_for_code.setdefault(code, name)

lines = [
    "// Generated from <linux/input-event-codes.h>. Do not edit by hand.",
    "// Regenerate with scripts/gen-keytable.sh.",
    "//",
    "// kKeyTable is sorted by name for lookup; kKeyReverse is sorted by code and",
    "// keeps the first name defined for a code, which is the canonical one",
    "// (KEY_ESC for 1, KEY_LEFTCTRL for 29, and so on).",
    "",
    "static const KeyEntry kKeyTable[] = {",
]
lines += [f'    {{"{name}", {code}}},' for name, code in sorted(pairs)]
lines += ["};", "", "static const ReverseKeyEntry kKeyReverse[] = {"]
lines += [f'    {{{code}, "{first_name_for_code[code]}"}},' for code in sorted(first_name_for_code)]
lines += ["};", ""]
open(destination, "w").write("\n".join(lines))
print(f"wrote {destination}: {len(pairs)} names, {len(first_name_for_code)} codes")
PY
