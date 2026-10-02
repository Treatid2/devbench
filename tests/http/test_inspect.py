"""Tests for the `inspect` tool (live game-state reads)."""

from __future__ import annotations

import numbers
import json
import math
import os
from urllib.parse import urlsplit
import uuid

import pytest

from conftest import Client, require_enum, require_tool


def _is_number(v) -> bool:
    return isinstance(v, numbers.Number) and not isinstance(v, bool)


@pytest.fixture
def inspect(tool_schema):
    return require_tool(tool_schema, "inspect")


def test_state(client, inspect):
    require_enum(inspect, "kind", "state")
    body = client.ok("inspect", {"kind": "state"})
    assert isinstance(body, dict), body
    assert body.get("plugin") == "devbench", body
    assert "version" in body, body
    assert isinstance(body.get("vr"), bool), body
    assert isinstance(body.get("playerLoaded"), bool), body
    assert "frame" in body, body


def test_vm(client, inspect):
    require_enum(inspect, "kind", "vm")
    body = client.ok("inspect", {"kind": "vm"})
    assert body.get("available") is True, body
    assert isinstance(body.get("loadedTypes"), int) and body["loadedTypes"] > 0, body
    for field in ("attachedScripts", "arrays", "runningStacks", "frozenStacks"):
        assert isinstance(body.get(field), int), (field, body)
    assert isinstance(body.get("overstressed"), bool), body


@pytest.mark.requires_player
def test_scene(client, inspect):
    require_enum(inspect, "kind", "scene")
    body = client.ok("inspect", {"kind": "scene"})
    assert body.get("playerLoaded") is True, body

    cell = body.get("cell")
    assert isinstance(cell, dict), body
    assert "formId" in cell and "formType" in cell and "name" in cell, cell

    pos = body.get("position")
    assert isinstance(pos, list) and len(pos) == 3, body
    assert all(_is_number(c) for c in pos), pos

    hour = body.get("gameHour")
    assert _is_number(hour) and 0 <= hour < 24, body

    assert isinstance(body.get("weather"), dict), body


@pytest.mark.requires_player
def test_refs_player_by_formid(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok("inspect", {"kind": "refs", "formId": "0x14"})
    assert body.get("count") == 1, body
    refs = body.get("refs")
    assert isinstance(refs, list) and len(refs) == 1, body
    ref = refs[0]
    assert ref.get("formId") == "0x00000014", ref
    assert ref.get("base", {}).get("formType") == "NPC_", ref
    actor = ref.get("actor")
    assert isinstance(actor, dict), ref
    assert _is_number(actor.get("health")), actor
    assert _is_number(actor.get("level")), actor
    rotation = ref.get("rotation")
    assert isinstance(rotation, list) and len(rotation) == 3, ref
    assert all(_is_number(v) for v in rotation), ref


@pytest.fixture(scope="module")
def mesh_qualification():
    """Explicit caller-owned positive fixture; never bootstrap or discover a game.

    Missing opt-in skips this whole capability, not an empty optional-field loop.
    Once configured, a missing field/capability/known reference is a failure.
    """
    raw = os.environ.get("DEVBENCH_MESH_FIXTURE")
    if raw is None:
        pytest.skip("mesh capability NOT QUALIFIED: DEVBENCH_MESH_FIXTURE known-positive fixture absent")
    assert len(raw) <= 4096, "mesh fixture must be a small JSON identity contract"
    fixture = json.loads(raw)
    assert isinstance(fixture, dict), fixture
    for field in ("formId", "model", "cellFormId"):
        assert isinstance(fixture.get(field), str) and fixture[field], (field, fixture)
    assert type(fixture.get("pid")) is int and fixture["pid"] > 0, fixture
    assert type(fixture.get("vr")) is bool, fixture
    url = os.environ.get("DEVBENCH_URL", "").rstrip("/")
    parts = urlsplit(url)
    assert (parts.scheme == "http" and parts.hostname in {"localhost", "127.0.0.1", "::1"}
            and parts.port is not None and parts.path == "" and not parts.query and not parts.fragment
            and parts.username is None and parts.password is None), "explicit loopback DEVBENCH_URL required"
    client = Client(url)
    state = client.ok("inspect", {"kind": "state"})
    assert state.get("plugin") == "devbench" and state.get("playerLoaded") is True, state
    assert state.get("pid") == fixture["pid"] and state.get("vr") is fixture["vr"], state
    assert state.get("port") == parts.port and isinstance(state.get("exe"), str) and state["exe"], state
    binding = tuple(state.get(field) for field in ("pid", "port", "exe", "vr", "version"))

    def call(args):
        current = client.ok("inspect", {"kind": "state"})
        assert current.get("playerLoaded") is True, current
        assert tuple(current.get(field) for field in ("pid", "port", "exe", "vr", "version")) == binding, current
        body = client.ok("inspect", {"kind": "refs", **args})
        after = client.ok("inspect", {"kind": "state"})
        assert tuple(after.get(field) for field in ("pid", "port", "exe", "vr", "version")) == binding, after
        return body

    return fixture, call


def test_refs_enumerate_reports_cell_and_model(mesh_qualification):
    fixture, call = mesh_qualification
    body = call({"formId": fixture["formId"]})
    refs = body.get("refs")
    assert isinstance(refs, list) and len(refs) == 1, body
    ref = refs[0]
    assert int(ref["formId"], 16) == int(fixture["formId"], 16), ref
    assert isinstance(ref.get("model"), str) and ref["model"].lower() == fixture["model"].lower(), ref
    assert isinstance(ref.get("cell"), dict), ref
    assert int(ref["cell"]["formId"], 16) == int(fixture["cellFormId"], 16), ref
    bounds = ref.get("bounds")
    assert isinstance(bounds, dict), ref
    for field in ("min", "max"):
        values = bounds.get(field)
        assert isinstance(values, list) and len(values) == 3, bounds
        assert all(_is_number(value) and math.isfinite(value) for value in values), bounds
    assert all(lo <= hi for lo, hi in zip(bounds["min"], bounds["max"])), bounds
    assert any(lo < hi for lo, hi in zip(bounds["min"], bounds["max"])), bounds
    # Enumerating must also publish the known fixture, not just the direct-ID path.
    found = call({"model": fixture["model"], "limit": 1000})
    assert found.get("truncated") is False, "choose a fixture with fewer than1000 model matches"
    assert any(int(item["formId"], 16) == int(fixture["formId"], 16) for item in found.get("refs", [])), found


def test_refs_model_filter(mesh_qualification):
    fixture, call = mesh_qualification
    needle = fixture["model"].swapcase()
    body = call({"model": needle, "limit": 1000})
    refs = body.get("refs")
    assert isinstance(refs, list) and refs and body.get("truncated") is False, body
    assert any(int(ref["formId"], 16) == int(fixture["formId"], 16) for ref in refs), body
    for ref in refs:
        assert isinstance(ref.get("model"), str) and needle.lower() in ref["model"].lower(), ref
    missing = call({"model": f"__devbench_no_match_{uuid.uuid4().hex}__", "limit": 1})
    assert missing.get("refs") == [] and missing.get("count") == 0, missing


@pytest.mark.requires_player
def test_refs_actors_in_radius(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok(
        "inspect",
        {"kind": "refs", "formType": "Actor", "radius": 100000, "limit": 200},
    )
    assert isinstance(body.get("count"), int) and body["count"] >= 1, body
    refs = body.get("refs")
    assert isinstance(refs, list) and refs, body

    # Every ref that is actually an actor must carry an `actor` snapshot.
    for ref in refs:
        ftype = (ref.get("formType") or "")
        btype = (ref.get("base", {}) or {}).get("formType", "")
        if "ACHR" in ftype or "ACHR" in btype or "NPC_" in btype:
            assert isinstance(ref.get("actor"), dict), ref

    assert any((r.get("base", {}) or {}).get("name") for r in refs), \
        "expected at least one actor ref with a base.name"


@pytest.mark.requires_player
def test_refs_enumerate(client, inspect):
    require_enum(inspect, "kind", "refs")
    body = client.ok("inspect", {"kind": "refs"})
    assert isinstance(body.get("count"), int) and body["count"] > 0, body
    assert isinstance(body.get("refs"), list), body
    assert isinstance(body.get("truncated"), bool), body


def test_mods(client, inspect):
    require_enum(inspect, "kind", "mods")
    body = client.ok("inspect", {"kind": "mods"})
    assert isinstance(body, dict), body
    for field in ("count", "lightCount", "total"):
        assert isinstance(body.get(field), int), (field, body)
    assert body["total"] == body["count"] + body["lightCount"], body

    plugins = body.get("plugins")
    light = body.get("lightPlugins")
    assert isinstance(plugins, list) and isinstance(light, list), body
    assert len(plugins) == body["count"], body
    assert len(light) == body["lightCount"], body

    # Skyrim.esm is always present (full plugin, load-order index 0) once data is loaded.
    for p in plugins:
        assert isinstance(p.get("index"), int), p
        assert isinstance(p.get("name"), str) and p["name"], p
    names = [p["name"].lower() for p in plugins]
    assert "skyrim.esm" in names, names


@pytest.mark.requires_player
def test_player(client, inspect):
    require_enum(inspect, "kind", "player")
    body = client.ok("inspect", {"kind": "player"})
    assert body.get("playerLoaded") is True, body
    assert isinstance(body.get("level"), int), body
    assert _is_number(body.get("gold")), body

    avs = body.get("actorValues")
    assert isinstance(avs, dict), body
    for av in ("health", "magicka", "stamina", "carryWeight"):
        slot = avs.get(av)
        assert isinstance(slot, dict), (av, body)
        assert _is_number(slot.get("current")) and _is_number(slot.get("max")), (av, slot)

    equipped = body.get("equipped")
    assert isinstance(equipped, dict), body
    for hand in ("right", "left", "ammo"):
        assert hand in equipped, equipped  # may be null when nothing is equipped


@pytest.mark.requires_player
def test_inventory_player(client, inspect):
    require_enum(inspect, "kind", "inventory")
    body = client.ok("inspect", {"kind": "inventory"})
    assert isinstance(body, dict), body
    owner = body.get("owner")
    assert isinstance(owner, dict) and owner.get("formId") == "0x00000014", body
    assert isinstance(body.get("count"), int), body
    assert isinstance(body.get("truncated"), bool), body
    items = body.get("items")
    assert isinstance(items, list), body
    assert len(items) == body.get("returned"), body
    for it in items:
        assert isinstance(it.get("formId"), str), it
        assert isinstance(it.get("count"), int) and it["count"] > 0, it


@pytest.mark.requires_player
def test_inventory_formtype_filter(client, inspect):
    require_enum(inspect, "kind", "inventory")
    body = client.ok("inspect", {"kind": "inventory", "formType": "Armor", "limit": 50})
    assert isinstance(body.get("items"), list), body
    for it in body["items"]:
        assert "armo" in (it.get("formType") or "").lower(), it


@pytest.mark.requires_player
def test_quests(client, inspect):
    require_enum(inspect, "kind", "quests")
    body = client.ok("inspect", {"kind": "quests"})
    assert isinstance(body, dict), body
    assert isinstance(body.get("count"), int), body
    assert isinstance(body.get("truncated"), bool), body
    quests = body.get("quests")
    assert isinstance(quests, list), body
    assert len(quests) == body.get("returned"), body
    for q in quests:
        assert isinstance(q.get("formId"), str), q
        assert isinstance(q.get("stage"), int), q
        assert isinstance(q.get("active"), bool) and isinstance(q.get("completed"), bool), q
        objs = q.get("objectives")
        assert isinstance(objs, list), q
        for o in objs:
            assert isinstance(o.get("index"), int), o
            assert o.get("state") in {"displayed", "completed", "failed", "dormant"}, o


@pytest.mark.requires_player
def test_effects(client, inspect):
    require_enum(inspect, "kind", "effects")
    body = client.ok("inspect", {"kind": "effects"})
    assert isinstance(body, dict), body
    target = body.get("target")
    assert isinstance(target, dict) and target.get("formId") == "0x00000014", body
    assert isinstance(body.get("count"), int), body
    effects = body.get("activeEffects")
    assert isinstance(effects, list), body
    assert len(effects) == body["count"], body
    for e in effects:
        assert _is_number(e.get("magnitude")), e
        assert _is_number(e.get("duration")), e
        assert _is_number(e.get("elapsed")), e


def test_extensions_list_and_dispatch(client, inspect):
    # RegisterToolExtension lets a mod add a custom inspect kind. The host registers a
    # `devbench.selftest` inspect extension through the public C-ABI (the ping pattern),
    # so `kind=extensions` lists it and `kind=devbench.selftest` routes to it and echoes.
    require_enum(inspect, "kind", "extensions")
    listed = client.ok("inspect", {"kind": "extensions"})
    exts = listed.get("extensions")
    assert isinstance(exts, list), listed
    kinds = [e.get("kind") for e in exts]
    if "devbench.selftest" not in kinds:
        pytest.skip("devbench.selftest inspect extension not registered on this build")
    assert isinstance(next(e for e in exts if e["kind"] == "devbench.selftest").get("descriptor"), dict), exts

    body = client.ok("inspect", {"kind": "devbench.selftest", "foo": 7})
    assert body.get("invoked") is True, body
    assert body.get("echo", {}).get("foo") == 7, body


def test_kind_enum_includes_registered_extension(client, inspect):
    # The host registers a `devbench.selftest` inspect extension via RegisterToolExtension. The
    # inspect tool's `kind` enum is REBUILT to include registered keys, so they're discoverable in
    # tools/list (first-call-correct) instead of only via a kind=extensions round-trip.
    enum = inspect.get("inputSchema", {}).get("properties", {}).get("kind", {}).get("enum", [])
    assert isinstance(enum, list) and enum, inspect
    if "devbench.selftest" not in enum:
        pytest.skip("devbench.selftest inspect extension not registered on this build")
    assert "devbench.selftest" in enum, enum
    # and the static built-in kinds are still present
    assert {"state", "mods", "refs", "extensions"}.issubset(set(enum)), enum


def test_unknown_kind_400(client, inspect):
    require_enum(inspect, "kind", "extensions")  # feature present → unknown kind is a clean 400
    status, body = client.call("inspect", {"kind": "NoSuchKindXYZ"})
    assert status == 400, (status, body)
    assert isinstance(body, dict) and "error" in body, body
