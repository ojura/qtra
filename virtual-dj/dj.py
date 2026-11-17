#!/usr/bin/env python3
"""Run qtra's reusable DJ desk from this nonexclusive collection."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/party_dj'))
from desk import main

if __name__ == '__main__':
    raise SystemExit(main())
