# uk-2002

Source: <https://law.di.unimi.it/webdata/uk-2002/>

- Nodes: 18,520,486
- Directed arcs: 298,113,762
- `uk-2002.graph` and `uk-2002.properties`: original LAW/WebGraph files,
  downloaded by `scripts/prepare_data.sh` and excluded from the submission
- `uk-2002.txt`: reproducible streamed text edge list, not retained

The preparation script verifies the source files. Their publisher MD5 values
are:

```text
f237aad6d4df46392e6bc25a9c7f58d9  uk-2002.graph
d1964ecbb84b8b6db4da790189f55c4b  uk-2002.properties
```

The text export can be produced by `tools/ConvertWebGraphToTxt.java` using
WebGraph 3.6.12. The verified conversion took 13.9 seconds locally and emitted
exactly 298,113,762 arcs. Its 298,113,765 lines consist of three comment/header
lines followed by all arcs.

SHA-256 of the text export:

```text
8f8753b20e26cef04710936ed9a6c8c1c23ed4b37e77e3e76cb5aeeaa295386e  uk-2002.txt
```
