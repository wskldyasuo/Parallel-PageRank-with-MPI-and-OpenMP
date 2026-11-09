# Graph download manifest

Large data files are excluded from the submission. Run
`bash scripts/prepare_data.sh all` from the project root to download and process
them. Each data set has an official source and a deterministic local filename.

| Dataset | Nodes | Edges | Official page | Local raw file |
|---|---:|---:|---|---|
| web-Google | 875,713 | 5,105,039 | <https://snap.stanford.edu/data/web-Google.html> | `web-Google.txt.gz` |
| web-BerkStan | 685,230 | 7,600,595 | <https://snap.stanford.edu/data/web-BerkStan.html> | `web-BerkStan.txt.gz` |
| enwiki-2013 | 4,203,323 | 101,311,614 | <https://snap.stanford.edu/data/enwiki-2013.html> | `enwiki-2013/enwiki-2013.txt.gz` |
| uk-2002 | 18,520,486 | 298,113,762 | <https://law.di.unimi.it/webdata/uk-2002/> | `uk-2002/uk-2002.graph` and `.properties` |

Large extracted text edge lists and generated `.prcsr` files are reproducible
intermediates and are intentionally excluded. See the root README for the full
processing sequence and software requirements.

