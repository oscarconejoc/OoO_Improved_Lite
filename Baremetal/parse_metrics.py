#!/usr/bin/env python3
"""Convierte métricas agregadas de Gem5 en CSV por benchmark individual.

Gem5 expone métricas globales en `stats.txt`. Como los tres micro-benchmarks
de GEMM corren en una misma simulación, este script reparte los totales por
benchmark usando pesos de cómputo (FLOPs = 2*m*n*k).
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

BENCHMARKS = [
    (0, 4, 4, 4),
    (1, 8, 8, 8),
    (2, 16, 16, 16),
]


def _resolve_stats_path(path_text: str) -> Path:
    candidate = Path(path_text)
    if candidate.is_dir():
        candidate = candidate / "stats.txt"
    if candidate.name != "stats.txt":
        raise RuntimeError("Pasa la ruta a gem5/out/stats.txt o al directorio gem5/out")
    if not candidate.is_file():
        raise RuntimeError(f"No se encontró el archivo de estadísticas: {candidate}")
    return candidate


def _parse_stats_file(stats_path: Path) -> dict[str, str]:
    metrics: dict[str, str] = {}
    pattern = re.compile(r"^([A-Za-z0-9_.:\-]+)\s+([-+0-9.eE]+)\b")

    for line in stats_path.read_text().splitlines():
        match = pattern.match(line.strip())
        if not match:
            continue
        metric_name, value_text = match.groups()
        metrics.setdefault(metric_name, value_text)
    return metrics


def _get_float(metrics: dict[str, str], key: str, default: float = 0.0) -> float:
    value = metrics.get(key)
    if value is None:
        return default
    return float(value)


def _build_rows(metrics: dict[str, str]) -> list[list[str]]:
    total_cycles = _get_float(metrics, "system.cpu.numCycles")
    total_instret = _get_float(metrics, "simInsts")
    total_sim_seconds = _get_float(metrics, "simSeconds")
    total_host_seconds = _get_float(metrics, "hostSeconds")

    workloads = []
    flops_total = 0.0
    for bench_id, m, n, k in BENCHMARKS:
        flops = float(2 * m * n * k)
        flops_total += flops
        workloads.append((bench_id, m, n, k, flops))

    rows: list[list[str]] = []
    for bench_id, m, n, k, flops in workloads:
        weight = (flops / flops_total) if flops_total else 0.0
        est_cycles = int(round(total_cycles * weight))
        est_instret = int(round(total_instret * weight))
        rows.append(
            [
                str(bench_id),
                str(m),
                str(n),
                str(k),
                str(int(flops)),
                f"{weight:.6f}",
                str(est_cycles),
                str(est_instret),
                f"{total_sim_seconds:.6f}",
                f"{total_host_seconds:.6f}",
                "estimated_from_aggregate_stats",
            ]
        )
    return rows


def _print_pretty(headers: list[str], rows: list[list[str]]) -> None:
    widths = [len(h) for h in headers]
    for row in rows:
        for idx, value in enumerate(row):
            widths[idx] = max(widths[idx], len(value))

    def fmt_row(values: list[str]) -> str:
        return " | ".join(values[i].ljust(widths[i]) for i in range(len(values)))

    separator = "-+-".join("-" * w for w in widths)

    print(fmt_row(headers))
    print(separator)
    for row in rows:
        print(fmt_row(row))


def parse_metrics(stats_path_text: str, output_format: str = "csv") -> None:
    stats_path = _resolve_stats_path(stats_path_text)
    metrics = _parse_stats_file(stats_path)
    headers = [
        "benchmark_id",
        "m",
        "n",
        "k",
        "flops",
        "weight",
        "est_cycles",
        "est_instret",
        "sim_seconds_total",
        "host_seconds_total",
        "note",
    ]
    rows = _build_rows(metrics)

    if output_format == "pretty":
        _print_pretty(headers, rows)
        return

    writer = csv.writer(sys.stdout)
    writer.writerow(headers)
    writer.writerows(rows)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convierte métricas de Gem5 en CSV o tabla alineada por benchmark"
    )
    parser.add_argument("stats_path", help="Ruta a gem5/out o gem5/out/stats.txt")
    parser.add_argument(
        "--format",
        choices=["csv", "pretty"],
        default="csv",
        help="Formato de salida (default: csv)",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = _parse_args()
    parse_metrics(args.stats_path, args.format)
