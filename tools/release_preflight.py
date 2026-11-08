#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report which conditions for a release are still unmet.

    python3 tools/release_preflight.py            # read-only, needs the gh CLI
    python3 tools/release_preflight.py --tag v0.1.0-rc.1

It changes nothing: it only reads from GitHub (code-scanning alerts, issues, workflow runs,
secret names, variables and the release environment) and from the working tree. The exit
status is 0 when every condition is met and 1 otherwise, so it can be run again after each
fix. The list of conditions is in docs/releasing.md.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import check_release_version

ROOT = Path(__file__).resolve().parent.parent

# Alerts in these trees are in code we build but do not own, so they do not hold a release.
THIRD_PARTY_PREFIXES = (".cache/", "build/", "_deps/", "vcpkg_installed/", "third_party/")

# Workflow file and the name the report gives it.
WORKFLOWS = (
    ("ci.yml", "CI"),
    ("nightly.yml", "Nightly"),
    ("platform-matrix.yml", "Platform matrix"),
    ("docs.yml", "Docs"),
    ("release.yml", "Release dry run"),
)

FILES = ("docs/signing-key.asc", "SECURITY.md")
GPG_SECRETS = ("REPO_GPG_PRIVATE_KEY", "REPO_GPG_PASSPHRASE", "RELEASE_BOT_APP_ID", "RELEASE_BOT_PRIVATE_KEY")
TEST_SIGN_SECRETS = ("WIN_TESTSIGN_PFX_BASE64", "WIN_TESTSIGN_PFX_PASSWORD")


@dataclass(frozen=True)
class Result:
    name: str
    met: bool | None  # None: could not be read
    detail: str


def is_first_party(path: str) -> bool:
    return not path.startswith(THIRD_PARTY_PREFIXES)


def first_party_alerts(alerts: list[dict]) -> list[dict]:
    return [a for a in alerts if is_first_party(a.get("most_recent_instance", {}).get("location", {}).get("path", ""))]


def judge_alerts(alerts: list[dict]) -> Result:
    own = first_party_alerts(alerts)
    outside = len(alerts) - len(own)
    note = f"; {outside} more in third-party trees are ignored" if outside else ""
    if own:
        by_rule: dict[str, int] = {}
        for alert in own:
            by_rule[alert["rule"]["id"]] = by_rule.get(alert["rule"]["id"], 0) + 1
        top = ", ".join(f"{rule} x{count}" for rule, count in sorted(by_rule.items(), key=lambda kv: -kv[1])[:4])
        return Result("Code-scanning alerts", False, f"{len(own)} open in first-party paths ({top}){note}")
    return Result("Code-scanning alerts", True, f"0 open in first-party paths{note}")


def judge_issues(issues: list[dict]) -> Result:
    if issues:
        listed = ", ".join(f"#{i['number']}" for i in issues)
        return Result("Nightly analysis issues", False, f"{len(issues)} open ({listed})")
    return Result("Nightly analysis issues", True, "none open")


def judge_run(label: str, run: dict | None) -> Result:
    if run is None:
        return Result(label, False, "no completed run found")
    when = run.get("createdAt", "")[:10]
    text = f"{run.get('conclusion')} ({run.get('event')} on {run.get('headBranch')}, {when})"
    return Result(label, run.get("conclusion") == "success", text)


def judge_secrets(names: set[str] | None, sign_windows: str | None) -> Result:
    if names is None:
        return Result("Signing secrets", None, "the secret names could not be read (needs organisation admin)")
    wanted = list(GPG_SECRETS) + (list(TEST_SIGN_SECRETS) if sign_windows == "test" else [])
    missing = [n for n in wanted if n not in names]
    if missing:
        return Result("Signing secrets", False, "missing " + ", ".join(missing))
    return Result("Signing secrets", True, f"{len(wanted)} present (SIGN_WINDOWS={sign_windows or 'false'})")


def judge_environment(env: dict | None) -> Result:
    if env is None:
        return Result("release environment", False, "not found")
    reviewers = [r for rule in env.get("protection_rules", []) if rule.get("type") == "required_reviewers" for r in rule["reviewers"]]
    if not reviewers:
        return Result("release environment", False, "exists but has no required reviewer")
    return Result("release environment", True, f"{len(reviewers)} required reviewer(s)")


def judge_files(root: Path = ROOT) -> list[Result]:
    return [
        Result(name, (root / name).is_file(), "present" if (root / name).is_file() else "missing; the owner provides it")
        for name in FILES
    ]


def judge_version(tag: str | None, root: Path = ROOT) -> Result:
    declared = check_release_version.declared_versions(root)
    if len(set(declared.values())) != 1:
        return Result("Project version", False, "files disagree: " + ", ".join(f"{k} {v}" for k, v in declared.items()))
    version = next(iter(declared.values()))
    if tag:
        problems = check_release_version.check(tag, root)
        return Result("Project version", not problems, "; ".join(problems) or f"{tag} matches {version}")
    return Result("Project version", True, f"{version} in CMakeLists.txt and vcpkg.json")


def gh(*args: str) -> object | None:
    """JSON from `gh api`, or None when the call fails (no access, no network)."""
    done = subprocess.run(["gh", "api", "--paginate", *args], capture_output=True, text=True, check=False)
    if done.returncode != 0:
        return None
    # --paginate prints one JSON document per page; join the arrays.
    decoder, text, index, merged = json.JSONDecoder(), done.stdout, 0, []
    while index < len(text):
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text):
            break
        value, index = decoder.raw_decode(text, index)
        if isinstance(value, list):
            merged.extend(value)
        else:
            return value
    return merged


def latest_run(repo: str, workflow: str, branch: str | None) -> dict | None:
    query = f"repos/{repo}/actions/workflows/{workflow}/runs?status=completed&per_page=1"
    if branch:
        query += f"&branch={branch}"
    page = subprocess.run(["gh", "api", query], capture_output=True, text=True, check=False)
    if page.returncode != 0:
        return None
    runs = json.loads(page.stdout).get("workflow_runs", [])
    if not runs:
        return None
    run = runs[0]
    return {
        "conclusion": run["conclusion"],
        "event": run["event"],
        "headBranch": run["head_branch"],
        "createdAt": run["created_at"],
    }


def collect(repo: str, tag: str | None) -> list[Result]:
    org = repo.split("/")[0]
    results = [judge_version(tag)]
    results += judge_files()
    alerts = gh(f"repos/{repo}/code-scanning/alerts?state=open&ref=refs/heads/develop&per_page=100")
    results.append(Result("Code-scanning alerts", None, "could not be read") if alerts is None else judge_alerts(alerts))
    issues = gh(f"repos/{repo}/issues?state=open&labels=nightly-analysis&per_page=100")
    results.append(Result("Nightly analysis issues", None, "could not be read") if issues is None else judge_issues(issues))
    for workflow, label in WORKFLOWS:
        # Docs run for a pull request and for main, never for develop, so any branch will do.
        branch = None if workflow == "docs.yml" else "develop"
        results.append(judge_run(label + (" on develop" if branch else ""), latest_run(repo, workflow, branch)))
    names = gh(f"orgs/{org}/actions/secrets?per_page=100")
    secret_names = None if names is None else {s["name"] for s in (names.get("secrets", []) if isinstance(names, dict) else names)}
    variables = gh(f"repos/{repo}/actions/variables?per_page=100")
    sign = None
    if isinstance(variables, dict):
        sign = next((v["value"] for v in variables.get("variables", []) if v["name"] == "SIGN_WINDOWS"), None)
    elif isinstance(variables, list):
        sign = next((v["value"] for v in variables if v.get("name") == "SIGN_WINDOWS"), None)
    results.append(judge_secrets(secret_names, sign))
    environment = gh(f"repos/{repo}/environments/release")
    results.append(judge_environment(environment if isinstance(environment, dict) else None))
    return results


def render(results: list[Result]) -> str:
    marks = {True: "ok     ", False: "UNMET  ", None: "unknown"}
    return "\n".join(f"{marks[r.met]} {r.name}: {r.detail}" for r in results)


def default_repo() -> str:
    done = subprocess.run(
        ["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"],
        capture_output=True,
        text=True,
        check=False,
    )
    return done.stdout.strip() or "iainchesworthlabs/virtual-serial-studio"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", help="the tag to be pushed, such as v0.1.0-rc.1; checked against the project version")
    parser.add_argument("--repo", help="owner/name; the repository of the working directory by default")
    args = parser.parse_args(argv[1:])
    results = collect(args.repo or default_repo(), args.tag)
    print(render(results))
    return 0 if all(r.met for r in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
