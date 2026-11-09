#!/usr/bin/env python3
import csv
import re
import subprocess
from collections import Counter
from pathlib import Path


root = Path(__file__).resolve().parents[2]
manifest_path = root / "results/vanda/submitted_jobs.tsv"
raw_dir = root / "results/vanda/raw"

with manifest_path.open() as stream:
    jobs = list(csv.DictReader(stream, delimiter="\t"))

states = Counter()
failures = []
complete_csv = 0
for job in jobs:
    job_id = job["job_id"]
    completed = subprocess.run(
        ["qstat", "-x", "-f", job_id], text=True, capture_output=True
    )
    text = completed.stdout + completed.stderr
    state_match = re.search(r"^\s*job_state = (\w+)", text, re.MULTILINE)
    exit_match = re.search(r"^\s*Exit_status = (-?\d+)", text, re.MULTILINE)
    state = state_match.group(1) if state_match else "unknown"
    states[state] += 1
    if state == "F" and (not exit_match or int(exit_match.group(1)) != 0):
        failures.append((job_id, exit_match.group(1) if exit_match else "missing"))

    files = list(raw_dir.glob(f"{job_id.split('.')[0]}-*.csv"))
    expected_rows = 31 if job["suite"] == "fixed32" else (6 if job["total_cpus"] == "1" else 11)
    if len(files) == 1:
        with files[0].open() as stream:
            rows = sum(1 for _ in stream)
        if rows == expected_rows:
            complete_csv += 1

print("jobs:", len(jobs))
print("states:", " ".join(f"{key}={value}" for key, value in sorted(states.items())))
print("complete CSV files:", f"{complete_csv}/{len(jobs)}")
print("failures:", failures if failures else "none")
