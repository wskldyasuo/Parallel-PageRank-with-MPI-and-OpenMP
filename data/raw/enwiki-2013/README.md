# enwiki-2013

Source: <https://snap.stanford.edu/data/enwiki-2013.html>

- Nodes: 4,203,323
- Directed edges: 101,311,614
- `enwiki-2013.txt.gz`: original SNAP download, 335,102,744 bytes; generated
  locally by `scripts/prepare_data.sh` and not included in the submission
- `enwiki-2013.txt`: reproducible extracted edge list, not retained

The original archive has no newline between the column-name comment and its
first edge (`1 0`). The downloaded archive remains unchanged;
`scripts/normalize_enwiki2013.py` repairs the extracted copy so that all
101,311,614 edges are readable by the project loader.

SHA-256:

```text
99d251763e2834e11b7c8baf8e8dffd7de99d5d6217fcf6f3c082154b8b459ee  enwiki-2013.txt.gz
10fc66c002c5e6a566c7f78bd62366a32bafb30ef61efedcd38b43252f39c1b7  enwiki-2013.txt
```

The generated normalized text file has 101,311,618 lines: four comment/header
lines and 101,311,614 edge lines.
