#!/usr/bin/env python3
"""nothink_proxy.py — OpenAI-compatible passthrough that disables Qwen "thinking".

WHY. graft's --deep pass sends max_tokens 2048 (per-file summaries) / 8192 (crux + synthesis) at
temperature 0, and has no way to pass chat_template_kwargs — not a flag, not an env var. Qwen3.6 is
a REASONING model, so it spends that budget on a chain-of-thought preamble and is cut off before the
answer: measured on this endpoint as 1001 completions finishing on `length` against 803 on `stop`.

This sits in front of vLLM and injects {"chat_template_kwargs": {"enable_thinking": false}} into
every chat request, which turns the same prompt from a truncated ramble into a clean answer. Only
/v1/chat/completions is rewritten; everything else is forwarded untouched.

    python3 tools/nothink_proxy.py --upstream http://192.168.2.219:8000 --port 8001
"""
import argparse, json, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = "http://192.168.2.219:8000"

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _proxy(self, method):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)) or None
        # Only the chat endpoint carries a chat template; leave /v1/models et al. alone.
        if body and self.path.rstrip("/").endswith("/chat/completions"):
            try:
                payload = json.loads(body)
                kw = payload.setdefault("chat_template_kwargs", {})
                kw.setdefault("enable_thinking", False)   # setdefault: never override a caller
                body = json.dumps(payload).encode()
            except (ValueError, AttributeError):
                pass                                     # not JSON we understand — forward as-is
        req = urllib.request.Request(UPSTREAM + self.path, data=body, method=method)
        for h in ("Content-Type", "Authorization", "Accept"):
            if h in self.headers:
                req.add_header(h, self.headers[h])
        if body is not None:
            req.add_header("Content-Length", str(len(body)))
        try:
            with urllib.request.urlopen(req, timeout=1800) as up:
                data, status, ctype = up.read(), up.status, up.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            data, status, ctype = e.read(), e.code, e.headers.get("Content-Type", "application/json")
        except Exception as e:
            data, status, ctype = json.dumps({"error": str(e)}).encode(), 502, "application/json"
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self): self._proxy("POST")
    def do_GET(self):  self._proxy("GET")
    def log_message(self, *a): pass          # quiet: graft is chatty enough

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", default=UPSTREAM)
    ap.add_argument("--port", type=int, default=8001)
    a = ap.parse_args()
    UPSTREAM = a.upstream.rstrip("/")
    print(f"nothink proxy: 127.0.0.1:{a.port} -> {UPSTREAM}  (enable_thinking=false injected)")
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
