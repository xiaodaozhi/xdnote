"""Check tracked repository hygiene, not product behavior or platform acceptance."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
result = subprocess.run(
    ["git", "ls-files", "-z", "--cached"], cwd=ROOT, check=True, stdout=subprocess.PIPE
)
paths = [part.decode("utf-8", errors="strict") for part in result.stdout.split(b"\0") if part]
errors = []
for name in paths:
    parts = Path(name).parts
    if not parts or any(part in {"build", "target", "dist", "node_modules", "__pycache__"} for part in parts):
        errors.append(f"Generated or cache path is tracked: {name}")
    if parts[0] == "design" and ("exports" in parts or name.lower().endswith(".zip")):
        errors.append(f"Design delivery archive is tracked: {name}")
    if Path(name).name == ".env" or Path(name).name.startswith(".env.") and Path(name).name != ".env.example":
        errors.append(f"Local environment file is tracked: {name}")
for message in errors:
    print(message, file=sys.stderr)
print(f"Repository hygiene: {len(paths)} tracked paths, {len(errors)} errors")
sys.exit(bool(errors))
