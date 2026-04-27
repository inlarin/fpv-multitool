#!/usr/bin/env python3
"""UI design-system quality audit — wrapper around scripts/lint_ui.py --report.

Run from project root:
    python tools/audit_ui.py

Prints per-file totals + sample violations + token coverage. Exit code is
always 0 (audit, not gate). For build-time enforcement see scripts/lint_ui.py
as the platformio.ini pre-build hook.
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from lint_ui import report  # noqa: E402

sys.exit(report())
