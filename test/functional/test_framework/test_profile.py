#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lightweight profiling helpers for functional tests."""

from contextlib import contextmanager
import asyncio
from functools import wraps
import inspect
import json
import os
import platform
from pathlib import Path
import sys
import tempfile
from threading import Lock
from time import perf_counter, time
import unittest


PROFILE_ENV_VAR = "BITCOIN_TEST_PROFILE"
PROFILE_FILE_ENV_VAR = "BITCOIN_TEST_PROFILE_FILE"


class Profile:
    def __init__(self):
        self.enabled = os.getenv(PROFILE_ENV_VAR, "") == "1"
        self.output_file = os.getenv(PROFILE_FILE_ENV_VAR)
        self.started_at = time()
        self._lock = Lock()
        self._data = {}
        self._metadata = {}

    def set_metadata(self, **metadata):
        if not self.enabled:
            return
        with self._lock:
            self._metadata.update({k: v for k, v in metadata.items() if v is not None})

    def record_duration(self, name, seconds, calls=1):
        if not self.enabled:
            return
        with self._lock:
            entry = self._data.setdefault(name, {
                "calls": 0,
                "seconds": 0.0,
                "min_seconds": None,
                "max_seconds": 0.0,
            })
            entry["calls"] += calls
            entry["seconds"] += seconds
            entry["min_seconds"] = seconds if entry["min_seconds"] is None else min(entry["min_seconds"], seconds)
            entry["max_seconds"] = max(entry["max_seconds"], seconds)

    @contextmanager
    def section(self, name):
        if not self.enabled:
            yield
            return
        start = perf_counter()
        try:
            yield
        finally:
            self.record_duration(name, perf_counter() - start)

    def write(self, **metadata):
        if not self.enabled:
            return
        if not self.output_file:
            script_name = Path(sys.argv[0]).name
            self.output_file = str(Path.cwd() / "test-profiles" / f"{script_name}.json")
        self.set_metadata(**metadata)
        path = Path(self.output_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        finished_at = time()
        with self._lock:
            data = {
                "version": 1,
                "metadata": {
                    "argv": sys.argv,
                    "elapsed_seconds": finished_at - self.started_at,
                    "profile_file": str(path),
                    "python_version": sys.version,
                    "platform": platform.platform(),
                    "platform_machine": platform.machine(),
                    "platform_system": platform.system(),
                    "started_at": self.started_at,
                    "finished_at": finished_at,
                    **self._metadata,
                },
                "timings": [
                    {
                        "name": name,
                        **entry,
                    }
                    for name, entry in sorted(self._data.items())
                ],
            }
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf8")


_PROFILE = Profile()


def enabled():
    return _PROFILE.enabled


def set_metadata(**metadata):
    _PROFILE.set_metadata(**metadata)


def record_duration(name, seconds, calls=1):
    _PROFILE.record_duration(name, seconds, calls)


def write_profile(**metadata):
    _PROFILE.write(**metadata)


@contextmanager
def profile_section(name):
    with _PROFILE.section(name):
        yield


def profile_function(name, func):
    if not enabled():
        return func
    unwrapped = inspect.unwrap(func)
    if inspect.isgeneratorfunction(unwrapped) or inspect.isasyncgenfunction(unwrapped):
        return func
    if getattr(func, "_profile_wrapped", False):
        return func

    if inspect.iscoroutinefunction(func):
        @wraps(func)
        async def async_wrapper(*args, **kwargs):
            with profile_section(name):
                return await func(*args, **kwargs)

        async_wrapper._profile_wrapped = True
        return async_wrapper

    @wraps(func)
    def wrapper(*args, **kwargs):
        with profile_section(name):
            return func(*args, **kwargs)

    wrapper._profile_wrapped = True
    return wrapper


def profile(name):
    def decorator(func):
        return profile_function(name, func)

    return decorator


class ProfileTest(unittest.TestCase):
    def setUp(self):
        self.profile = Profile()
        self.profile.enabled = True

    def test_record_duration_aggregates(self):
        self.profile.record_duration("section", 0.2)
        self.profile.record_duration("section", 0.4, calls=2)

        section = self.profile._data["section"]
        self.assertEqual(section["calls"], 3)
        self.assertAlmostEqual(section["seconds"], 0.6)
        self.assertEqual(section["min_seconds"], 0.2)
        self.assertEqual(section["max_seconds"], 0.4)

    def test_disabled_profile_records_nothing(self):
        self.profile.enabled = False

        self.profile.record_duration("section", 1)

        self.assertEqual({}, self.profile._data)

    def test_disabled_profile_function_returns_original(self):
        old_enabled = _PROFILE.enabled
        _PROFILE.enabled = False
        try:
            def sample():
                return 1

            self.assertIs(sample, profile_function("sample", sample))
        finally:
            _PROFILE.enabled = old_enabled

    def test_disabled_profile_writes_nothing(self):
        self.profile.enabled = False
        with tempfile.TemporaryDirectory() as tmpdir:
            output_file = Path(tmpdir) / "profile.json"
            self.profile.output_file = str(output_file)

            self.profile.write(status="passed")

            self.assertFalse(output_file.exists())

    def test_profile_function_supports_async_functions(self):
        old_enabled = _PROFILE.enabled
        old_data = _PROFILE._data
        _PROFILE.enabled = True
        _PROFILE._data = {}
        try:
            async def sample():
                return 5

            wrapped = profile_function("async_sample", sample)

            self.assertEqual(asyncio.run(wrapped()), 5)
            self.assertEqual(_PROFILE._data["async_sample"]["calls"], 1)
        finally:
            _PROFILE.enabled = old_enabled
            _PROFILE._data = old_data

    def test_profile_function_skips_context_managers(self):
        old_enabled = _PROFILE.enabled
        _PROFILE.enabled = True
        try:
            @contextmanager
            def sample():
                yield 1

            self.assertIs(sample, profile_function("sample", sample))
        finally:
            _PROFILE.enabled = old_enabled

    def test_section_records_when_wrapped_code_raises(self):
        with self.assertRaises(ValueError):
            with self.profile.section("raising_section"):
                raise ValueError

        self.assertEqual(self.profile._data["raising_section"]["calls"], 1)

    def test_write_records_metadata_and_status(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            output_file = Path(tmpdir) / "profile.json"
            self.profile.output_file = str(output_file)
            self.profile.set_metadata(test_name="unit")
            self.profile.record_duration("section", 0.1)

            self.profile.write(status="passed", exit_code=0)

            data = json.loads(output_file.read_text(encoding="utf8"))
        self.assertEqual(data["version"], 1)
        self.assertEqual(data["metadata"]["status"], "passed")
        self.assertEqual(data["metadata"]["exit_code"], 0)
        self.assertEqual(data["metadata"]["test_name"], "unit")
        self.assertEqual(data["timings"][0]["name"], "section")
