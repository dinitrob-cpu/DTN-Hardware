# DTN-Hardware — Ground Station Dashboard
#
# Flask + Server-Sent Events dashboard served on the Pi ground station.
# Talks to the running dtn-node over a Unix socket for state + trace.
# For the MVP we also read the node's stdout trace via a log file; a
# future revision will add a JSON status endpoint to the node app.
#
# Run:  python3 app.py --node-log /var/log/dtn-node.log --port 8000

import argparse
import threading
import time
import os
from flask import Flask, render_template, request, Response, jsonify

app = Flask(__name__)

STATE = {
    "nodes": {},          # node_id -> { last_seen, queue, custody, rssi }
    "trace": [],          # list of recent trace lines
    "log_path": None,
}

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/state")
def state():
    return jsonify(STATE)

@app.route("/send", methods=["POST"])
def send():
    """Send a bundle. For the MVP this prints a CLI command the user can
    paste into the node app's REPL; a later revision will talk to the
    node over a Unix socket directly."""
    dst  = request.form.get("dst", "ROVR")
    cls  = request.form.get("class", "tlm")
    msg  = request.form.get("msg", "")
    cmd  = f"send {dst} {cls} {msg}"
    return jsonify({"cli_command": cmd, "note": "paste into dtn-node --cli"})

@app.route("/trace/stream")
def trace_stream():
    def gen():
        last_pos = 0
        while True:
            if STATE["log_path"] and os.path.exists(STATE["log_path"]):
                with open(STATE["log_path"], "r") as f:
                    f.seek(last_pos)
                    new = f.read()
                    last_pos = f.tell()
                    if new:
                        for line in new.splitlines():
                            yield f"data: {line}\n\n"
            time.sleep(0.5)
    return Response(gen(), mimetype="text/event-stream")

def parse_trace_line(line):
    """Parse a stdout trace line like '[12.34] EART BUNDLE_GENERATED b=... ...'
    into a dict for the dashboard state. Heuristic; not a real parser."""
    try:
        t, node, ev, bid, *rest = line.strip().split(maxsplit=4)
        node_id = node
        STATE["nodes"].setdefault(node_id, {})
        STATE["nodes"][node_id]["last_seen"] = float(t.strip("[]"))
        STATE["nodes"][node_id]["last_event"] = ev
        STATE["trace"].append(line.strip())
        STATE["trace"] = STATE["trace"][-200:]
    except Exception:
        pass

def tail_thread():
    while True:
        if STATE["log_path"] and os.path.exists(STATE["log_path"]):
            try:
                with open(STATE["log_path"], "r") as f:
                    f.seek(0, 2)
                    while True:
                        line = f.readline()
                        if line:
                            parse_trace_line(line)
                        else:
                            time.sleep(0.2)
            except Exception:
                time.sleep(1.0)
        else:
            time.sleep(1.0)

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--node-log", default="/tmp/dtn-node.log")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--host", default="0.0.0.0")
    args = p.parse_args()
    STATE["log_path"] = args.node_log
    threading.Thread(target=tail_thread, daemon=True).start()
    app.run(host=args.host, port=args.port, debug=False)