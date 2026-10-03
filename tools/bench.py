#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial", "matplotlib"]
# ///
"""bench.py — benchmark and stress-test KV engines on ESP32-S3 boards.

Each board runs one engine (built with tools/fw.sh). This script drives all
of them over serial at the same time, runs the same workload on each and
writes a report. All timing happens *on the board* (the `bench` shell
command), so the serial link adds nothing to the numbers.

    tools/bench.py list                         # which board is on which port
    tools/bench.py run                          # all boards, default ladder
    tools/bench.py run --sizes 1000,10000,100000,200000 --vsize 64
    tools/bench.py run --ports /dev/ttyUSB0 --sizes 500   # quick smoke test
    tools/bench.py report bench_results/<run>   # rebuild report from results
    tools/bench.py race                         # LED race for filming, one board at a time
    tools/bench.py race-report bench_results/race

For every size in the ladder, on a freshly erased board:
    fill         put n keys (random 64-bit keys, vsize-byte values)
    read_hit     n random gets of existing keys, every value checked
    read_miss    gets of keys that were never written
    overwrite    put the first n/10 keys again with new values
    delete       delete the first n/20 keys
    verify       get every key in order; checks values, overwrites and deletes
    reopen       reboot; time to mount + replay what's on flash
    verify_boot  the full verify again after the reboot (durability)
A variant that hits the per-step time budget isn't run at larger sizes.

`race` is for filming: it formats each connected board in turn, waits for
Enter (start the camera), then runs the `race` shell command: 3-2-1-GO on the
green LED, a seeded mix of put/get/del bursts with one LED blink per N ops,
and a long green blink at the finish. Same seed = same workload on every
engine, so boards filmed separately line up on the GO frame. Results append
to <out>/race.jsonl; `race-report` turns them into a table and 16:9 charts.

Run it as ./tools/bench.py: uv installs pyserial and matplotlib (charts) on
first use. Without uv: `python3 tools/bench.py ...` with pyserial installed
(the ESP-IDF Python env has it); charts are skipped without matplotlib.
"""

import argparse
import csv
import datetime as dt
import glob
import json
import queue
import re
import sys
import threading
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run ./tools/bench.py (uv installs it), or `pip install pyserial`")

BAUD = 115200
LINE_RE = re.compile(r"(READY|INFO|BENCH|BENCH_ERR|RACE|RACE_ERR) (\{.*\})")
# INFO fields that must match across boards for the comparison to be fair.
FAIRNESS_KEYS = ("cpu_mhz", "flash_mb", "psram_mb", "opt", "idf", "partition_kb", "chip_rev")
STEPS = ("fill", "read_hit", "read_miss", "overwrite", "delete", "verify", "reopen", "verify_boot")


class BoardError(Exception):
    pass


class Board:
    """One ESP32 on one serial port, speaking the `db>` shell."""

    def __init__(self, port, log_path=None):
        self.port = port
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.2
        # Don't toggle EN/IO0 through the auto-reset circuit when opening.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.lines = queue.Queue()
        self.log = open(log_path, "a", buffering=1) if log_path else None
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self.info = None

    def _read_loop(self):
        buf = b""
        while not self._stop:
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if self.log:
                    self.log.write(f"{time.strftime('%H:%M:%S')} {line}\n")
                self.lines.put(line)

    def close(self):
        self._stop = True
        self._reader.join(timeout=1)
        self.ser.close()
        if self.log:
            self.log.close()

    def drain(self):
        while not self.lines.empty():
            self.lines.get_nowait()

    def send(self, cmd):
        if self.log:
            self.log.write(f"{time.strftime('%H:%M:%S')} >>> {cmd}\n")
        self.ser.write(cmd.encode() + b"\r")
        self.ser.flush()

    def wait_for(self, kinds, timeout):
        """Next READY/INFO/BENCH/... line of one of `kinds`; returns (kind, json)."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise BoardError(f"{self.port}: no {'/'.join(kinds)} within {timeout:.0f}s")
            try:
                line = self.lines.get(timeout=min(left, 1.0))
            except queue.Empty:
                continue
            m = LINE_RE.search(line)
            if m and m.group(1) in kinds:
                try:
                    return m.group(1), json.loads(m.group(2))
                except json.JSONDecodeError as e:
                    raise BoardError(f"{self.port}: bad {m.group(1)} line: {line!r} ({e})")

    def command(self, cmd, kinds, timeout):
        self.drain()
        self.send(cmd)
        return self.wait_for(kinds, timeout)

    def identify(self, boot_timeout=120):
        """INFO from the board, waiting out a boot in progress if needed."""
        self.send("")  # flush any half-typed line
        for _ in range(2):
            try:
                _, self.info = self.command("info", ("INFO",), 3)
                break
            except BoardError:
                # Probably booting (or the port open reset it): wait for READY.
                try:
                    self.wait_for(("READY",), boot_timeout)
                except BoardError:
                    pass
        if not self.info:
            raise BoardError(f"{self.port}: no answer to `info` (is the bench firmware flashed? tools/fw.sh)")
        return self.info

    def wait_ready(self, timeout):
        _, ready = self.wait_for(("READY",), timeout)
        if not ready.get("ok"):
            raise BoardError(f"{self.port}: database failed to open after boot: {ready.get('err')}")
        return ready

    def format(self, timeout=600):
        self.drain()
        self.send("format yes")
        return self.wait_ready(timeout)

    def reboot(self, timeout=600):
        self.drain()
        self.send("reboot")
        return self.wait_ready(timeout)

    def bench(self, op, budget_s, **opts):
        args = " ".join(f"{k}={v}" for k, v in opts.items() if v is not None)
        cmd = f"bench {op} {args} max_s={budget_s}"
        kind, res = self.command(cmd, ("BENCH", "BENCH_ERR"), budget_s + 180)
        if kind == "BENCH_ERR":
            raise BoardError(f"{self.port}: `{cmd}` rejected: {res.get('error')}")
        return res


def find_ports():
    ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        try:
            from serial.tools import list_ports
            ports = sorted(p.device for p in list_ports.comports())
        except ImportError:
            pass
    return ports


def connect_all(ports, out_dir=None):
    """Open and identify every port in parallel; returns {port: Board}."""
    boards, errors = {}, {}

    def one(port):
        log = out_dir / f"serial-{Path(port).name}.log" if out_dir else None
        try:
            b = Board(port, log)
        except serial.SerialException as e:
            errors[port] = str(e)
            return
        try:
            b.identify()
            boards[port] = b
        except BoardError as e:
            errors[port] = str(e)
            b.close()

    threads = [threading.Thread(target=one, args=(p,)) for p in ports]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for port, err in sorted(errors.items()):
        print(f"  {port}: skipped ({err})")
    return dict(sorted(boards.items()))


def check_fairness(boards):
    warnings = []
    infos = {p: b.info for p, b in boards.items()}
    for key in FAIRNESS_KEYS:
        values = {p: i.get(key) for p, i in infos.items()}
        if len(set(map(str, values.values()))) > 1:
            warnings.append(f"boards differ in {key}: " + ", ".join(f"{p}={v}" for p, v in values.items()))
    for p, i in infos.items():
        if i.get("wifi") == "connected":
            warnings.append(f"{p} ({i['engine']}) has WiFi connected: background radio work may add jitter")
    return warnings


# ---------------------------------------------------------------- running

class Recorder:
    def __init__(self, out_dir):
        self.path = out_dir / "results.jsonl"
        self.lock = threading.Lock()

    def add(self, row):
        row["time"] = dt.datetime.now().isoformat(timespec="seconds")
        with self.lock, open(self.path, "a") as f:
            f.write(json.dumps(row) + "\n")


def variants_for(info, sqlite_batches):
    """(label, batch) pairs to run on a board."""
    if info["engine"] == "sqlite":
        return [("sqlite" if b == 1 else f"sqlite-batch{b}", b) for b in sqlite_batches]
    return [(info["engine"], 1)]


def run_variant(board, label, batch, args, rec, say):
    common = dict(keys=args.keys, vsize=args.vsize, seed=args.seed)
    budget = args.budget
    for n in args.sizes:
        base = dict(board=board.port, mac=board.info.get("mac"), engine=board.info["engine"],
                    variant=label, size=n, vsize=args.vsize, keys=args.keys, batch=batch)

        def record(step, res):
            rec.add({**base, "step": step, **{k: v for k, v in res.items() if k not in base}})
            if step in ("reopen",):
                say(f"{label:>16} n={n:<7} {step:<11} open {res['open_us'] / 1000:9.1f} ms")
            else:
                flag = ""
                if res.get("bad") or res.get("err"):
                    flag = f"  !! bad={res['bad']} err={res['err']} ({res['first_err']})"
                if res.get("timed_out"):
                    flag += f"  (budget hit after {res['done']}/{res['n']})"
                say(f"{label:>16} n={n:<7} {step:<11} {res['ops_per_s']:10.1f} ops/s  "
                    f"p50 {res['p50_us']:>7} us  p99 {res['p99_us']:>8} us{flag}")
            return res

        say(f"{label:>16} n={n:<7} formatting...")
        board.format()

        fill = record("fill", board.bench("put", budget, n=n, batch=batch, gen=0, **common))
        live = fill["ok"]
        capped = fill["timed_out"]
        if live == 0:
            say(f"{label:>16} n={n:<7} nothing was written ({fill['first_err']}), stopping this variant")
            return

        reads = min(n, args.max_reads) if args.max_reads else n
        r = record("read_hit", board.bench("get", budget, n=reads, order="rand", range=live, live=live, **common))
        capped |= r["timed_out"]
        r = record("read_miss", board.bench("get", budget, n=min(reads, 10000), order="rand",
                                            start=max(n, live), range=max(n, 1000), live=live, **common))
        capped |= r["timed_out"]

        ow = max(1, live // 10)
        r = record("overwrite", board.bench("put", budget, n=ow, batch=batch, gen=1, **common))
        ow = r["ok"] if r["ok"] == r["done"] else 0  # partial overwrite: don't claim any
        capped |= r["timed_out"]
        dl = max(1, live // 20) if ow else 0
        if dl:
            r = record("delete", board.bench("del", budget, n=dl, batch=batch, **common))
            dl = r["ok"] if r["ok"] == r["done"] else 0
            capped |= r["timed_out"]

        expect = dict(live=live, ow_below=ow, del_below=dl)
        r = record("verify", board.bench("get", budget, n=live, order="seq", **expect, **common))
        capped |= r["timed_out"]

        record("reopen", board.reboot())
        r = record("verify_boot", board.bench("get", budget, n=live, order="seq", **expect, **common))
        capped |= r["timed_out"]

        if capped:
            say(f"{label:>16} hit the {budget}s budget at n={n}; skipping larger sizes")
            return


def cmd_run(args):
    out_dir = Path(args.out) if args.out else Path("bench_results") / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    ports = args.ports or find_ports()
    if not ports:
        sys.exit("no serial ports found (pass --ports)")
    print(f"connecting to {', '.join(ports)}")
    boards = connect_all(ports, out_dir)
    if not boards:
        sys.exit("no benchmark boards answered")
    for p, b in boards.items():
        print(f"  {p}: {b.info['engine']:<8} mac {b.info['mac']}  {b.info['partition_kb']} KB storage, "
              f"-{b.info['opt']}, wifi {b.info['wifi']}")
    for w in check_fairness(boards):
        print(f"  WARNING: {w}")

    (out_dir / "config.json").write_text(json.dumps({
        "args": {k: v for k, v in vars(args).items() if k != "func"},
        "boards": {p: b.info for p, b in boards.items()},
        "fairness_warnings": check_fairness(boards),
    }, indent=2))
    rec = Recorder(out_dir)
    print_lock = threading.Lock()
    failures = []

    def worker(board):
        def say(msg):
            with print_lock:
                print(f"[{Path(board.port).name}] {msg}", flush=True)
        try:
            for label, batch in variants_for(board.info, args.sqlite_batch):
                run_variant(board, label, batch, args, rec, say)
        except BoardError as e:
            failures.append(str(e))
            say(f"FAILED: {e}")

    threads = [threading.Thread(target=worker, args=(b,)) for b in boards.values()]
    t0 = time.monotonic()
    for t in threads:
        t.start()
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        print("\ninterrupted; writing report for what finished")
    for b in boards.values():
        b.close()
    print(f"done in {(time.monotonic() - t0) / 60:.1f} min")
    write_report(out_dir)
    if failures:
        sys.exit(1)


def cmd_list(args):
    ports = args.ports or find_ports()
    if not ports:
        sys.exit("no serial ports found")
    boards = connect_all(ports)
    for p, b in boards.items():
        i = b.info
        keys = i["keys"] if i["keys"] >= 0 else "?"
        print(f"{p}: {i['engine']:<8} mac {i['mac']}  open={i['open']} keys={keys}  "
              f"{i['partition']}={i['partition_kb']} KB  -{i['opt']}  wifi {i['wifi']}  built {i['built']}")
        b.close()
    for w in check_fairness(boards):
        print(f"WARNING: {w}")


# ---------------------------------------------------------------- reporting

def load_rows(out_dir):
    path = Path(out_dir) / "results.jsonl"
    if not path.exists():
        sys.exit(f"{path} not found")
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def fmt_rate(v):
    return f"{v / 1000:.1f}k" if v >= 10000 else f"{v:.0f}"


def fmt_us(v):
    if v >= 1_000_000:
        return f"{v / 1e6:.2f} s"
    if v >= 1000:
        return f"{v / 1000:.1f} ms"
    return f"{v} µs"


def write_report(out_dir):
    out_dir = Path(out_dir)
    rows = load_rows(out_dir)
    if not rows:
        print("no results to report")
        return
    cfg = json.loads((out_dir / "config.json").read_text()) if (out_dir / "config.json").exists() else {}

    # Flat CSV of everything.
    fields = sorted({k for r in rows for k in r})
    lead = ["variant", "size", "step", "ops_per_s", "p50_us", "p99_us", "max_us", "open_us", "ok", "bad", "err"]
    fields = [f for f in lead if f in fields] + [f for f in fields if f not in lead]
    with open(out_dir / "results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)

    variants = list(dict.fromkeys(r["variant"] for r in rows))
    sizes = sorted({r["size"] for r in rows})
    by = {(r["variant"], r["size"], r["step"]): r for r in rows}

    md = [f"# KV engine benchmark — {out_dir.name}", ""]
    a = cfg.get("args", {})
    if a:
        md += [f"Values {a.get('vsize')} B, keys `{a.get('keys')}`, seed {a.get('seed')}, "
               f"budget {a.get('budget')} s per step. All latencies measured on-device per operation.", ""]
    if cfg.get("boards"):
        md += ["## Boards", "", "| port | engine | mac | storage | opt | cpu | wifi | built |", "|---|---|---|---|---|---|---|---|"]
        for p, i in cfg["boards"].items():
            md.append(f"| {p} | {i['engine']} | {i['mac']} | {i['partition_kb']} KB | -{i['opt']} | {i['cpu_mhz']} MHz "
                      f"| {i['wifi']} | {i['built']} |")
        md.append("")
        for wmsg in cfg.get("fairness_warnings", []):
            md.append(f"> ⚠️ {wmsg}")
        md.append("")

    # Correctness first: a fast wrong database is not a result.
    # reopen rows carry ok (bool) and err (an esp_err_t name); the rest carry bad/err counts.
    problems = [r for r in rows
                if (not r.get("ok") if r["step"] == "reopen" else r.get("bad") or r.get("err"))]
    md += ["## Correctness", ""]
    if problems:
        md += ["| variant | n | step | bad | err | first error | first bad index |", "|---|---|---|---|---|---|---|"]
        for r in problems:
            md.append(f"| {r['variant']} | {r['size']} | {r['step']} | {r.get('bad', '–')} | {r.get('err', '–')} "
                      f"| {r.get('first_err', '–')} | {r.get('first_bad_index', '–')} |")
    else:
        md.append("Every value read back matched, every delete and overwrite was honoured, and all data "
                  "survived a reboot.")
    md.append("")

    titles = {
        "fill": "Insert (put new keys)", "read_hit": "Random reads (existing keys)",
        "read_miss": "Random reads (absent keys)", "overwrite": "Overwrite (put existing keys)",
        "delete": "Delete", "verify": "Sequential full read + verify", "verify_boot": "Verify after reboot",
    }
    for step, title in titles.items():
        if not any(k[2] == step for k in by):
            continue
        md += [f"## {title}", "", "ops/s, median / p99 latency. *italic* = stopped at the time budget.", "",
               "| variant | " + " | ".join(f"n={s:,}" for s in sizes) + " |",
               "|---|" + "---|" * len(sizes)]
        for v in variants:
            cells = []
            for s in sizes:
                r = by.get((v, s, step))
                if not r:
                    cells.append("–")
                    continue
                cell = f"{fmt_rate(r['ops_per_s'])} ({fmt_us(r['p50_us'])} / {fmt_us(r['p99_us'])})"
                if r.get("timed_out"):
                    cell = f"*{cell}, {r['done']:,} done*"
                cells.append(cell)
            md.append(f"| {v} | " + " | ".join(cells) + " |")
        md.append("")

    if any(k[2] == "reopen" for k in by):
        md += ["## Boot (mount + replay)", "", "| variant | " + " | ".join(f"n={s:,}" for s in sizes) + " |",
               "|---|" + "---|" * len(sizes)]
        for v in variants:
            cells = [fmt_us(by[(v, s, "reopen")]["open_us"]) if (v, s, "reopen") in by else "–" for s in sizes]
            md.append(f"| {v} | " + " | ".join(cells) + " |")
        md.append("")

    charts = plot(out_dir, rows, variants, sizes, by)
    if charts:
        md += ["## Charts", ""] + [f"![{c.stem}]({c.name})" for c in charts] + [""]

    (out_dir / "summary.md").write_text("\n".join(md))
    print(f"report: {out_dir / 'summary.md'} (+ results.csv{', charts' if charts else ''})")


def plot(out_dir, rows, variants, sizes, by):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    charts = []
    steps = [s for s in ("fill", "read_hit", "overwrite", "delete", "verify") if any(k[2] == s for k in by)]
    if steps and len(sizes) > 1:
        fig, axes = plt.subplots(1, len(steps), figsize=(4 * len(steps), 3.6), squeeze=False)
        for ax, step in zip(axes[0], steps):
            for v in variants:
                pts = [(s, by[(v, s, step)]["ops_per_s"]) for s in sizes if (v, s, step) in by]
                if pts:
                    ax.plot(*zip(*pts), marker="o", label=v)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_title(step)
            ax.set_xlabel("keys")
            ax.grid(True, which="both", alpha=0.3)
        axes[0][0].set_ylabel("ops/s")
        axes[0][-1].legend(fontsize=8)
        fig.tight_layout()
        path = out_dir / "throughput.png"
        fig.savefig(path, dpi=130)
        plt.close(fig)
        charts.append(path)

    # Latency at the largest size every variant finished.
    common = [s for s in sizes if all((v, s, "read_hit") in by for v in variants)]
    if common:
        s = common[-1]
        fig, ax = plt.subplots(figsize=(7, 3.6))
        width = 0.8 / max(1, len(variants))
        for i, v in enumerate(variants):
            vals = [by[(v, s, st)]["p99_us"] if (v, s, st) in by else 0 for st in steps]
            ax.bar([x + i * width for x in range(len(steps))], vals, width, label=v)
        ax.set_xticks([x + width * (len(variants) - 1) / 2 for x in range(len(steps))])
        ax.set_xticklabels(steps)
        ax.set_yscale("log")
        ax.set_ylabel("p99 latency (µs)")
        ax.set_title(f"p99 latency at n={s:,}")
        ax.legend(fontsize=8)
        ax.grid(True, axis="y", which="both", alpha=0.3)
        fig.tight_layout()
        path = out_dir / "latency_p99.png"
        fig.savefig(path, dpi=130)
        plt.close(fig)
        charts.append(path)
    return charts


# ---------------------------------------------------------------- race

RACE_OPS = ("put", "get", "del")


def race_cmd(args):
    opts = dict(ops=args.ops, keys=args.keys, vsize=args.vsize, seed=args.seed,
                blink_put=args.blink["put"], blink_get=args.blink["get"], blink_del=args.blink["del"])
    return "race " + " ".join(f"{k}={v}" for k, v in opts.items() if v)


def cmd_race(args):
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    ports = args.ports or find_ports()
    if not ports:
        sys.exit("no serial ports found (pass --ports)")
    boards = connect_all(ports, out_dir)
    if not boards:
        sys.exit("no benchmark boards answered")
    cmd = race_cmd(args)
    failed = False
    try:
        for port, b in boards.items():
            engine = b.info["engine"]
            if b.info.get("wifi") == "connected":
                print(f"  WARNING: {engine} has WiFi connected; radio work may add jitter")
            print(f"{engine} on {port}: formatting...", flush=True)
            b.format()
            if not args.no_wait:
                input(f"{engine} ready. Start recording, then press Enter for 3-2-1-GO ")
            print(f"{engine}: 3-2-1-GO ({cmd})", flush=True)
            kind, res = b.command(cmd, ("RACE", "RACE_ERR"), args.timeout)
            if kind == "RACE_ERR":
                print(f"{engine}: race rejected: {res.get('error')}")
                failed = True
                continue
            row = {"time": dt.datetime.now().isoformat(timespec="seconds"), "board": port,
                   **{k: b.info.get(k) for k in ("mac", *FAIRNESS_KEYS)}, **res}
            with open(out_dir / "race.jsonl", "a") as f:
                f.write(json.dumps(row) + "\n")
            bad = sum(res[op]["bad"] + res[op]["err"] for op in RACE_OPS)
            print(f"{engine}: FINISHED in {res['wall_us'] / 1e6:.3f} s "
                  f"({res['ops_per_s']:.0f} ops/s){'  !! ' + str(bad) + ' bad/errors' if bad else ''}", flush=True)
            failed |= bad > 0
    finally:
        for b in boards.values():
            b.close()
    write_race_report(out_dir)
    if failed:
        sys.exit(1)


def load_race(out_dir):
    """Latest race per engine, for the config of the most recent race."""
    path = Path(out_dir) / "race.jsonl"
    if not path.exists():
        sys.exit(f"{path} not found")
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if not rows:
        return [], []
    key = lambda r: (r["ops"], r["keys"], r["vsize"], r["seed"])
    want = key(rows[-1])
    latest = {}
    for r in rows:
        if key(r) == want:
            latest[r["engine"]] = r
    skipped = sorted({r["engine"] for r in rows if key(r) != want and r["engine"] not in latest})
    return sorted(latest.values(), key=lambda r: r["wall_us"]), skipped


def write_race_report(out_dir):
    out_dir = Path(out_dir)
    rows, skipped = load_race(out_dir)
    if not rows:
        print("no races to report")
        return
    r0 = rows[0]
    md = [f"# Race — {r0['ops']:,} ops, {r0['keys']:,} keys, {r0['vsize']} B values, seed {r0['seed']}", "",
          "| place | engine | race time | ops/s | put ops/s (p99) | get ops/s (p99) | del ops/s (p99) | correct |",
          "|---|---|---|---|---|---|---|---|"]
    for place, r in enumerate(rows, 1):
        bad = sum(r[op]["bad"] + r[op]["err"] for op in RACE_OPS)
        cells = [f"{fmt_rate(r[op]['ops_per_s'])} ({fmt_us(r[op]['p99_us'])})" for op in RACE_OPS]
        md.append(f"| {place} | {r['engine']} | {r['wall_us'] / 1e6:.2f} s | {fmt_rate(r['ops_per_s'])} | "
                  + " | ".join(cells) + f" | {'yes' if not bad else f'NO ({bad} bad/errors)'} |")
    mix = " / ".join(f"{r0[op]['n']:,} {op}" for op in RACE_OPS)
    md += ["", f"Workload: {mix} in {r0['bursts']} bursts; identical on every engine (same seed).",
           "Latency is measured per operation on the board; race time is wall clock from GO to the last op.", ""]
    for key in FAIRNESS_KEYS:
        values = {r["engine"]: r.get(key) for r in rows}
        if len(set(map(str, values.values()))) > 1:
            md.append(f"> ⚠️ boards differ in {key}: " + ", ".join(f"{e}={v}" for e, v in values.items()))
    if skipped:
        md.append(f"> Not shown (raced with different settings): {', '.join(skipped)}")
    md.append("")

    # Blink rates: the LEDs saturate at 10 blinks/s, so the slowest engine
    # should land around 2/s for the difference to stay visible.
    slow = {op: min(r[op]["n"] / (r["wall_us"] / 1e6) for r in rows) for op in RACE_OPS}
    suggest = {op: max(1, round(v / 2)) for op, v in slow.items()}
    md += ["Suggested `--blink` so the slowest engine blinks about twice a second: `--blink "
           + ",".join(f"{op}={n}" for op, n in suggest.items()) + "`", ""]

    charts = plot_race(out_dir, rows)
    if charts:
        md += [f"![{c.stem}]({c.name})" for c in charts] + [""]
    (out_dir / "race.md").write_text("\n".join(md))
    print("\n".join(md[:len(rows) + 4]))
    print(f"report: {out_dir / 'race.md'}{' (+ charts)' if charts else ''}")


def plot_race(out_dir, rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    plt.rcParams.update({"font.size": 18, "axes.titlesize": 26, "axes.titleweight": "bold"})
    engines = [r["engine"] for r in rows]
    colors = {e: plt.cm.tab10(i) for i, e in enumerate(sorted(engines))}
    charts = []

    # Race time, winner on top. 16:9 at 1920x1080 for the video.
    fig, ax = plt.subplots(figsize=(16, 9))
    secs = [r["wall_us"] / 1e6 for r in rows]
    bars = ax.barh(engines[::-1], secs[::-1], color=[colors[e] for e in engines[::-1]])
    ax.bar_label(bars, labels=[f"{s:.2f} s" for s in secs[::-1]], padding=10)
    ax.set_xlabel("race time (s), lower is better")
    ax.set_title(f"{rows[0]['ops']:,} mixed operations")
    ax.set_xlim(0, max(secs) * 1.15)
    ax.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    charts.append(out_dir / "race_time.png")
    fig.savefig(charts[-1], dpi=120)
    plt.close(fig)

    # Per-op throughput and p99 latency, log scale: engines differ by 100x+.
    for metric, title, label in (("ops_per_s", "Throughput by operation", "ops/s (log), higher is better"),
                                 ("p99_us", "p99 latency by operation", "µs (log), lower is better")):
        fig, ax = plt.subplots(figsize=(16, 9))
        width = 0.8 / len(rows)
        for i, r in enumerate(rows):
            xs = [x + i * width for x in range(len(RACE_OPS))]
            vals = [r[op][metric] for op in RACE_OPS]
            bars = ax.bar(xs, vals, width, label=r["engine"], color=colors[r["engine"]])
            fmt = fmt_rate if metric == "ops_per_s" else fmt_us
            ax.bar_label(bars, labels=[fmt(v) for v in vals], padding=4, fontsize=14)
        ax.set_xticks([x + width * (len(rows) - 1) / 2 for x in range(len(RACE_OPS))])
        ax.set_xticklabels(RACE_OPS)
        ax.set_yscale("log")
        ax.set_ylabel(label)
        ax.set_title(title)
        ax.legend()
        ax.spines[["top", "right"]].set_visible(False)
        fig.tight_layout()
        charts.append(out_dir / f"race_{metric.split('_')[0]}.png")
        fig.savefig(charts[-1], dpi=120)
        plt.close(fig)
    return charts


def blink_arg(s):
    out = {"put": 10, "get": 100, "del": 10}
    for part in s.split(","):
        op, _, n = part.partition("=")
        if op not in out or not n.isdigit() or int(n) < 1:
            raise argparse.ArgumentTypeError(f"expected put=N,get=N,del=N, got {part!r}")
        out[op] = int(n)
    return out


def int_list(s):
    return [int(x.replace("_", "")) for x in s.split(",") if x]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="identify the board on each serial port")
    p.add_argument("--ports", nargs="+", help="serial ports (default: all /dev/ttyUSB* /dev/ttyACM*)")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("run", help="run the benchmark on every board")
    p.add_argument("--ports", nargs="+", help="serial ports (default: all /dev/ttyUSB* /dev/ttyACM*)")
    p.add_argument("--sizes", type=int_list, default=[1000, 10000, 100000],
                   help="comma-separated key counts (default 1000,10000,100000)")
    p.add_argument("--vsize", type=int, default=32, help="value size in bytes (default 32)")
    p.add_argument("--keys", choices=("rand", "seq"), default="rand",
                   help="random 64-bit keys, or 0,1,2,... (default rand)")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--budget", type=int, default=300, help="seconds per step before a variant is capped (default 300)")
    p.add_argument("--max-reads", type=int, default=0, help="cap random-read ops per size (default: n)")
    p.add_argument("--sqlite-batch", type=int_list, default=[1, 100],
                   help="SQLite ops per transaction to compare (default 1,100)")
    p.add_argument("--out", help="output directory (default bench_results/<timestamp>)")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("report", help="rebuild summary.md / results.csv / charts from a results directory")
    p.add_argument("dir")
    p.set_defaults(func=lambda a: write_report(Path(a.dir)))

    p = sub.add_parser("race", help="LED race for filming: format, wait for Enter, race; one board at a time")
    p.add_argument("--ports", nargs="+", help="serial ports (default: all /dev/ttyUSB* /dev/ttyACM*)")
    p.add_argument("--ops", type=int, default=10000, help="operations per race (default 10000)")
    p.add_argument("--keys", type=int, default=0, help="key space, at most 65535 (default ops/4)")
    p.add_argument("--vsize", type=int, default=64, help="value size in bytes (default 64)")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--blink", type=blink_arg, default=blink_arg("put=10"),
                   help="ops per LED blink, e.g. put=10,get=100,del=10 (the default)")
    p.add_argument("--timeout", type=int, default=3600, help="seconds to wait for a race to finish (default 3600)")
    p.add_argument("--no-wait", action="store_true", help="don't wait for Enter before each race")
    p.add_argument("--out", default="bench_results/race",
                   help="results directory; races accumulate here across runs (default bench_results/race)")
    p.set_defaults(func=cmd_race)

    p = sub.add_parser("race-report", help="rebuild race.md and the race charts from a results directory")
    p.add_argument("dir", nargs="?", default="bench_results/race")
    p.set_defaults(func=lambda a: write_race_report(Path(a.dir)))

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
