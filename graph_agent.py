#!/usr/bin/env python3
"""Thin entry point for the graph agent harness.

All behaviour lives in the agent_harness package; this file only wires it to a
command line, the same way app/main.cpp only wires the libraries together.

    ./graph_agent.py --graph graphs/graph_draw.planned.json --plan-only
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from agent_harness.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
