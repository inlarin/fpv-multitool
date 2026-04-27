#!/usr/bin/env python3
"""UI design-system enforcement.

Two modes:
  * Pre-build hook (called by platformio.ini) -- fails build on violations
    in data/index.html + data/tabs/*.html. Set LINT_UI_WARN_ONLY=1 in env
    to demote failures to warnings during legacy-tab migration.
  * Standalone audit -- `python scripts/lint_ui.py --report` prints a
    full quality summary (totals per file, no exit code).

Why these specific rules -- see docs/UI.md "Anti-patterns".
"""
import os
import re
import sys

# ---------- Configuration ---------------------------------------------------

# PIO SCons exec()-s this file without setting __file__, so detect path
# from the env it injects. Standalone CLI gets __file__ normally.
if "__file__" in globals():
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
else:
    PROJECT_DIR = os.getcwd()  # PIO runs from project root

# Files we lint. Glob handled manually to avoid pulling pathlib.
HTML_TARGETS = [
    "data/index.html",
] + sorted(
    f"data/tabs/{name}"
    for name in os.listdir(os.path.join(PROJECT_DIR, "data", "tabs"))
    if name.endswith(".html") and name != "_styleguide.html"  # styleguide allowed inline
)

CSS_FILE = "data/style.css"

# Files exempt from rules -- playground / styleguide / generated.
EXEMPT_FILES = {
    "data/tabs/_styleguide.html",  # intentional showcase, embeds raw markup samples
}

# ---------- Rules -----------------------------------------------------------

# Inline-style -- banned except for `display:none` (initial tab hide is the only
# legitimate use; should ideally be a class but is convention from v1).
INLINE_STYLE_RE = re.compile(r'\bstyle="([^"]+)"')
INLINE_STYLE_OK = re.compile(r'^\s*display\s*:\s*none\s*;?\s*$')

# Hardcoded color -- any `#rgb` or `#rrggbb` outside the design system.
# Excludes colors inside SVG `data:` URIs (favicon) and CSS comments.
HEX_COLOR_RE = re.compile(r'#[0-9a-fA-F]{3,6}\b')

# Forbidden tags in tab HTML -- should use .card-title / .page-section-title.
FORBIDDEN_TAG_RE = re.compile(r'<(h[123])\b', re.IGNORECASE)

# Legacy classes -- warn-only, nudge toward .kv-*.
LEGACY_CLASS_RE = re.compile(r'class="[^"]*\b(?:row|label|value)\b[^"]*"')

# Token references -- `var(--xxx)`. Used by undefined-token rule.
VAR_REF_RE = re.compile(r'var\((--[a-zA-Z0-9_-]+)\)')

# ---------- Helpers ---------------------------------------------------------

class Violation:
    __slots__ = ("rule", "path", "line", "snippet", "hint")
    def __init__(self, rule, path, line, snippet, hint=""):
        self.rule = rule
        self.path = path
        self.line = line
        self.snippet = snippet[:120]  # cap for readability
        self.hint = hint
    def __repr__(self):
        return f"{self.path}:{self.line:<4} {self.rule:<18} {self.snippet}"


def read(rel):
    p = os.path.join(PROJECT_DIR, rel)
    with open(p, "r", encoding="utf-8") as f:
        return f.read().splitlines()


def collect_defined_tokens():
    """Parse data/style.css :root + dark-theme block to learn which tokens exist."""
    tokens = set()
    in_block = False
    for line in read(CSS_FILE):
        s = line.strip()
        if s.startswith(":root") or s.startswith("[data-theme"):
            in_block = True
            continue
        if in_block:
            if s.startswith("}"):
                in_block = False
                continue
            m = re.match(r"(--[a-zA-Z0-9_-]+)\s*:", s)
            if m:
                tokens.add(m.group(1))
    return tokens


# ---------- Rule implementations -------------------------------------------

def check_html(path, lines, defined_tokens):
    out = []
    if path in EXEMPT_FILES:
        return out
    for i, line in enumerate(lines, start=1):
        # 1. inline-style
        for m in INLINE_STYLE_RE.finditer(line):
            content = m.group(1)
            if INLINE_STYLE_OK.match(content):
                continue
            out.append(Violation(
                "inline-style", path, i, m.group(0),
                "use a class -- see docs/UI.md, or extend data/style.css",
            ))
        # 2. hardcoded-color (skip SVG data URIs and inline `display:none` lines --
        # the favicon SVG includes %23xxxxxx URL-encoded colors that look like hex)
        if "data:image/svg" not in line:
            for m in HEX_COLOR_RE.finditer(line):
                # don't flag CSS color in linked SVG icons
                snippet = line.strip()
                out.append(Violation(
                    "hardcoded-color", path, i, m.group(0),
                    "use var(--…) -- see Tokens section in docs/UI.md",
                ))
        # 3. forbidden-tag (h1/h2/h3 outside skeleton index.html -- those are
        # allowed in nav header)
        if path != "data/index.html":
            for m in FORBIDDEN_TAG_RE.finditer(line):
                out.append(Violation(
                    "forbidden-tag", path, i, m.group(0),
                    "use .card-title / .page-section-title",
                ))
        # 4. legacy-class -- warn only, don't fail
        # (handled outside, in audit summary)
        # 5. undefined-token
        for m in VAR_REF_RE.finditer(line):
            tok = m.group(1)
            if tok not in defined_tokens:
                out.append(Violation(
                    "undefined-token", path, i, m.group(0),
                    f"token {tok} not defined in data/style.css :root",
                ))
    return out


def check_css(path, lines, defined_tokens):
    out = []
    for i, line in enumerate(lines, start=1):
        # Undefined token references in CSS (a custom prop using var(--missing))
        for m in VAR_REF_RE.finditer(line):
            tok = m.group(1)
            if tok not in defined_tokens:
                out.append(Violation(
                    "undefined-token", path, i, m.group(0),
                    f"token {tok} not defined in :root",
                ))
    return out


def collect_legacy(path, lines):
    """Counts only -- no fail. Reported in audit summary."""
    count = 0
    if path in EXEMPT_FILES:
        return 0
    for line in lines:
        if LEGACY_CLASS_RE.search(line):
            count += 1
    return count


# ---------- Top-level runners ----------------------------------------------

def lint_all():
    defined = collect_defined_tokens()
    violations = []
    for rel in HTML_TARGETS:
        try:
            lines = read(rel)
        except FileNotFoundError:
            continue
        violations.extend(check_html(rel, lines, defined))
    try:
        violations.extend(check_css(CSS_FILE, read(CSS_FILE), defined))
    except FileNotFoundError:
        pass
    return violations, defined


def report():
    """Standalone audit -- prints summary, returns 0."""
    violations, defined = lint_all()
    by_file = {}
    by_rule = {}
    for v in violations:
        by_file.setdefault(v.path, []).append(v)
        by_rule[v.rule] = by_rule.get(v.rule, 0) + 1

    legacy_per_file = {}
    for rel in HTML_TARGETS:
        try:
            legacy_per_file[rel] = collect_legacy(rel, read(rel))
        except FileNotFoundError:
            pass

    print("=" * 64)
    print("UI design-system audit")
    print("=" * 64)
    print(f"Tokens defined: {len(defined)}")
    print(f"Files scanned:  {len(HTML_TARGETS)} HTML + 1 CSS")
    print()
    print("Per-file (strict rules -- must be 0 to ship):")
    print(f"  {'file':<32} {'inline':>6} {'hex':>5} {'tag':>5} {'tok':>5} {'legacy':>7}")
    for rel in HTML_TARGETS + [CSS_FILE]:
        vs = by_file.get(rel, [])
        c_inline = sum(1 for v in vs if v.rule == "inline-style")
        c_hex    = sum(1 for v in vs if v.rule == "hardcoded-color")
        c_tag    = sum(1 for v in vs if v.rule == "forbidden-tag")
        c_tok    = sum(1 for v in vs if v.rule == "undefined-token")
        c_leg    = legacy_per_file.get(rel, 0) if rel.endswith(".html") else 0
        flag = "OK" if (c_inline + c_hex + c_tag + c_tok) == 0 else "!!"
        print(f"  {flag} {rel:<30} {c_inline:>6} {c_hex:>5} {c_tag:>5} {c_tok:>5} {c_leg:>7}")
    print()
    print("Totals:")
    for rule in ("inline-style", "hardcoded-color", "forbidden-tag", "undefined-token"):
        print(f"  {rule:<18} {by_rule.get(rule, 0)}")
    print(f"  legacy-class       {sum(legacy_per_file.values())}  (warn-only)")
    print()
    if violations:
        print("Sample violations (first 10):")
        for v in violations[:10]:
            print(f"  {v}")
            if v.hint:
                print(f"      -> {v.hint}")
    else:
        print("Clean.")
    return 0


def hook(strict):
    """Pre-build hook entry -- prints violations, exits non-zero if strict and any."""
    violations, _ = lint_all()
    if not violations:
        print("[lint_ui] clean")
        return 0
    print(f"[lint_ui] found {len(violations)} violation(s):")
    for v in violations[:30]:  # cap output
        print(f"  {v}")
        if v.hint:
            print(f"      -> {v.hint}")
    if len(violations) > 30:
        print(f"  ... and {len(violations) - 30} more (run `python scripts/lint_ui.py --report`)")
    if strict:
        print("[lint_ui] BUILD FAILED -- fix violations (or unset LINT_UI_STRICT to demote)")
        return 1
    print("[lint_ui] WARN-ONLY -- migration in progress, set LINT_UI_STRICT=1 to fail")
    return 0


# ---------- Entry points ---------------------------------------------------

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--report":
        sys.exit(report())
    # Default: warn-only during the v1->v2 design-system migration. Set
    # LINT_UI_STRICT=1 once all tabs are migrated (`tools/audit_ui.py` reports
    # 0 violations across all data/tabs/*.html) to flip to fail-on-violation.
    strict = os.environ.get("LINT_UI_STRICT", "0") == "1"
    sys.exit(hook(strict))


# PlatformIO pre-build hook entry -- `Import("env")` is injected by SCons.
try:
    Import("env")  # noqa: F821 -- provided by PlatformIO build env
    # Default: warn-only during the v1->v2 design-system migration. Set
    # LINT_UI_STRICT=1 once all tabs are migrated (`tools/audit_ui.py` reports
    # 0 violations across all data/tabs/*.html) to flip to fail-on-violation.
    strict = os.environ.get("LINT_UI_STRICT", "0") == "1"
    rc = hook(strict)
    if rc != 0:
        env.Exit(rc)  # noqa: F821
except NameError:
    # Standalone CLI invocation
    if __name__ == "__main__":
        main()
