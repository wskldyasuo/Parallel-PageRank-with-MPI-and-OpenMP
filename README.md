# DSA5208 Project 3: Parallel PageRank

This repository contains three PageRank implementations over one shared CSR
graph representation:

1. serial C++;
2. MPI-only;
3. MPI + OpenMP.

All modes use the same graph loader, duplicate-edge policy, original-ID
mapping, dangling-node treatment, convergence rule, and output format. Both
SNAP edge lists and the project's binary `.prcsr` format are supported.

## Repository layout

```text
include/pagerank/       Shared CSR, serial, and distributed interfaces
src/                    Serial, MPI, hybrid, CLI, and graph-generator code
tests/                  Deterministic correctness inputs and checks
scripts/                Reproducible experiment scripts
validation/             Independent NumPy/NetworkX validation
data/                   Download manifests, hashes, and CSR metadata
results/                Local and Vanda experiment records
00_...ipynb - 05_...    Ordered environment, scaling, and extension notebooks
report/                 Editable English report draft
output/pdf/             Generated report PDF
```

## Submission contents

The final report is `output/pdf/DSA5208_Project3_Yan_Wan_Wang.pdf`. It covers
all required report elements:

| Requirement | Report location |
|---|---|
| PageRank algorithm introduction | Section 1 |
| Parallelization design decisions | Section 2 and Section 6.2 |
| Parallel scaling and efficiency | Sections 5.2-5.7 |
| Correctness validation | Section 4 |
| Runtime figures from Vanda | Figures 1-11 in Sections 5-6 |
| Conclusions about the real graph data | Section 6.1 and Section 7 |
| AI-use explanation | AI Use Declaration |

The C++ source is under `src/` and `include/`; the commands below build, test,
run, and reproduce the submitted analysis without relying on a fixed local
path.

## Reproduce on another computer

The project is location-independent: run commands from the extracted project
root, and do not rename or separate its internal directories. Native macOS and
Linux are supported. On Windows, use WSL2 with the Linux instructions.

Install the system toolchain first.

macOS with Homebrew:

```bash
brew install python open-mpi libomp curl
```

Ubuntu 24.04 or another Debian-based system with Python 3.12 or newer:

```bash
sudo apt update
sudo apt install build-essential openmpi-bin libopenmpi-dev python3 python3-venv curl
```

Then create the isolated Python environment:

```bash
bash scripts/setup_environment.sh
```

The submission intentionally excludes the multi-gigabyte graph files. Download
and convert all four official data sets before executing the environment/data
notebook:

```bash
bash scripts/prepare_data.sh all
```

The three SNAP archives require about 360 MiB of downloads; the two required
UK-2002 WebGraph files add about 81 MiB. The generated `.prcsr` files occupy
about 2.0 GiB. Temporary extracted text files require additional space but are
deleted after successful conversion. Set `KEEP_TEXT=1` to retain them.

To compile, run all correctness tests, execute all six notebooks in place,
regenerate every figure, and rebuild the final PDF:

```bash
bash scripts/reproduce_all.sh
```

The notebooks analyse the saved Vanda CSV files included under `results/`.
They do not require access to the original Vanda account and do not resubmit
cluster jobs. Re-running the HPC timing measurements themselves requires a PBS
cluster compatible with the scripts under `scripts/vanda/`; those scripts use
the supplied project directory rather than a user-specific absolute path.
Numerical results and saved plots are reproducible from the included records;
new performance timings will naturally vary with hardware, MPI implementation,
system load, and scheduler placement.

## Download and prepare the graph data

The large graph files are not included in the submission. The preparation
script downloads only the files required by the project, verifies their
SHA-256 hashes, and writes the binary files expected under `data/processed/`.

| Data set | Official page | Direct download used by the script |
|---|---|---|
| web-Google | [SNAP data page](https://snap.stanford.edu/data/web-Google.html) | [`web-Google.txt.gz`](https://snap.stanford.edu/data/web-Google.txt.gz) |
| web-BerkStan | [SNAP data page](https://snap.stanford.edu/data/web-BerkStan.html) | [`web-BerkStan.txt.gz`](https://snap.stanford.edu/data/web-BerkStan.txt.gz) |
| enwiki-2013 | [SNAP data page](https://snap.stanford.edu/data/enwiki-2013.html) | [`enwiki-2013.txt.gz`](https://snap.stanford.edu/data/enwiki-2013.txt.gz) |
| uk-2002 | [LAW data page](https://law.di.unimi.it/webdata/uk-2002/) | [`uk-2002.graph`](https://data.law.di.unimi.it/webdata/uk-2002/uk-2002.graph) and [`uk-2002.properties`](https://data.law.di.unimi.it/webdata/uk-2002/uk-2002.properties) |

Prepare one graph when a full download is unnecessary:

```bash
bash scripts/prepare_data.sh google
bash scripts/prepare_data.sh berkstan
bash scripts/prepare_data.sh enwiki
bash scripts/prepare_data.sh uk
```

Raw-data processing is deterministic:

1. web-Google and web-BerkStan are decompressed and passed to
   `build/generate_web_graph --replicas 1`. This loads their sparse original
   vertex IDs, deduplicates edges, maps IDs densely, and writes incoming CSR.
2. The official enwiki archive has a missing newline between its fourth header
   line and first edge. `scripts/normalize_enwiki2013.py` repairs that exact
   byte sequence before the same CSR generator is used.
3. UK-2002 is distributed in compressed LAW/WebGraph format. The preparation
   script uses `tools/ConvertWebGraphToTxt.java` with WebGraph 3.6.12 to stream
   it into a dense zero-based edge list, then uses
   `build/convert_snap_to_csr` for the lower-memory CSR conversion.
4. Every archive and final `.prcsr` file is checked against the hashes under
   `data/raw/` and `data/processed/`. Large extracted text files are disposable
   intermediates.

Preparing UK-2002 additionally requires Java and Maven:

```bash
# macOS
brew install openjdk maven

# Ubuntu/Debian
sudo apt install default-jdk maven
```

## Build

Requirements: a C++17 compiler, MPI with `mpicxx`/`mpirun`, OpenMP, and make.

```bash
make -j
```

The build creates:

- `build/pagerank`: unified `serial|mpi|hybrid` program;
- `build/generate_web_graph`: mapped-ID binary CSR generator;
- `build/convert_snap_to_csr`: low-memory converter for dense zero-based SNAP
  files such as uk-2002.

On macOS, the Makefile selects the C++ compiler behind Open MPI and links
Homebrew `libomp`. Override paths when needed, for example:

```bash
make CXX=/usr/bin/clang++ LIBOMP=/opt/homebrew/opt/libomp
```

On Atlas/Vanda, load the course compiler and MPI modules first. Linux uses
`-fopenmp` by default.

## Correctness tests

```bash
make test
make test-parallel
make validate PYTHON=.venv/bin/python
```

`test-parallel` checks all three modes against the serial CSR implementation.
It includes one-rank/multi-thread Hybrid execution, full-vector and
demand-driven exchange, both partition policies, more ranks than vertices,
explicit dangling vertices, and isolated vertices declared in a SNAP header.

## Run the three implementations

Serial:

```bash
./build/pagerank \
  --input data/processed/web-Google.prcsr \
  --mode serial
```

MPI-only:

```bash
mpirun --bind-to none -np 4 ./build/pagerank \
  --input data/processed/web-Google.prcsr \
  --mode mpi --verify
```

MPI + OpenMP:

```bash
mpirun --bind-to none -np 2 ./build/pagerank \
  --input data/processed/web-Google.prcsr \
  --mode hybrid --threads 2 --verify
```

`--bind-to none` is convenient for local smoke tests. Use the binding and
mapping policy required by the cluster scheduler for reported experiments.

Common options:

```text
--input-format auto|snap|csr
--partition vertices|in-edges
--score-exchange full|demand
--alpha VALUE
--tolerance VALUE          (alias: --tol)
--max-iterations N
--top-k N
--output FILE
--metrics FILE
--run-id VALUE
--verify
```

`--verify` computes the serial CSR reference before distributing the graph.
Do not include verification runs in performance measurements.

## Distributed design

- Rank 0 loads and validates the CSR graph.
- Target vertices are assigned in contiguous ceil-sized blocks; empty ranks
  are valid when the process count exceeds the vertex count.
- `--partition in-edges` selects the contiguous in-edge-work partition, which
  balances the estimated work `1 + in-degree`; the default `vertices` policy
  uses equal numbers of target vertices.
- Each rank stores only the incoming CSR slice for its target block, plus the
  global out-degree array needed by pull updates.
- `--score-exchange full` retains the baseline complete score vector and uses
  `MPI_Allgatherv` after every update.
- `--score-exchange demand` deduplicates remote source dependencies once,
  exchanges requested vertex IDs to construct a fixed communication plan, and
  uses `MPI_Alltoallv` to exchange only those score values each iteration.
  Each rank stores local scores plus the remote scores required by its incoming
  CSR slice.
- Both exchange modes combine dangling mass and the global L1 change with
  `MPI_Allreduce` each iteration.
- Hybrid mode applies OpenMP to dangling, pull-update, and L1 loops. MPI calls
  remain outside parallel regions and use `MPI_THREAD_FUNNELED`.
- The timed solve excludes file loading, distribution, optional serial
  verification, output, and validation.

Dependency construction is timed separately and excluded from solve time.
Metrics distinguish solve, score-packing, and score-exchange time; values and
bytes exchanged per iteration; exchange-related state; and maximum per-rank
peak RSS. Complete
scores are gathered to rank 0 after the timed solve only when output, top-k, or
serial verification is requested.

The MPI distribution uses standard `int` counts, so the active distributed
baseline requires both the vertex count and edge count to be at most
`INT_MAX`.

## Binary CSR and large graphs

The same three modes auto-detect `.prcsr` input:

```bash
./build/pagerank \
  --input data/processed/web-Google.prcsr \
  --mode serial

mpirun -np 4 ./build/pagerank \
  --input data/processed/web-Google.prcsr \
  --mode mpi
```

Create a binary CSR copy of the Google graph with:

```bash
./build/generate_web_graph \
  --input data/raw/web-Google.txt \
  --replicas 1 \
  --output data/processed/web-Google.prcsr
```

## Repeated local experiment

Run all modes repeatedly and append a common metrics CSV:

```bash
./scripts/run_all_modes.sh data/processed/web-Google.prcsr 5 4 2 2
```

Arguments are dataset, repetitions, MPI ranks, hybrid ranks, and threads per
hybrid rank. The script performs one unrecorded warm-up per mode by default;
set `WARMUPS` to change it. Keep total resources comparable when discussing
speedup and efficiency.

## Re-run the complete Vanda experiment campaign

The checked-in CSV files are sufficient to reproduce the submitted figures.
This section is only for collecting a new set of timings on Vanda. A complete
rerun is scheduler- and allocation-intensive: it submits 89 measurement jobs,
one build/test job, and one weak-data preparation job. Every configuration has
one unrecorded warm-up and five measured repetitions.

### 1. Create a clean remote project

Connect to the NUS VPN when off campus. From the local project root, choose a
new remote directory so that historical CSV files cannot be mixed with the new
campaign:

```bash
export VANDA_REMOTE='<nusnet-id>@vanda.nus.edu.sg'
export VANDA_PROJECT='DSA5208_Project3_rerun'

rsync -av \
  --exclude '.venv/' \
  --exclude 'build/' \
  --exclude 'results/vanda/' \
  --exclude 'output/' \
  ./ "$VANDA_REMOTE:$VANDA_PROJECT/"
```

Do not reuse a remote directory containing previous `results/vanda/**/*.csv`
files. Most submit scripts rewrite their manifest but do not remove old CSVs.

The large graph files are not part of the submission. Prepare them locally,
then transfer only the four binary CSR files:

```bash
bash scripts/prepare_data.sh all
ssh "$VANDA_REMOTE" "mkdir -p '$VANDA_PROJECT/data/processed'"
rsync -av data/processed/*.prcsr \
  "$VANDA_REMOTE:$VANDA_PROJECT/data/processed/"
```

The JSON metadata and expected hashes are already included in the source
upload. Alternatively, `prepare_data.sh` can be run on Vanda only if the login
environment has outbound download access, Java, and Maven.

### 2. Build and test on Vanda

Log in, enter the new directory, and submit the build job. All PBS scripts load
the same `openmpi/gcc/64/4.1.8` module used for the reported results.

```bash
ssh "$VANDA_REMOTE"
export VANDA_PROJECT='DSA5208_Project3_rerun'
cd "$HOME/$VANDA_PROJECT"

build_job=$(qsub -v "PROJECT_DIR=$PWD" scripts/vanda/build_vanda.pbs)
echo "$build_job"
qstat -u "$USER"
```

Wait for the build job to finish successfully before submitting the experiment
suites. Verify its final state and exit status with:

```bash
qstat -x -f "$build_job" | egrep 'job_state|Exit_status|Output_Path'
```

`Exit_status = 0` is required. The build job compiles the C++ programs and runs
both `make test` and `make test-parallel`.

### 3. Submit the formal experiment suites

Run the following commands from the remote project root. The first argument is
always the remote project directory; this avoids any account-specific absolute
path in the scripts.

```bash
# Fixed-32 decomposition and the original strong-scaling series: 420 rows.
bash scripts/vanda/submit_vanda_all.sh "$PWD"

# Comprehensive all-layout strong scaling: 660 rows.
bash scripts/vanda/submit_vanda_strong_full.sh "$PWD"

# BerkStan replica and UK induced-subgraph scaling: 120 rows.
# This also submits one dependent data-preparation job.
bash scripts/vanda/submit_vanda_weak.sh \
  "$PWD" "/scratch/$USER/DSA5208_Project3_rerun/data/weak"

# Equal-vertex versus in-edge-work partitioning: 80 rows.
bash scripts/vanda/submit_vanda_e3.sh "$PWD"

# Compute, Allreduce, and score-exchange phase timing: 160 rows.
bash scripts/vanda/submit_vanda_phase.sh "$PWD"

# Full-vector versus demand-driven exchange: 160 rows.
bash scripts/vanda/submit_vanda_demand.sh "$PWD"

# Representative UK-2002 optimization pipeline: 25 rows.
bash scripts/vanda/submit_vanda_waterfall_uk.sh "$PWD"
```

The expected measurement inventory is:

| Suite | Submit script | PBS jobs | Measured rows | Analysed in |
|---|---|---:|---:|---|
| Fixed-32 + original strong scaling | `submit_vanda_all.sh` | 36 | 420 | Notebook 02 |
| Comprehensive strong scaling | `submit_vanda_strong_full.sh` | 12 | 660 | Notebook 02 |
| Weak/structural scaling | `submit_vanda_weak.sh` | 13 including preparation | 120 | Notebook 02 |
| In-edge-work partitioning | `submit_vanda_e3.sh` | 4 | 80 | Notebook 03 |
| Phase timing | `submit_vanda_phase.sh` | 16 | 160 | Notebook 04 |
| Demand-driven exchange | `submit_vanda_demand.sh` | 8 | 160 | Notebook 05 |
| UK waterfall | `submit_vanda_waterfall_uk.sh` | 1 | 25 | Notebook 05 |
| **Total** |  | **90 including preparation** | **1,625** |  |

The separate build/test job makes 91 PBS jobs for the complete campaign.
Submit suites in stages if queue or allocation limits apply; the suites are
independent after the build, except that weak-scaling jobs depend automatically
on their preparation job.

### 4. Monitor jobs and inspect failures

```bash
qstat -u "$USER"
python3 scripts/vanda/check_vanda_status.py
```

`check_vanda_status.py` checks the fixed-32/original-strong manifest. Every
suite also writes `submitted_jobs.tsv`, raw CSV files, and PBS logs under its
own `results/vanda/` subdirectory. For a particular job:

```bash
qstat -x -f <job-id> | egrep 'job_state|Exit_status|resources_used'
find results/vanda -type f -name '*.out' -exec grep -Hn \
  -E 'error|failed|killed|out of memory' {} +
```

Do not analyse a campaign until all jobs have `Exit_status = 0`. Each CSV row
must report convergence, and the row totals should match the table above.

### 5. Fetch the completed records

Exit the SSH session and run these commands from the local project root:

```bash
bash scripts/vanda/fetch_vanda_results.sh \
  "$VANDA_REMOTE" "$VANDA_PROJECT"
bash scripts/vanda/fetch_vanda_e3.sh \
  "$VANDA_REMOTE" "$VANDA_PROJECT"
bash scripts/vanda/fetch_vanda_phase.sh \
  "$VANDA_REMOTE" "$VANDA_PROJECT"
bash scripts/vanda/fetch_vanda_demand.sh \
  "$VANDA_REMOTE" "$VANDA_PROJECT"

for suite in strong_full weak waterfall_uk; do
  mkdir -p "results/vanda/$suite"
  rsync -av \
    "$VANDA_REMOTE:$VANDA_PROJECT/results/vanda/$suite/" \
    "results/vanda/$suite/"
done
```

Fetch into a clean local copy when performing an independent rerun. Otherwise,
archive the included reference records first so the notebooks do not combine
two campaigns.

### 6. Rebuild the analyses and report

After confirming the CSV inventory, execute the Vanda analysis notebooks and
rebuild the PDF:

```bash
.venv/bin/jupyter execute 02_vanda_experiments.ipynb --inplace
.venv/bin/jupyter execute 03_load_balancing_experiment.ipynb --inplace
.venv/bin/jupyter execute 04_phase_timing_experiment.ipynb --inplace
.venv/bin/jupyter execute 05_demand_exchange_experiment.ipynb --inplace
.venv/bin/python scripts/build_report.py
```

New timings will not exactly match the submitted values because PBS may place
jobs on different nodes and cluster contention changes. Speedup denominators
must come from the serial measurements in the same job series, as documented
in the report.

## Notebooks

Open the notebooks in numeric order:

All commands below and in the notebooks assume the current directory is the
extracted project root containing `Makefile`; no user-specific absolute path is
required.

1. `00_environment_data_check.ipynb` - toolchain, build/tests, four CSR data
   sets, and checksums;
2. `01_local_experiments.ipynb` - Mac serial, MPI-only, and MPI+OpenMP results;
3. `02_vanda_experiments.ipynb` - Vanda fixed-32 decomposition, comprehensive
   strong scaling over every feasible rank/thread layout from 1 to 128 CPUs,
   BerkStan replica weak scaling, UK-2002 induced-subgraph scaling, and
   strong-vs-weak comparison.
4. `03_load_balancing_experiment.ipynb` - Experiment 5, comparing equal-vertex
   and in-edge-work partitions on Vanda.
5. `04_phase_timing_experiment.ipynb` - representative strong-scaling runs
   instrumented for local compute, `Allreduce`, and `Allgatherv` time.
6. `05_demand_exchange_experiment.ipynb` - Experiment 7, a controlled
   full-vector versus demand-driven exchange comparison at 32 and 64 CPUs,
   plus the controlled UK-2002 optimization-pipeline waterfall.

All six notebooks invoke or analyse the repository implementation instead
of generating a second copy of the source code.

## Report

The editable report source is `report/project3_report_draft.md`. Regenerate its
PDF after editing with a Python environment that provides ReportLab,
Matplotlib, and Pillow. The bundled Latin Modern font files under
`assets/fonts/latin-modern/` are embedded automatically:

```bash
python3 scripts/build_report.py
```

The generated file is `output/pdf/DSA5208_Project3_Yan_Wan_Wang.pdf`. The cover
identifies Yan Tianao (A0354280H), Wan Gehong (A0356324E), and Wang Tianyi
(A0357264X) as the authors.

## Validation and reporting

Defaults are `alpha=0.85`, global L1 tolerance `1e-12`, uniform initialization
and dangling redistribution, and 1,000 maximum iterations. Input edges are
deduplicated and self-loops are retained.

For an independent numerical check:

```bash
make validate PYTHON=.venv/bin/python
```

The saved real-graph validation compares the raw SNAP `web-Google` graph in
NetworkX with the processed CSR graph used by the C++ experiments:

```bash
.venv/bin/python validation/validate_networkx.py \
  --input data/raw/web-Google.txt.gz \
  --cpp-input data/processed/web-Google.prcsr \
  --cpp-id-mode dense-sorted \
  --report validation/reports/web-Google_networkx.json
```

`dense-sorted` aligns the CSR's dense internal IDs with the sorted original
SNAP IDs. The report records both input hashes, graph size, library versions,
runtime, full-vector L1 and maximum errors, and the acceptance decision.

The demand-driven MPI implementation is independently checked on the same
complete graph with four ranks and in-edge-work partitioning:

```bash
.venv/bin/python validation/validate_networkx.py \
  --input data/raw/web-Google.txt.gz \
  --cpp-input data/processed/web-Google.prcsr \
  --cpp-id-mode dense-sorted \
  --cpp-mode mpi --mpi-ranks 4 \
  --partition in-edges --score-exchange demand \
  --report validation/reports/web-Google_networkx_demand.json
```

The assignment requires disclosure of AI assistance. Document the actual use
of AI in extracting, reviewing, refactoring, testing, and documenting this
implementation in the final report.
