# Processed and generated graphs

Binary `.prcsr` files use the project's `PRCSR001` format:

1. fixed-width header with vertex and edge counts;
2. incoming CSR offsets (`uint64_t`);
3. incoming source IDs (`uint32_t`);
4. global out-degrees (`uint32_t`);
5. output/original IDs (`int64_t`).

Generated files are not committed. Keep their JSON sidecars and checksums with
formal experiment records.

The four generated binary files represent web-Google, web-BerkStan,
enwiki-2013, and uk-2002. They are excluded from the submission and recreated
with `scripts/prepare_data.sh`; their expected hashes are recorded in
`SHA256SUMS`.

Sparse original-ID graphs are converted with `generate_web_graph --replicas 1`
so their ID mapping is preserved. The dense zero-based uk-2002 export can use
the lower-memory streaming converter:

```bash
./build/convert_snap_to_csr \
  data/raw/uk-2002/uk-2002.txt \
  data/processed/uk-2002.prcsr
```

