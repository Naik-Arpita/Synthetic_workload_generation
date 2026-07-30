#!/usr/bin/env python3
import sys
import os
import json
import math

def parse_trace(filepath):
    if not os.path.exists(filepath):
        sys.stderr.write(f"[FATAL] Trace file not found: {filepath}\n")
        sys.exit(1)

    raw_snapshots = []
    
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
            if isinstance(data, list):
                raw_snapshots = data
            elif isinstance(data, dict):
                raw_snapshots = [data]
    except Exception:
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        raw_snapshots.append(json.loads(line))
                    except Exception:
                        pass

    if not raw_snapshots:
        sys.stderr.write("[FATAL] No valid snapshots parsed.\n")
        sys.exit(1)

    total_cores_A = 8
    parsed_snaps = []

    for j in raw_snapshots:
        if "lscpu" in j and "cpus_num" in j["lscpu"]:
            total_cores_A = int(j["lscpu"]["cpus_num"])
        elif "cpu_total" in j and "cpus" in j["cpu_total"]:
            total_cores_A = int(j["cpu_total"]["cpus"])

        if "cpu_total" in j and "idle" in j["cpu_total"]:
            idle = float(j["cpu_total"]["idle"])
            util = max(0.0, min(100.0, 100.0 - idle))
            
            active_count = 0
            if "cpus" in j and isinstance(j["cpus"], dict):
                for core_data in j["cpus"].values():
                    if isinstance(core_data, dict) and "idle" in core_data:
                        if (100.0 - float(core_data["idle"])) > 1.0:
                            active_count += 1
            
            active_cores_A = active_count if active_count > 0 else max(1, round((util / 100.0) * total_cores_A))
            parsed_snaps.append((util, active_cores_A))

   
    total_active_fraction = sum(a / total_cores_A for _, a in parsed_snaps)
    avg_active_fraction = total_active_fraction / len(parsed_snaps)

    print(f"{len(parsed_snaps)} {total_cores_A} {avg_active_fraction:.6f}")
    for util, _ in parsed_snaps:
        print(f"{util:.4f}")

if __name__ == "__main__":
    trace_path = sys.argv[1] if len(sys.argv) > 1 else "5CG04131YG_20260727_0857.json"
    parse_trace(trace_path)