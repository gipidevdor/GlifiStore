import argparse
import csv
import hashlib
import io
import json
import os
import platform
import statistics
import subprocess
from datetime import UTC, datetime
from pathlib import Path

root = Path.cwd()
parser = argparse.ArgumentParser(
    description="Collect the ADR 0036 campaign into a fresh directory"
)
parser.add_argument("--output", type=Path, required=True)
result = parser.parse_args().output.resolve()
result.mkdir(parents=True, exist_ok=False)
source = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
subprocess.run(
    ["git", "diff", "--exit-code", "HEAD"], check=True, stdout=subprocess.DEVNULL
)
binary = root / "build/unix-release"
metadata = {
    "timestamp_utc": datetime.now(UTC).isoformat(),
    "source_commit": source,
    "system": platform.system(),
    "system_release": platform.release(),
    "machine": platform.machine(),
    "cpu": subprocess.check_output(
        ["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"], text=True
    ).strip(),
    "physical_cpus": int(
        subprocess.check_output(["/usr/sbin/sysctl", "-n", "hw.physicalcpu"], text=True)
    ),
    "logical_cpus": os.cpu_count(),
    "memory_bytes": int(
        subprocess.check_output(["/usr/sbin/sysctl", "-n", "hw.memsize"], text=True)
    ),
    "load_average_before": os.getloadavg(),
    "power_source": subprocess.check_output(
        ["/usr/bin/pmset", "-g", "batt"], text=True
    ).splitlines()[0],
    "configuration": "unix-release; GLIFISTORE_BUILD_LAB=ON; CMAKE_BUILD_TYPE=Release",
    "production": "official paired embedded Store; volatile; worker-affine callers; 64 keys/worker; 64-byte values; no CPU pinning",
    "commands": [],
    "binaries_sha256": {},
}
for name in [
    "glifistore_generation_shell_benchmark",
    "glifistore_generation_publication_benchmark",
    "glifistore_generation_runtime_benchmark",
]:
    metadata["binaries_sha256"][name] = hashlib.sha256(
        (binary / name).read_bytes()
    ).hexdigest()


def run(name, executable, arguments):
    command = [str(binary / executable), *arguments]
    metadata["commands"].append(
        {"output": name, "argv": [f"build/unix-release/{executable}", *arguments]}
    )
    print("Running " + name, flush=True)
    with (
        (result / name).open("w") as output,
        (result / (name + ".stderr")).open("w") as errors,
    ):
        subprocess.run(command, check=True, stdout=output, stderr=errors)
    if not (result / (name + ".stderr")).stat().st_size:
        (result / (name + ".stderr")).unlink()


run(
    "generation-shell.tsv",
    "glifistore_generation_shell_benchmark",
    ["--ops", "20000", "--warmup", "1", "--repeats", "7"],
)
for mode in ["adopt", "get"]:
    run(
        f"publication-{mode}.tsv",
        "glifistore_generation_publication_benchmark",
        ["--ops", "20000", "--warmup", "1", "--repeats", "7", "--reader-work", mode],
    )
for threads in [1, 2]:
    for workload in ["put", "get", "put-get"]:
        run(
            f"production-{workload}-{threads}t.tsv",
            "glifistore_generation_runtime_benchmark",
            [
                "--ops",
                "200000",
                "--warmup",
                "1",
                "--repeats",
                "7",
                "--threads",
                str(threads),
                "--workload",
                workload,
                "--generation-slot-pool",
                "both",
            ],
        )
metadata["load_average_after"] = os.getloadavg()
metadata["finished_utc"] = datetime.now(UTC).isoformat()
(result / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
summary = {}
for path in sorted(result.glob("*.tsv")):
    text = path.read_text()
    rows = list(
        csv.DictReader(
            io.StringIO(
                "\n".join(
                    line for line in text.splitlines() if not line.startswith("#")
                )
            ),
            delimiter="\t",
        )
    )
    groups = {}
    for row in rows:
        group = row.get("implementation", row.get("generation_slot_pool"))
        groups.setdefault(group, []).append(row)
    file_summary = {}
    for group, samples in groups.items():
        assert len(samples) == 7, (path, group, len(samples))
        assert {int(row["repeat"]) for row in samples} == set(range(7)), (path, group)
        assert all(int(row["checksum"]) > 0 for row in samples), (path, group)
        if "generation_slot_pool" in samples[0]:
            assert all(row["checksum"] == row["operations"] for row in samples), path
        excluded = {
            "repeat",
            "checksum",
            "threads",
            "operations",
            "generation_slot_pool",
            "workload",
            "reader_affinity",
            "writer_affinity",
        }
        metrics = {}
        for key in samples[0]:
            if key in excluded:
                continue
            try:
                values = [float(row[key]) for row in samples]
            except (ValueError, TypeError):
                continue
            quartiles = statistics.quantiles(values, n=4, method="inclusive")
            metrics[key] = {
                "median": statistics.median(values),
                "minimum": min(values),
                "maximum": max(values),
                "q25": quartiles[0],
                "q75": quartiles[2],
            }
        file_summary[group] = metrics
    summary[path.name] = file_summary
(result / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print("Validated 7 repeats per implementation/mode; wrote " + str(result), flush=True)
