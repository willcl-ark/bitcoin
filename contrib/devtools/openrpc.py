#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

from __future__ import annotations

from typing import Any


def _first_name(names: list[str], fallback: str = "") -> str:
    return names[0] if names else fallback


def _arg_schema(arg: dict[str, Any]) -> dict[str, Any]:
    t = arg["type"]
    inner = arg.get("inner", [])

    if t == "string":
        return {"type": "string"}
    if t == "hex":
        return {"type": "string", "pattern": "^[0-9a-fA-F]*$"}
    if t == "number":
        return {"type": "number"}
    if t == "boolean":
        return {"type": "boolean"}
    if t == "amount":
        return {"oneOf": [{"type": "number"}, {"type": "string"}]}
    if t == "range":
        return {
            "oneOf": [
                {"type": "number"},
                {
                    "type": "array",
                    "prefixItems": [{"type": "number"}, {"type": "number"}],
                    "minItems": 2,
                    "maxItems": 2,
                },
            ]
        }
    if t == "array":
        return {"type": "array", "items": _arg_schema(inner[0]) if inner else {}}
    if t in {"object", "object_named_params"}:
        properties: dict[str, Any] = {}
        required: list[str] = []
        for item in inner:
            if item.get("hidden"):
                continue
            name = _first_name(item.get("names", []))
            properties[name] = _arg_schema(item)
            if item.get("description"):
                properties[name]["description"] = item["description"]
            if item.get("type_str"):
                properties[name]["x-bitcoin-type-str"] = item["type_str"]
            if item.get("also_positional"):
                properties[name]["x-bitcoin-also-positional"] = True
            if not item.get("optional", False):
                required.append(name)
        schema: dict[str, Any] = {
            "type": "object",
            "properties": properties,
            "additionalProperties": False,
        }
        if required:
            schema["required"] = required
        return schema
    if t == "object_user_keys":
        return {"type": "object", "additionalProperties": True}
    raise ValueError(f"Unknown argument type: {t}")


def _result_schema(result: dict[str, Any]) -> dict[str, Any]:
    t = result["type"]
    inner = result.get("inner", [])

    if t in {"string", "string_amount", "hex"}:
        schema: dict[str, Any] = {"type": "string"}
        if t == "hex":
            schema["pattern"] = "^[0-9a-fA-F]*$"
        return schema
    if t in {"number", "number_time"}:
        schema = {"type": "number"}
        if t == "number_time":
            schema["x-bitcoin-unit"] = "unix-time"
        return schema
    if t == "boolean":
        return {"type": "boolean"}
    if t == "none":
        return {"type": "null"}
    if t == "array":
        return {"type": "array", "items": _result_schema(inner[0]) if inner else {}}
    if t == "array_fixed":
        return {
            "type": "array",
            "prefixItems": [_result_schema(item) for item in inner],
            "minItems": len(inner),
            "maxItems": len(inner),
        }
    if t == "object":
        properties: dict[str, Any] = {}
        required: list[str] = []
        for item in inner:
            if item.get("type") == "elision":
                continue
            name = item.get("key_name", "")
            if not name:
                continue
            properties[name] = _result_schema(item)
            if item.get("description"):
                properties[name]["description"] = item["description"]
            if not item.get("optional", False):
                required.append(name)
        schema = {
            "type": "object",
            "properties": properties,
            "additionalProperties": False,
        }
        if required:
            schema["required"] = required
        return schema
    if t == "object_dynamic":
        additional = _result_schema(inner[0]) if inner else {}
        return {"type": "object", "additionalProperties": additional}
    if t in {"any", "elision"}:
        return {}
    raise ValueError(f"Unknown result type: {t}")


def build_openrpc(dump: dict[str, Any]) -> dict[str, Any]:
    version = dump.get("version", "dev")
    command_descriptions = dump.get("commands", dump)
    methods: list[dict[str, Any]] = []

    for method_name, desc in sorted(command_descriptions.items()):
        params = []
        for arg in desc.get("arguments", []):
            if arg.get("hidden"):
                continue
            names = arg.get("names", [])
            param = {
                "name": _first_name(names, "arg"),
                "required": not arg.get("optional", False),
                "schema": _arg_schema(arg),
            }
            if len(names) > 1:
                param["x-bitcoin-aliases"] = names[1:]
            if arg.get("type_str"):
                param["x-bitcoin-type-str"] = arg["type_str"]
            if arg.get("also_positional"):
                param["x-bitcoin-also-positional"] = True
            if arg.get("description"):
                param["description"] = arg["description"]
            params.append(param)

        result_variants = desc.get("results", [])
        if not result_variants:
            result_schema: dict[str, Any] = {}
        elif len(result_variants) == 1:
            result_schema = _result_schema(result_variants[0])
        else:
            one_of = []
            for variant in result_variants:
                schema = _result_schema(variant)
                if variant.get("condition"):
                    schema = {**schema, "description": variant["condition"]}
                one_of.append(schema)
            result_schema = {"oneOf": one_of}

        method = {
            "name": method_name,
            "description": desc.get("description", ""),
            "params": params,
            "result": {"name": "result", "schema": result_schema},
            "x-bitcoin-category": desc.get("category", ""),
        }
        methods.append(method)

    return {
        "openrpc": "1.3.2",
        "info": {
            "title": "Bitcoin Core JSON-RPC",
            "version": version,
            "description": "Autogenerated from Bitcoin Core RPC metadata.",
        },
        "methods": methods,
    }
