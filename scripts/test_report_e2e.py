"""End-to-end report smoke test:
1. Build C++ JSON report writer test
2. Run it to generate report.json
3. Copy report.json to rust/ for dashboard
4. Start Rust dashboard server
5. Verify /api/report returns live data
6. Stop server and clean up
"""
import shutil
import subprocess
import sys
import time
from pathlib import Path

repo = Path(__file__).resolve().parent.parent
build_dir = repo / "build_stub"
test_exe = build_dir / "Release" / "test_json_report_writer.exe"
rust_dir = repo / "rust"
rust_report = rust_dir / "report.json"

def run(cmd, cwd=None, check=True):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, shell=isinstance(cmd, str))
    if check and result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    return result

def main():
    # Build C++ test
    print("[1/5] Building C++ test...")
    run(["cmake", "--build", str(build_dir), "--config", "Release", "--target", "test_json_report_writer"])
    if not test_exe.exists():
        print(f"Missing {test_exe}", file=sys.stderr)
        sys.exit(1)

    # Clean old reports
    if rust_report.exists():
        rust_report.unlink()

    # Run C++ test to generate report.json in CWD
    print("[2/5] Generating report.json...")
    result = run([str(test_exe)], cwd=repo)
    report_json = repo / "report.json"
    if not report_json.exists():
        print("report.json not generated", file=sys.stderr)
        sys.exit(1)

    # Copy to rust/ so dashboard can find it
    shutil.copy2(report_json, rust_report)

    # Start Rust dashboard server
    print("[3/5] Starting Rust dashboard...")
    server = subprocess.Popen(
        ["cargo", "run", "-p", "igpu_ml_dashboard"],
        cwd=rust_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(6)
        base = "http://127.0.0.1:8765"

        # Hit /api/report
        print("[4/5] Checking /api/report...")
        r = subprocess.run(["curl", "-s", f"{base}/api/report"], capture_output=True, text=True)
        if r.returncode != 0:
            print("curl failed", file=sys.stderr)
            sys.exit(1)

        body = r.stdout
        if '"schema_version"' not in body or '"its_predictor"' not in body:
            print(f"Unexpected report payload: {body[:500]}", file=sys.stderr)
            sys.exit(1)

        print("[5/5] Report endpoint verified.")
        print(body[:220])
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()

    # Cleanup
    if report_json.exists():
        report_json.unlink()
    if rust_report.exists():
        rust_report.unlink()

    print("PASS")

if __name__ == "__main__":
    main()
