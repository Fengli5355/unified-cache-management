#!/usr/bin/env python3
"""Summarize [ASU_PERF] events from one or more UCM log files."""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable


FIELD_RE = re.compile(r"([a-z][a-z0-9_]*)=([^\s\]]+)")
PID_RE = re.compile(r"\[(\d+),(\d+)\]\[[^\]]+\]$")
INTEGER_FIELDS = {
    "assign_us",
    "asu_count",
    "build_send_us",
    "client_dispatch_us",
    "client_finalize_us",
    "client_age_at_complete_us",
    "client_queue_us",
    "client_scatter_us",
    "client_task_id",
    "client_total_us",
    "client_transport_us",
    "completion_wait_us",
    "failed_sub_batches",
    "items",
    "prepare_us",
    "route_us",
    "scatter_us",
    "send_us",
    "send_setup_us",
    "submit_us",
    "status",
    "sub_batches",
    "total_us",
    "trace_id",
    "transport_queue_us",
    "transport_task_id",
    "transport_tasks",
    "transport_total_us",
    "wait_us",
}
TASK_COLUMNS = [
    "source",
    "pid",
    "client_task_id",
    "trace_id",
    "op",
    "items",
    "status",
    "store_total_us",
    "store_submit_us",
    "store_pre_wait_us",
    "store_wait_us",
    "store_post_submit_us",
    "client_total_us",
    "client_queue_us",
    "client_scatter_us",
    "client_transport_us",
    "client_finalize_us",
    "client_timing_error_us",
    "client_dispatch_us",
    "dht_scatter_us",
    "dht_route_us",
    "dht_reorder_us",
    "asu_count",
    "transport_tasks",
    "critical_transport_task_id",
    "critical_transport_total_us",
    "transport_queue_us",
    "transport_prepare_us",
    "transport_assign_us",
    "transport_build_send_us",
    "transport_send_setup_us",
    "transport_send_us",
    "transport_completion_wait_us",
    "transport_timing_error_us",
    "critical_transport_client_age_us",
    "send_sum_us",
    "send_max_us",
    "send_avg_us",
    "store_pre_wait_pct",
    "client_transport_pct",
    "transport_completion_wait_pct",
    "failed_sub_batches",
]
SUMMARY_METRICS = [
    "store_total_us",
    "store_submit_us",
    "store_pre_wait_us",
    "store_wait_us",
    "store_post_submit_us",
    "client_total_us",
    "client_queue_us",
    "client_scatter_us",
    "client_transport_us",
    "client_finalize_us",
    "client_timing_error_us",
    "client_dispatch_us",
    "dht_scatter_us",
    "dht_route_us",
    "dht_reorder_us",
    "critical_transport_total_us",
    "transport_queue_us",
    "transport_prepare_us",
    "transport_assign_us",
    "transport_build_send_us",
    "transport_send_setup_us",
    "transport_send_us",
    "transport_completion_wait_us",
    "transport_timing_error_us",
    "critical_transport_client_age_us",
    "send_sum_us",
    "send_max_us",
    "store_pre_wait_pct",
    "client_transport_pct",
    "transport_completion_wait_pct",
]


def ParseArgs() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Parse [ASU_PERF] logs and report per-task AsuStore, DHT scatter, and "
            "Transport Send timing."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, help="UCM log files to parse")
    parser.add_argument(
        "--csv", type=Path, dest="csv_path", help="write the per-task table to this CSV file"
    )
    parser.add_argument(
        "--no-tasks", action="store_true", help="do not print the per-task table to stdout"
    )
    return parser.parse_args()


def ParseEvent(line: str) -> dict[str, object] | None:
    if "[ASU_PERF]" not in line:
        return None
    fields: dict[str, object] = {key: value for key, value in FIELD_RE.findall(line)}
    if "event" not in fields:
        return None
    for name in INTEGER_FIELDS:
        value = fields.get(name)
        if value is None:
            continue
        try:
            fields[name] = int(str(value))
        except ValueError:
            pass
    pidMatch = PID_RE.search(line)
    fields["pid"] = int(pidMatch.group(1)) if pidMatch else 0
    return fields


def ReadEvents(paths: Iterable[Path]) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for path in paths:
        with path.open("r", encoding="utf-8", errors="replace") as logFile:
            for line in logFile:
                event = ParseEvent(line.rstrip())
                if event is None:
                    continue
                event["source"] = str(path)
                events.append(event)
    return events


def TaskKey(event: dict[str, object]) -> tuple[str, int, int] | None:
    taskId = event.get("client_task_id")
    if not isinstance(taskId, int):
        return None
    return str(event["source"]), int(event["pid"]), taskId


def BuildTaskRows(events: Iterable[dict[str, object]]) -> list[dict[str, object]]:
    tasks: dict[tuple[str, int, int], dict[str, object]] = {}
    sends: dict[tuple[str, int, int], list[dict[str, object]]] = defaultdict(list)
    completions: dict[tuple[str, int, int], list[dict[str, object]]] = defaultdict(list)

    for event in events:
        key = TaskKey(event)
        if key is None:
            continue
        name = event["event"]
        if name == "asu_transport_send":
            sends[key].append(event)
        elif name == "asu_transport_complete":
            completions[key].append(event)
        elif name in {
            "asu_store_io_submitted",
            "asu_store_io_done",
            "asu_dht_scatter",
            "asu_client_dispatch",
            "asu_client_complete",
        }:
            task = tasks.setdefault(key, {})
            task.update({field: value for field, value in event.items() if field != "event"})
            if name == "asu_store_io_done":
                task["store_total_us"] = event.get("total_us")
            elif name == "asu_store_io_submitted":
                task["store_submit_us"] = event.get("submit_us")
            elif name == "asu_dht_scatter":
                task["dht_route_us"] = event.get("route_us")
                task["dht_scatter_us"] = event.get("scatter_us")

    rows: list[dict[str, object]] = []
    for key in sorted(tasks):
        source, pid, taskId = key
        task = tasks[key]
        sendEvents = sends.get(key, [])
        completionEvents = completions.get(key, [])
        sendValues = [int(event["send_us"]) for event in sendEvents if "send_us" in event]
        validCompletions = [
            event
            for event in completionEvents
            if isinstance(event.get("client_age_at_complete_us"), int)
            and int(event["client_age_at_complete_us"]) >= 0
        ]
        criticalTransport = (
            max(validCompletions, key=lambda event: int(event["client_age_at_complete_us"]))
            if validCompletions
            else {}
        )
        routeUs = Number(task.get("dht_route_us"))
        scatterUs = Number(task.get("dht_scatter_us"))
        totalUs = Number(task.get("store_total_us"))
        submitUs = Number(task.get("store_submit_us"))
        waitUs = Number(task.get("wait_us"))
        preWaitUs = Subtract(totalUs, submitUs, waitUs, clampAtZero=True)
        clientTotalUs = Number(task.get("client_total_us"))
        clientQueueUs = Number(task.get("client_queue_us"))
        clientScatterUs = Number(task.get("client_scatter_us"))
        clientTransportUs = Number(task.get("client_transport_us"))
        clientFinalizeUs = Number(task.get("client_finalize_us"))
        transportTotalUs = Number(criticalTransport.get("transport_total_us"))
        transportQueueUs = Number(criticalTransport.get("transport_queue_us"))
        transportPrepareUs = Number(criticalTransport.get("prepare_us"))
        transportAssignUs = Number(criticalTransport.get("assign_us"))
        transportBuildSendUs = Number(criticalTransport.get("build_send_us"))
        transportSendSetupUs = Number(criticalTransport.get("send_setup_us"))
        transportSendUs = Number(criticalTransport.get("send_us"))
        transportCompletionWaitUs = Number(criticalTransport.get("completion_wait_us"))
        sendMaxUs = max(sendValues) if sendValues else None
        row: dict[str, object] = {
            "source": source,
            "pid": pid,
            "client_task_id": taskId,
            "trace_id": task.get("trace_id", ""),
            "op": task.get("op", ""),
            "items": task.get("items", ""),
            "status": task.get("status", ""),
            "store_total_us": BlankIfNone(totalUs),
            "store_submit_us": BlankIfNone(submitUs),
            "store_pre_wait_us": BlankIfNone(preWaitUs),
            "store_wait_us": BlankIfNone(waitUs),
            "store_post_submit_us": BlankIfNone(
                Subtract(totalUs, submitUs, clampAtZero=True)
            ),
            "client_total_us": BlankIfNone(clientTotalUs),
            "client_queue_us": BlankIfNone(clientQueueUs),
            "client_scatter_us": BlankIfNone(clientScatterUs),
            "client_transport_us": BlankIfNone(clientTransportUs),
            "client_finalize_us": BlankIfNone(clientFinalizeUs),
            "client_dispatch_us": task.get("client_dispatch_us", ""),
            "client_timing_error_us": BlankIfNone(
                Subtract(
                    clientTotalUs,
                    clientQueueUs,
                    clientScatterUs,
                    clientTransportUs,
                    clientFinalizeUs,
                )
            ),
            "dht_route_us": BlankIfNone(routeUs),
            "dht_scatter_us": BlankIfNone(scatterUs),
            "dht_reorder_us": BlankIfNone(Subtract(scatterUs, routeUs)),
            "asu_count": task.get("asu_count", ""),
            "transport_tasks": max(len(sendEvents), len(completionEvents)),
            "critical_transport_task_id": criticalTransport.get("transport_task_id", ""),
            "critical_transport_client_age_us": criticalTransport.get(
                "client_age_at_complete_us", ""
            ),
            "critical_transport_total_us": BlankIfNone(transportTotalUs),
            "transport_queue_us": BlankIfNone(transportQueueUs),
            "transport_prepare_us": BlankIfNone(transportPrepareUs),
            "transport_assign_us": BlankIfNone(transportAssignUs),
            "transport_build_send_us": BlankIfNone(transportBuildSendUs),
            "transport_send_setup_us": BlankIfNone(transportSendSetupUs),
            "transport_send_us": BlankIfNone(transportSendUs),
            "transport_completion_wait_us": BlankIfNone(transportCompletionWaitUs),
            "transport_timing_error_us": BlankIfNone(
                Subtract(
                    transportTotalUs,
                    transportQueueUs,
                    transportPrepareUs,
                    transportAssignUs,
                    transportBuildSendUs,
                    transportSendSetupUs,
                    transportSendUs,
                    transportCompletionWaitUs,
                )
            ),
            "send_sum_us": sum(sendValues) if sendValues else "",
            "send_max_us": BlankIfNone(sendMaxUs),
            "send_avg_us": Round(sum(sendValues) / len(sendValues)) if sendValues else "",
            "store_pre_wait_pct": BlankIfNone(Percent(preWaitUs, totalUs)),
            "client_transport_pct": BlankIfNone(Percent(clientTransportUs, clientTotalUs)),
            "transport_completion_wait_pct": BlankIfNone(
                Percent(transportCompletionWaitUs, transportTotalUs)
            ),
            "failed_sub_batches": sum(
                int(event.get("failed_sub_batches", 0)) for event in sendEvents
            ),
        }
        rows.append(row)
    return rows


def Number(value: object) -> float | None:
    return float(value) if isinstance(value, (int, float)) else None


def BlankIfNone(value: object | None) -> object:
    return "" if value is None else value


def Subtract(
    value: float | None, *parts: float | None, clampAtZero: bool = False
) -> float | None:
    if value is None or any(part is None for part in parts):
        return None
    result = value - sum(part for part in parts if part is not None)
    return max(result, 0.0) if clampAtZero else result


def Percent(part: float | None, total: float | None) -> float | None:
    if part is None or total is None or total <= 0:
        return None
    return Round(part * 100.0 / total)


def Round(value: float) -> float:
    return round(value, 2)


def Percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def PrintTasks(rows: list[dict[str, object]]) -> None:
    print("PER-TASK METRICS")
    writer = csv.DictWriter(sys.stdout, fieldnames=TASK_COLUMNS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


def PrintSummary(rows: list[dict[str, object]]) -> None:
    print("\nSUMMARY")
    print("op,metric,count,avg,p50,p95,p99,max")
    groups: dict[str, list[dict[str, object]]] = {"all": rows}
    for row in rows:
        groups.setdefault(str(row.get("op") or "unknown"), []).append(row)
    for op, groupRows in groups.items():
        for metric in SUMMARY_METRICS:
            values = [
                float(row[metric])
                for row in groupRows
                if isinstance(row.get(metric), (int, float))
            ]
            if not values:
                continue
            print(
                f"{op},{metric},{len(values)},{Round(sum(values) / len(values))},"
                f"{Round(Percentile(values, 0.50))},{Round(Percentile(values, 0.95))},"
                f"{Round(Percentile(values, 0.99))},{Round(max(values))}"
            )

    complete = sum(
        1
        for row in rows
        if row["store_total_us"] != "" and row["dht_scatter_us"] != ""
    )
    failed = sum(1 for row in rows if row.get("status") not in {"", 0, "0"})
    print(
        f"\ncoverage: tasks={len(rows)} complete_store_and_dht={complete} "
        f"failed_tasks={failed}"
    )
    print(
        "note: critical transport fields come from the Transport task that completed latest "
        "relative to Client submission. client_dispatch_us overlaps client_transport_us and "
        "is not an additive client phase."
    )


def WriteCsv(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=TASK_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def Main() -> int:
    args = ParseArgs()
    try:
        events = ReadEvents(args.logs)
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    rows = BuildTaskRows(events)
    if not args.no_tasks:
        PrintTasks(rows)
    PrintSummary(rows)
    if args.csv_path is not None:
        try:
            WriteCsv(args.csv_path, rows)
        except OSError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())
