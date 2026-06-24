#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

import json
import os
import shlex
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

MAX_RUNTIME = timedelta(hours=5, minutes=30)
MIN_CHECK_AGE = timedelta(days=1)
SILENT_CHECK_NAME = "Periodic silent merge check"
CTEST_SCRIPT = Path(".github/silent-merge-CTestScript.cmake")
RESULTS_FILE = Path("silent-merge-results.json")


def fmt_duration(td):
    total = int(td.total_seconds())
    d, rem = divmod(total, 86400)
    h, rem = divmod(rem, 3600)
    m = rem // 60
    if d:
        return f"{d}d {h}h {m}m" if m else f"{d}d {h}h"
    return f"{h}h {m}m" if m else f"{h}h"


def run(cmd, **kwargs):
    print("+ " + shlex.join(cmd), flush=True)
    kwargs.setdefault("check", True)
    kwargs.setdefault("text", True)
    try:
        return subprocess.run(cmd, **kwargs)
    except Exception as e:
        sys.exit(str(e))


def gh_api_paginated(url, key=None):
    result = run(["gh", "api", "--paginate", url], stdout=subprocess.PIPE)
    items = []
    for page in result.stdout.strip().splitlines():
        data = json.loads(page)
        items.extend(data.get(key) if key else data)
    return items


def get_open_prs(repo):
    return gh_api_paginated(f"repos/{repo}/pulls?state=open&draft=false&per_page=100")


def get_check_runs(repo, head_sha):
    return gh_api_paginated(f"repos/{repo}/commits/{head_sha}/check-runs", key="check_runs")


def has_failing_checks(check_runs):
    return any(
        check.get("conclusion") == "failure" and check.get("name") != SILENT_CHECK_NAME
        for check in check_runs
    )


def latest_passing_check_time(check_runs):
    times = []
    for check in check_runs:
        if check.get("name") == SILENT_CHECK_NAME:
            continue
        if check.get("conclusion") == "success":
            timestamp = check.get("completed_at")
            if timestamp:
                times.append(datetime.fromisoformat(timestamp))
    return max(times) if times else None


def append_result(pr_number, head_sha, conclusion, summary):
    results = json.loads(RESULTS_FILE.read_text()) if RESULTS_FILE.exists() else []
    results.append({"pr_number": pr_number, "head_sha": head_sha, "conclusion": conclusion, "summary": summary})
    RESULTS_FILE.write_text(json.dumps(results, indent=2))


def latest_check_time(check_runs):
    times = []
    for check in check_runs:
        if check.get("name") != SILENT_CHECK_NAME:
            continue
        timestamp = check.get("completed_at") or check.get("started_at")
        if timestamp:
            times.append(datetime.fromisoformat(timestamp))
    return max(times) if times else None


def run_ctest_script(pr_number):
    cmd = [
        "ctest",
        "-S",
        str(CTEST_SCRIPT),
        f"-DCTEST_BUILD_NAME:STRING=silent-merge-pr-{pr_number}",
    ]
    runner_name = os.environ.get("RUNNER_NAME")
    if runner_name:
        cmd.append(f"-DCTEST_SITE:STRING={runner_name}")
    return run(cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def main():
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        sys.exit("GITHUB_REPOSITORY is not set")

    repo_root = Path(__file__).resolve().parent.parent
    os.chdir(repo_root)

    run(["git", "config", "user.email", "action@github.com"])
    run(["git", "config", "user.name", "GitHub Action"])

    max_runtime_seconds = int(MAX_RUNTIME.total_seconds())
    start_time = time.monotonic()

    print("Fetching open PRs...")
    open_prs = get_open_prs(repo)
    print(f"Found {len(open_prs)} open PRs")

    prs = []
    for pr_json in open_prs:
        pr_number = pr_json.get("number")
        head_sha = pr_json.get("head", {}).get("sha")
        check_runs = get_check_runs(repo, head_sha) if head_sha else []
        prs.append(
            {
                "number": pr_number,
                "head_sha": head_sha,
                "created_at": pr_json.get("created_at"),
                "check_runs": check_runs,
                "latest_silent_check": latest_check_time(check_runs),
            }
        )

    candidate_prs = sorted(
        [pr for pr in prs if pr["latest_silent_check"] is None],
        key=lambda pr: pr["created_at"] or "")

    candidate_prs.extend(sorted(
            [pr for pr in prs if pr["latest_silent_check"] is not None],
            key=lambda pr: pr["latest_silent_check"]))

    if candidate_prs:
        print(f"{len(candidate_prs)} candidate PR(s) found")
        print("Processing PRs that have never been checked first and then by oldest check")

    for pr in candidate_prs:
        if time.monotonic() - start_time >= max_runtime_seconds:
            print(f"Reached maximum runtime ({fmt_duration(MAX_RUNTIME)}); stopping before next PR.")
            break
        print(f"Checking PR #{pr['number']}")

        if pr["latest_silent_check"] is not None:
            time_since = datetime.now(tz=pr["latest_silent_check"].tzinfo) - pr["latest_silent_check"]
            print(f"PR #{pr['number']} was last checked {fmt_duration(time_since)} ago")
            print(f"::notice title=Recheck interval PR #{pr['number']}::Last checked {fmt_duration(time_since)} ago")

        if has_failing_checks(pr["check_runs"]):
            print(f"PR #{pr['number']} already has failing check runs, skipping.")
            continue

        latest_pass = latest_passing_check_time(pr["check_runs"])
        if latest_pass is None:
            print(f"PR #{pr['number']} has no passing check runs, skipping.")
            continue
        age = datetime.now(tz=latest_pass.tzinfo) - latest_pass
        if age < MIN_CHECK_AGE:
            print(f"PR #{pr['number']} latest passing check is only {fmt_duration(age)} old (minimum {fmt_duration(MIN_CHECK_AGE)}), skipping.")
            continue

        # Merge the PR head against current master locally. GitHub's pull/{n}/merge ref can be
        # stale, so we cannot rely on it to test against the latest master.
        run(["git", "fetch", "origin", f"pull/{pr['number']}/head:pr-{pr['number']}-head"])
        run(["git", "checkout", "-B", "test-merge", "origin/master"])
        merge_result = run(["git", "merge", "--no-edit", f"pr-{pr['number']}-head"], check=False)
        if merge_result.returncode != 0:
            run(["git", "merge", "--abort"], check=False)
            print(f"PR #{pr['number']} has merge conflicts against current master.")
            append_result(
                pr_number=pr["number"],
                head_sha=pr["head_sha"],
                conclusion="failure",
                summary="This PR does not merge cleanly with the current master. Please rebase and ensure all checks pass.",
            )
            continue

        # Record this PR's result, but keep checking the remaining candidates.
        ci_result = run_ctest_script(pr["number"])
        if ci_result.returncode != 0:
            print(f"PR #{pr['number']} CTest script failed. Output:\n{ci_result.stdout}")
            conclusion = "failure"
            summary = "CMake configure, build, or unit tests failed when this PR was merged with the current master. Please rebase and ensure all checks pass."
        else:
            print(f"PR #{pr['number']} CTest script completed successfully.")
            conclusion = "success"
            summary = "CMake configure, build, and unit tests passed."

        append_result(
            pr_number=pr["number"],
            head_sha=pr["head_sha"],
            conclusion=conclusion,
            summary=summary,
        )

        stats = run(["ccache", "--show-stats"], stdout=subprocess.PIPE, check=False)
        if stats.returncode == 0:
            stats_inline = stats.stdout.strip().replace("\n", "%0A")
            print(f"::notice title=ccache stats PR #{pr['number']}::{stats_inline}")


if __name__ == "__main__":
    main()
