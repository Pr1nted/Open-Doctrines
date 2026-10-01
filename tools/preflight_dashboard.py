#!/usr/bin/env python3
"""The preflight runs, in a browser, the way CI shows them.

    tools/preflight_dashboard.py --serve [--port 8787]   open it
    tools/preflight_dashboard.py                          print the last run

WHY A PAGE AND NOT JUST THE TERMINAL OUTPUT

tools/preflight.sh prints a perfectly good report, and that report is gone the
moment the terminal scrolls. The questions that actually get asked afterwards
are "did this used to pass", "which stage is it on now", and "what did the
packages stage say the last three times" -- and a scrollback answers none of
them. Every run is kept on disk under build/preflight/runs/; this reads them.

It deliberately does what GitHub's own run list does and no more: runs newest
first, each with its stages, each stage openable to its log. There is no
database, no build step and no dependency -- the standard library serves the
page and the runs are read off disk on every request, so a run that finishes
while the page is open appears on the next refresh.

READING, NEVER WRITING. This never starts, stops, retries or deletes a run.
A dashboard that can launch things is a dashboard that can launch things by
accident, and the one here is bound to localhost with a pipeline that drives
six virtual machines behind it.
"""
import html
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(os.environ.get("OD_PREFLIGHT_LOGS", os.path.join(ROOT, "build", "preflight")), "runs")

STATUS_ORDER = {"fail": 0, "running": 1, "skip": 2, "pass": 3}


def read_tsv(path, width):
    """A list of rows padded to `width`. Missing file is no rows, not an error."""
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                parts = line.split("\t")
                out.append((parts + [""] * width)[:width])
    except FileNotFoundError:
        pass
    return out


def load_run(d):
    meta = {k: v for k, v in read_tsv(os.path.join(d, "meta.tsv"), 2)}
    stages = []
    for name, status, detail, desc in read_tsv(os.path.join(d, "stages.tsv"), 4):
        stages.append({"name": name, "status": status, "detail": detail,
                       "desc": desc, "log": name + ".log"})

    # A `current` file means the shell is inside a stage right now. It is
    # removed when the stage ends, so its presence is the liveness signal --
    # no heartbeat, no timestamps to get wrong.
    cur = read_tsv(os.path.join(d, "current"), 2)
    if cur:
        stages.append({"name": cur[0][0], "status": "running", "detail": "",
                       "desc": cur[0][1], "log": cur[0][0] + ".log"})

    status = meta.get("status", "running")
    # A run whose shell was killed leaves status=running forever. Anything
    # untouched for five minutes with no stage in flight is reported as what
    # it is rather than as a run that is somehow still going.
    if status == "running" and not cur:
        age = time.time() - os.path.getmtime(d)
        if age > 300:
            status = "abandoned"

    started = int(meta.get("started", 0) or 0)
    finished = int(meta.get("finished", 0) or 0)
    return {
        "id": meta.get("id", os.path.basename(d)),
        "dir": d,
        "commit": meta.get("commit", ""),
        "subject": meta.get("subject", ""),
        "branch": meta.get("branch", ""),
        "host": meta.get("host", ""),
        "started": started,
        "seconds": (finished - started) if finished else (int(time.time()) - started if started else 0),
        "status": status,
        "stages": stages,
    }


def all_runs(limit=50):
    try:
        names = sorted(os.listdir(RUNS), reverse=True)
    except FileNotFoundError:
        return []
    return [load_run(os.path.join(RUNS, n)) for n in names[:limit]
            if os.path.isdir(os.path.join(RUNS, n))]


# ── the page ────────────────────────────────────────────────────────────────
# One file, no assets, no network. The colours are the only thing doing any
# work here: a wall of grey text is exactly what the terminal already gives.

PAGE = """<!doctype html><html lang=en><head><meta charset=utf-8>
<title>preflight</title>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
:root{--bg:#f6f8fa;--fg:#1f2328;--dim:#59636e;--line:#d1d9e0;--card:#fff;
      --pass:#1a7f37;--fail:#cf222e;--skip:#9a6700;--run:#0969da}
@media(prefers-color-scheme:dark){:root{--bg:#0d1117;--fg:#e6edf3;--dim:#9198a1;
      --line:#3d444d;--card:#151b23;--pass:#3fb950;--fail:#f85149;--skip:#d29922;--run:#4493f8}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
 font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif}
header{border-bottom:1px solid var(--line);padding:14px 20px;display:flex;
 align-items:center;gap:12px;background:var(--card);position:sticky;top:0;z-index:2}
h1{font-size:15px;margin:0;font-weight:600}
.sub{color:var(--dim);font-size:12px}
main{max-width:980px;margin:0 auto;padding:20px}
.run{background:var(--card);border:1px solid var(--line);border-radius:6px;margin-bottom:12px}
.rh{display:flex;align-items:center;gap:10px;padding:12px 14px;cursor:pointer}
.rh:hover{background:rgba(127,127,127,.06)}
.dot{width:10px;height:10px;border-radius:50%;flex:none}
.pass .dot{background:var(--pass)}.fail .dot{background:var(--fail)}
.skip .dot,.partial .dot{background:var(--skip)}
.running .dot{background:var(--run);animation:p 1.2s ease-in-out infinite}
.abandoned .dot{background:var(--dim)}
@keyframes p{50%{opacity:.25}}
.title{font-weight:600;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.meta{color:var(--dim);font-size:12px;white-space:nowrap}
code{font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}
.stages{border-top:1px solid var(--line);padding:4px 0;display:none}
.run.open .stages{display:block}
.st{display:flex;align-items:center;gap:10px;padding:7px 14px 7px 18px}
.st:hover{background:rgba(127,127,127,.06)}
.st .name{font-weight:500;width:110px;flex:none}
.st .desc{color:var(--dim);flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.st a{color:var(--run);text-decoration:none;font-size:12px}
.st a:hover{text-decoration:underline}
.empty{color:var(--dim);text-align:center;padding:60px 20px}
pre{background:var(--card);border:1px solid var(--line);border-radius:6px;
 padding:14px;overflow:auto;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
a.back{color:var(--run);text-decoration:none}
</style></head><body>
<header><h1>preflight</h1><span class=sub id=sub></span></header>
<main id=main><div class=empty>loading</div></main>
<script>
const ago=s=>{if(!s)return'';const d=Date.now()/1000-s;
 for(const[n,v]of[['d',86400],['h',3600],['m',60]])if(d>=v)return Math.floor(d/v)+n+' ago';
 return 'just now'};
const dur=s=>s>=60?Math.floor(s/60)+'m '+(s%60)+'s':s+'s';
const esc=t=>{const d=document.createElement('div');d.textContent=t;return d.innerHTML};
let open=new Set();
async function draw(){
 const r=await fetch('api/runs');const runs=await r.json();
 document.getElementById('sub').textContent=runs.length?runs.length+' runs':'';
 const m=document.getElementById('main');
 if(!runs.length){m.innerHTML='<div class=empty>No runs yet. <code>tools/preflight.sh</code></div>';return}
 m.innerHTML=runs.map(run=>{
  const cls=run.status==='passed'?'pass':run.status==='failed'?'fail':run.status;
  const o=open.has(run.id)?' open':'';
  const stages=run.stages.map(s=>`<div class="st">
    <span class=dot style="background:var(--${s.status==='pass'?'pass':s.status==='fail'?'fail':s.status==='skip'?'skip':'run'})"></span>
    <span class=name>${esc(s.name)}</span>
    <span class=desc>${esc(s.desc||'')}</span>
    <span class=meta>${esc(s.detail||'')}</span>
    <a href="log?run=${encodeURIComponent(run.id)}&name=${encodeURIComponent(s.name)}">log</a>
   </div>`).join('');
  return `<div class="run ${cls}${o}" data-id="${esc(run.id)}">
   <div class=rh>
    <span class=dot></span>
    <span class=title>${esc(run.subject||run.id)}</span>
    <span class=meta><code>${esc(run.commit)}</code> &middot; ${esc(run.host)} &middot; ${dur(run.seconds)} &middot; ${ago(run.started)}</span>
   </div><div class=stages>${stages||'<div class=st><span class=desc>no stages recorded</span></div>'}</div></div>`
 }).join('');
 m.querySelectorAll('.rh').forEach(h=>h.onclick=()=>{
  const el=h.parentElement,id=el.dataset.id;
  el.classList.toggle('open');
  el.classList.contains('open')?open.add(id):open.delete(id);
 });
}
draw();setInterval(draw,4000);
</script></body></html>"""


def log_page(run_id, name, body):
    return ("<!doctype html><html lang=en><head><meta charset=utf-8><title>%s &middot; %s</title>"
            "<style>body{margin:0;background:#0d1117;color:#e6edf3;font:13px/1.6 ui-monospace,"
            "SFMono-Regular,Menlo,monospace}header{padding:12px 18px;border-bottom:1px solid #3d444d;"
            "font-family:-apple-system,sans-serif}a{color:#4493f8;text-decoration:none}"
            "pre{padding:18px;margin:0;white-space:pre-wrap;word-break:break-word}</style></head>"
            "<body><header><a href='.'>&larr; preflight</a> &nbsp; <b>%s</b> &middot; %s</header>"
            "<pre>%s</pre></body></html>"
            % (html.escape(name), html.escape(run_id), html.escape(name),
               html.escape(run_id), html.escape(body)))


class Handler(BaseHTTPRequestHandler):
    def _send(self, body, ctype="text/html; charset=utf-8", code=200):
        raw = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path, _, query = self.path.partition("?")
        args = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        from urllib.parse import unquote
        args = {k: unquote(v) for k, v in args.items()}

        if path in ("/", "/index.html"):
            return self._send(PAGE)
        if path == "/api/runs":
            return self._send(json.dumps(all_runs()), "application/json")
        if path == "/log":
            run_id, name = args.get("run", ""), args.get("name", "")
            # A run id and a stage name both come off the query string, so
            # both are checked against what is actually on disk rather than
            # being joined into a path. ../ in either is then simply a name
            # that does not exist.
            run = next((r for r in all_runs(500) if r["id"] == run_id), None)
            if not run or not any(s["name"] == name for s in run["stages"]):
                return self._send(log_page(run_id, name, "no such log"), code=404)
            try:
                with open(os.path.join(run["dir"], name + ".log"),
                          encoding="utf-8", errors="replace") as fh:
                    body = fh.read()
            except OSError as e:
                body = "could not read it: %s" % e
            return self._send(log_page(run_id, name, body or "(empty)"))
        self._send("<h1>404</h1>", code=404)

    def log_message(self, *a):
        pass          # the server's own access log is noise here


def print_last():
    runs = all_runs(1)
    if not runs:
        print("no runs yet -- tools/preflight.sh")
        return 1
    r = runs[0]
    print("%s  %s  %s  %s" % (r["id"], r["status"], r["commit"], r["host"]))
    for s in r["stages"]:
        print("   %-10s %-8s %s" % (s["name"], s["status"], s["detail"]))
    return 0 if r["status"] in ("passed", "partial") else 1


def main():
    argv = sys.argv[1:]
    if "--serve" not in argv:
        return print_last()
    port = int(argv[argv.index("--port") + 1]) if "--port" in argv else 8787
    # localhost only. This reads a directory that describes six virtual
    # machines; it is nobody else's business and it is not hardened for them.
    srv = HTTPServer(("127.0.0.1", port), Handler)
    print("preflight dashboard: http://127.0.0.1:%d   (ctrl-c to stop)" % port)
    print("reading %s" % RUNS)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
