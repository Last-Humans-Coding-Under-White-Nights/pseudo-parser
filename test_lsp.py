#!/usr/bin/env python3
import sys
import os
import json
import subprocess
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

def send_rpc(p, obj):
    msg = json.dumps(obj)
    header = f"Content-Length: {len(msg)}\r\n\r\n"
    p.stdin.write((header + msg).encode("utf-8"))
    p.stdin.flush()

def read_rpc(p):
    line = p.stdout.readline().decode("utf-8", errors="replace")
    if not line:
        return None
    length = 0
    while line.strip() != "":
        if line.startswith("Content-Length:"):
            length = int(line.split(":")[1].strip())
        line = p.stdout.readline().decode("utf-8", errors="replace")
        if not line:
            return None
    if length == 0:
        return None
    content = p.stdout.read(length).decode("utf-8", errors="replace")
    return json.loads(content)

def main():
    if len(sys.argv) < 2:
        print("Usage: test_lsp.py <path-to-clangd_pseudo-plugin> [path-to-clangd]")
        sys.exit(1)

    plugin_path = os.path.abspath(sys.argv[1])
    clangd_bin = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("CLANGD_BIN", "clangd")
    if os.path.exists(clangd_bin):
        clangd_bin = os.path.abspath(clangd_bin)

    # Check if clangd supports -load
    help_out = subprocess.run([clangd_bin, "--help"], capture_output=True, text=True)
    if "-load" not in help_out.stdout and "-load" not in help_out.stderr:
        print(f"clangd at '{clangd_bin}' does not support -load (dynamic plugin support was added on Oct 8, 2026). Skipping live LSP plugin test.")
        return 0

    env = os.environ.copy()
    clangd_dir = os.path.dirname(os.path.abspath(clangd_bin))
    env["PATH"] = clangd_dir + os.pathsep + env.get("PATH", "")

    print(f"Testing clangd ({clangd_bin}) with plugin ({plugin_path})...")
    proc = subprocess.Popen([
        clangd_bin,
        "-enable-config=0",
        "-log=verbose",
        f"-load={plugin_path}"
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

    # 1. Initialize
    send_rpc(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
    init_resp = read_rpc(proc)
    if not init_resp:
        proc.poll()
        stdout_rem = proc.stdout.read().decode("utf-8", errors="replace")
        stderr_rem = proc.stderr.read().decode("utf-8", errors="replace")
        print(f"ERROR: clangd failed to initialize. Exit code: {proc.returncode}")
        print(f"clangd stdout:\n{stdout_rem}")
        print(f"clangd stderr:\n{stderr_rem}")
    assert init_resp and init_resp.get("id") == 1, f"Initialize failed: {init_resp}"
    send_rpc(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
    print("[OK] Clangd initialized with pseudo-parser module")

    # 2. Test syntax error diagnostics on didOpen
    test1_path = os.path.abspath("test1.cpp").replace("\\", "/")
    test1_uri = f"file:///{test1_path.lstrip('/')}"
    test1_code = "int f() {} {}\n"
    send_rpc(proc, {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": test1_uri,
                "languageId": "cpp",
                "version": 1,
                "text": test1_code
            }
        }
    })

    t0 = time.time()
    diags1 = []
    while time.time() - t0 < 5:
        msg = read_rpc(proc)
        if not msg:
            break
        if msg.get("method") == "textDocument/publishDiagnostics":
            diags1.extend(msg.get("params", {}).get("diagnostics", []))
            if any("syntax error" in d.get("message", "") for d in diags1):
                break
    print(f"[OK] Received {len(diags1)} diagnostics for test1 in {time.time()-t0:.3f}s:")
    for d in diags1:
        print(f"   - {d.get('range')}: {d.get('message')}")
    assert any("syntax error" in d.get("message", "") for d in diags1), "Expected syntax error diagnostic"

    # 3. Test preprocessor recovery diagnostics on didOpen
    test2_path = os.path.abspath("test2.cpp").replace("\\", "/")
    test2_uri = f"file:///{test2_path.lstrip('/')}"
    test2_code = "#ifdef X\nint f() {\n#elif Y\nint g()\n#else\nint ff(*\n#endif\n}\n"
    send_rpc(proc, {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": test2_uri,
                "languageId": "cpp",
                "version": 1,
                "text": test2_code
            }
        }
    })

    t0 = time.time()
    diags2 = []
    while time.time() - t0 < 5:
        msg = read_rpc(proc)
        if not msg:
            break
        if msg.get("method") == "textDocument/publishDiagnostics":
            diags2.extend(msg.get("params", {}).get("diagnostics", []))
            if len(diags2) > 0:
                break
    print(f"[OK] Received {len(diags2)} diagnostics for test2 in {time.time()-t0:.3f}s:")
    for d in diags2:
        print(f"   - {d.get('range')}: {d.get('message')}")
    assert len(diags2) > 0, "Expected diagnostics for unbalanced conditional code"

    # 4. Shutdown
    send_rpc(proc, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}})
    read_rpc(proc)
    send_rpc(proc, {"jsonrpc": "2.0", "method": "exit", "params": {}})
    proc.wait(timeout=5)
    print(f"[OK] Clangd cleanly terminated with exit code {proc.returncode}")
    assert proc.returncode == 0, f"Expected 0 exit code, got {proc.returncode}"
    print("All LSP diagnostics tests passed!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
