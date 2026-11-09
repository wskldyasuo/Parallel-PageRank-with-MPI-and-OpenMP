#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$project_dir"

target=${1:-all}
keep_text=${KEEP_TEXT:-0}

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

verify_sha256() {
    local path=$1
    local expected=$2
    local actual
    actual=$(sha256_file "$path")
    if [[ "$actual" != "$expected" ]]; then
        echo "SHA-256 mismatch for $path" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

download() {
    local url=$1
    local output=$2
    if [[ -f "$output" ]]; then
        echo "Using existing $output"
        return
    fi
    mkdir -p "$(dirname "$output")"
    echo "Downloading $url"
    curl --fail --location --retry 3 --output "$output.part" "$url"
    mv "$output.part" "$output"
}

extract_gzip() {
    local archive=$1
    local output=$2
    if [[ -f "$output" ]]; then
        echo "Using existing $output"
        return
    fi
    echo "Extracting $archive"
    gzip -dc "$archive" > "$output.part"
    mv "$output.part" "$output"
}

discard_text_if_requested() {
    local path=$1
    if [[ "$keep_text" != 1 ]]; then
        rm -f "$path"
    fi
}

prepare_snap() {
    local name=$1
    local url=$2
    local archive=$3
    local archive_sha=$4
    local output=$5
    local output_sha=$6
    local normalize=${7:-0}
    local text=${archive%.gz}

    download "$url" "$archive"
    verify_sha256 "$archive" "$archive_sha"
    if [[ ! -f "$output" ]]; then
        extract_gzip "$archive" "$text"
        if [[ "$normalize" == 1 ]]; then
            python3 scripts/normalize_enwiki2013.py "$text"
            verify_sha256 "$text" \
                10fc66c002c5e6a566c7f78bd62366a32bafb30ef61efedcd38b43252f39c1b7
        fi
        echo "Converting $name to PRCSR001"
        ./build/generate_web_graph --input "$text" --replicas 1 --output "$output"
    fi
    verify_sha256 "$output" "$output_sha"
    discard_text_if_requested "$text"
}

prepare_google() {
    prepare_snap \
        web-Google \
        https://snap.stanford.edu/data/web-Google.txt.gz \
        data/raw/web-Google.txt.gz \
        bcac0af0471d749f4a8c010bca92b61cf2868a0570741de06892fc062f265ea6 \
        data/processed/web-Google.prcsr \
        e59709804cfe4a48815a36c281e25fd3021105035d92b289767d06907b91815b
}

prepare_berkstan() {
    prepare_snap \
        web-BerkStan \
        https://snap.stanford.edu/data/web-BerkStan.txt.gz \
        data/raw/web-BerkStan.txt.gz \
        760a1de92b8f5a68616254f1eafce337cc924a095c0af636a0337294a27fbb1b \
        data/processed/web-BerkStan.prcsr \
        f43fa5b3d0b730dbc5dd125f305c97e1eea34a0a6fdefa4610b4a347953d85c6
}

prepare_enwiki() {
    prepare_snap \
        enwiki-2013 \
        https://snap.stanford.edu/data/enwiki-2013.txt.gz \
        data/raw/enwiki-2013/enwiki-2013.txt.gz \
        99d251763e2834e11b7c8baf8e8dffd7de99d5d6217fcf6f3c082154b8b459ee \
        data/processed/enwiki-2013.prcsr \
        0f13f64c688276a3291c307cc516a2fb48a588e2f28f9f85033f49aa3cc5c937 \
        1
}

prepare_uk() {
    local graph=data/raw/uk-2002/uk-2002.graph
    local properties=data/raw/uk-2002/uk-2002.properties
    local text=data/raw/uk-2002/uk-2002.txt
    local output=data/processed/uk-2002.prcsr
    local classpath_file="$project_dir/build/webgraph.classpath"
    local classes="$project_dir/build/webgraph-classes"

    download \
        https://data.law.di.unimi.it/webdata/uk-2002/uk-2002.graph \
        "$graph"
    download \
        https://data.law.di.unimi.it/webdata/uk-2002/uk-2002.properties \
        "$properties"
    verify_sha256 "$graph" \
        516928e3ca136a6e0d5d71ba3e1441a21ef76285ac66257ce8dc237f905bc34f
    verify_sha256 "$properties" \
        dcd65e4579ea67b3d8692083cdd1ed513a4fa543406d49bf5849c9250777b6fb

    if [[ ! -f "$output" ]]; then
        if [[ ! -f "$text" ]]; then
            command -v mvn >/dev/null || {
                echo "Maven is required to convert uk-2002." >&2
                exit 1
            }
            mkdir -p "$classes"
            mvn -q -f tools/webgraph-pom.xml dependency:build-classpath \
                -Dmdep.outputFile="$classpath_file"
            javac -cp "$(cat "$classpath_file")" -d "$classes" \
                tools/ConvertWebGraphToTxt.java
            java -cp "$classes:$(cat "$classpath_file")" \
                ConvertWebGraphToTxt \
                data/raw/uk-2002/uk-2002 \
                "$text"
        fi
        verify_sha256 "$text" \
            8f8753b20e26cef04710936ed9a6c8c1c23ed4b37e77e3e76cb5aeeaa295386e
        echo "Converting uk-2002 to PRCSR001"
        ./build/convert_snap_to_csr "$text" "$output"
    fi
    verify_sha256 "$output" \
        6e27cff2a336607610b381f5793b7a8aea6ca1eb866afb4b3b0b694ba174883b
    discard_text_if_requested "$text"
}

case "$target" in
    google|web-Google) selected=(google) ;;
    berkstan|web-BerkStan) selected=(berkstan) ;;
    enwiki|enwiki-2013) selected=(enwiki) ;;
    uk|uk-2002) selected=(uk) ;;
    all) selected=(google berkstan enwiki uk) ;;
    *)
        echo "Usage: $0 [all|google|berkstan|enwiki|uk]" >&2
        exit 2
        ;;
esac

command -v curl >/dev/null || { echo "curl is required." >&2; exit 1; }
command -v gzip >/dev/null || { echo "gzip is required." >&2; exit 1; }
mkdir -p data/raw/enwiki-2013 data/raw/uk-2002 data/processed
make -j"${BUILD_JOBS:-2}" build/generate_web_graph build/convert_snap_to_csr

for dataset in "${selected[@]}"; do
    "prepare_$dataset"
done

echo "Data preparation complete. KEEP_TEXT=1 preserves extracted text files."
