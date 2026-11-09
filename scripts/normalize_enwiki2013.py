#!/usr/bin/env python3
"""Fix the missing newline in the official enwiki-2013 text header."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


BAD_HEADER = b"# FromNodeId ToNodeId1 0\n"
FIXED_HEADER = b"# FromNodeId ToNodeId\n1 0\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    args = parser.parse_args()

    source = args.path.resolve()
    partial = source.with_name(source.name + ".normalize.part")
    if partial.exists():
        raise SystemExit(f"partial output already exists: {partial}")

    replaced = False
    with source.open("rb") as input_stream, partial.open("xb") as output_stream:
        for line_number, line in enumerate(input_stream, start=1):
            if line_number == 4:
                if line != BAD_HEADER:
                    raise RuntimeError(
                        "unexpected line 4; refusing to modify a file that does not match "
                        "the official enwiki-2013 formatting defect"
                    )
                output_stream.write(FIXED_HEADER)
                replaced = True
            else:
                output_stream.write(line)

    if not replaced:
        raise RuntimeError("input ended before the expected header line")
    os.replace(partial, source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
