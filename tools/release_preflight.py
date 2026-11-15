#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report which conditions for a release are still unmet.

    python3 tools/release_preflight.py            # read-only, needs the gh CLI
    python3 tools/release_preflight.py --tag v0.1.0-rc.1
    python3 tools/release_preflight.py --commit <sha>

It changes nothing: it only reads from GitHub (code-scanning alerts, issues, workflow runs,
secret names, variables and the release environment) and from the working tree. The exit
status is 0 when every condition is met and 1 otherwise, so it can be run again after each
fix. The list of conditions is in docs/releasing.md.

A workflow run counts only when it is a run of that workflow on `develop`, finished with
success, and says something about the commit to be released, which is the tip of `develop`
unless --commit names another:

    commit   CI, Docs and the Release dry run must have run on that very commit.
    recent   The scheduled analyses (Nightly, Platform matrix, Static analysis, CodeQL,
             MSVC analysis and the nightly Fuzz) must have run in the last MAX_AGE_DAYS days,
             on that commit or on one of its ancestors.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

import check_release_version

ROOT = Path(__file__).resolve().parent.parent

# Alerts in these trees are in code we build but do not own, so they do not hold a release.
THIRD_PARTY_PREFIXES = (".cache/", "build/", "_deps/", "vcpkg_installed/", "third_party/")

COMMIT = "commit"
RECENT = "recent"

# A scheduled run older than this says nothing about the commit being released.
MAX_AGE_DAYS = 2

BRANCH = "develop"


@dataclass(frozen=True)
class Workflow:
    file: str
    label: str
    rule: str
    # Only runs of these events count. The push runs of fuzz.yml are 60-second runs that
    # concurrency cancels, and a manual run is not the night's run.
    events: tuple[str, ...] | None = None


# All of them run on `develop`; docs.yml does on every push to it.
WORKFLOWS = (
    Workflow("ci.yml", "CI", COMMIT),
    Workflow("docs.yml", "Docs", COMMIT),
    Workflow("release.yml", "Release dry run", COMMIT),
    Workflow("nightly.yml", "Nightly", RECENT),
    Workflow("platform-matrix.yml", "Platform matrix", RECENT),
    Workflow("static-analysis.yml", "Static analysis", RECENT),
    Workflow("codeql.yml", "CodeQL", RECENT),
    Workflow("msvc-analysis.yml", "MSVC analysis", RECENT),
    Workflow("fuzz.yml", "Fuzz, nightly", RECENT, events=("schedule",)),
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


def parse_time(text: str) -> datetime | None:
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def judge_run(
    label: str,
    run: dict | None,
    rule: str = COMMIT,
    target: str | None = None,
    now: datetime | None = None,
    relation: str | None = None,
) -> Result:
    """Whether the newest completed run of a workflow allows the release.

    `rule` is COMMIT (the run must be of `target`) or RECENT (no older than MAX_AGE_DAYS, and of
    `target` or an ancestor of it, which `relation` says: the status GitHub gives when it compares
    the run's commit with `target`). A `target` of None means the commit to release is not known.
    """
    if run is None:
        where = f" on {target[:7]}" if rule == COMMIT and target else ""
        return Result(label, False, f"no completed run found{where}")
    when = run.get("createdAt", "")[:10]
    sha = run.get("headSha") or ""
    text = f"{run.get('conclusion')} ({run.get('event')} on {run.get('headBranch')}, {when}, commit {sha[:7] or 'unknown'})"
    if run.get("conclusion") != "success":
        return Result(label, False, text)
    if target is None:
        return Result(label, None, text + "; the commit to release could not be read")
    if rule == COMMIT:
        if sha != target:
            return Result(label, False, text + f"; ran on an older commit, the release is {target[:7]}")
        return Result(label, True, text)
    created = parse_time(run.get("createdAt", ""))
    current = now or datetime.now(timezone.utc)
    if created is None or current - created > timedelta(days=MAX_AGE_DAYS):
        return Result(label, False, text + f"; older than {MAX_AGE_DAYS} days")
    if sha != target and relation not in ("identical", "ahead"):
        return Result(label, False, text + f"; that commit is not in the history of {target[:7]}")
    return Result(label, True, text)


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


def runs_query(repo: str, workflow: str, branch: str, head_sha: str | None = None, event: str | None = None) -> str:
    """The API path of the newest completed runs of a workflow on a branch.

    `head_sha` limits it to one commit and `event` to one event. The server does the filtering because
    the page holds only 30 runs, and fuzz.yml has a push run for every merge, which can push the
    night's scheduled run off it.
    """
    query = f"repos/{repo}/actions/workflows/{workflow}/runs?status=completed&per_page=30&branch={branch}"
    if head_sha:
        query += f"&head_sha={head_sha}"
    if event:
        query += f"&event={event}"
    return query


def latest_run(
    repo: str, workflow: str, branch: str, head_sha: str | None = None, events: tuple[str, ...] | None = None
) -> dict | None | bool:
    """The newest completed run, None when there is none, False when GitHub could not be read."""
    event = events[0] if events and len(events) == 1 else None
    page = subprocess.run(["gh", "api", runs_query(repo, workflow, branch, head_sha, event)], capture_output=True, text=True, check=False)
    if page.returncode != 0:
        return False
    for run in json.loads(page.stdout).get("workflow_runs", []):
        if events is None or run["event"] in events:
            return {
                "conclusion": run["conclusion"],
                "event": run["event"],
                "headBranch": run["head_branch"],
                "headSha": run["head_sha"],
                "createdAt": run["created_at"],
            }
    return None


def commit_of(repo: str, ref: str) -> str | None:
    done = subprocess.run(["gh", "api", f"repos/{repo}/commits/{ref}", "--jq", ".sha"], capture_output=True, text=True, check=False)
    sha = done.stdout.strip()
    return sha if done.returncode == 0 and len(sha) == 40 else None


def relation_of(repo: str, base: str, target: str) -> str | None:
    """How `target` stands to `base`: identical, ahead (base is an ancestor of target), behind or diverged."""
    done = subprocess.run(
        ["gh", "api", f"repos/{repo}/compare/{base}...{target}", "--jq", ".status"], capture_output=True, text=True, check=False
    )
    return done.stdout.strip() if done.returncode == 0 else None


def collect(repo: str, tag: str | None, commit: str | None = None) -> list[Result]:
    org = repo.split("/")[0]
    results = [judge_version(tag)]
    results += judge_files()
    alerts = gh(f"repos/{repo}/code-scanning/alerts?state=open&ref=refs/heads/develop&per_page=100")
    results.append(Result("Code-scanning alerts", None, "could not be read") if alerts is None else judge_alerts(alerts))
    issues = gh(f"repos/{repo}/issues?state=open&labels=nightly-analysis&per_page=100")
    results.append(Result("Nightly analysis issues", None, "could not be read") if issues is None else judge_issues(issues))
    target = commit_of(repo, commit or BRANCH)
    for workflow in WORKFLOWS:
        label = f"{workflow.label} on {BRANCH}"
        of_commit = target if workflow.rule == COMMIT else None
        run = latest_run(repo, workflow.file, BRANCH, of_commit, workflow.events)
        if run is False:
            results.append(Result(label, None, "could not be read"))
            continue
        relation = None
        if workflow.rule == RECENT and run and target and run["headSha"] != target:
            relation = relation_of(repo, run["headSha"], target)
        results.append(judge_run(label, run, workflow.rule, target, relation=relation))
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
    parser.add_argument("--commit", help="the commit to be tagged, when it is not the tip of develop")
    args = parser.parse_args(argv[1:])
    results = collect(args.repo or default_repo(), args.tag, args.commit)
    print(render(results))
    return 0 if all(r.met for r in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
