#!/usr/bin/env python3
"""Accept only the single bounded, closed report; publish a fixed artifact gate."""
from pathlib import Path
import os
import sys
from run import read_json
import schema

try:
    if len(sys.argv) != 2: raise ValueError()
    read_json(Path(sys.argv[1]), schema.final, 65536)
    output=os.environ.get('GITHUB_OUTPUT')
    if output:
        with open(output,'a',encoding='ascii') as handle: handle.write('safe=true\n')
    print('SAFE_REPORT_VALIDATED')
except Exception:
    print('REPORT_REJECTED')
    raise SystemExit(1)
