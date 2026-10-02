#!/usr/bin/env python3
"""Inspect a placed reference and optionally move/frame it in an owned test session.

Default is inspection only. --apply explicitly permits player/camera mutation:
    python tests/http/examples/goto_mesh.py --formid 0x1a3f2 --runtime vr
    python tests/http/examples/goto_mesh.py --formid 0x1a3f2 --port 8921 --apply

DEVBENCH_URL is authoritative; otherwise discovery must select exactly one
verified instance. The selected pid/port/exe/vr/version is printed and rechecked.
Only run --apply when you own the loaded development session; this manual
example does not acquire an environment lease or replace a runtime controller.
Do not interfere with another task or replay a timed-out mutation blindly.

Uses existing inspect/Papyrus/camera/capture APIs. MoveTo callback receipt is
followed by loaded/context/pose/target readbacks, not a guessed settling sleep.
POV is read back after an observed game frame; that is NOT proof of GPU
presentation, pixel quality or SE/VR visual parity. Bounds are a local-extents
distance heuristic, not projected fitting, collision/occlusion/clipping or
aspect-ratio compensation. Character/text input is unrelated.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
import re
import sys
import time
from urllib.parse import urlsplit, urlunsplit

import requests


def finite_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def vector(value) -> list[float]:
    if not isinstance(value, list) or len(value) != 3 or not all(finite_number(x) for x in value):
        raise RuntimeError(f"expected a finite three-component vector, got {value!r}")
    return value


def form_id(value) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", value):
        raise RuntimeError("missing or malformed native FormID")
    return f"0x{int(value, 16):08x}"


def form_record(value) -> str:
    if not isinstance(value, dict):
        raise RuntimeError("expected a native form identity object")
    return form_id(value.get("formId"))


def normalize_url(value: str) -> str:
    parts = urlsplit(value.strip())
    if (parts.scheme != "http" or parts.hostname not in {"localhost", "127.0.0.1", "::1"}
            or parts.username is not None or parts.password is not None
            or parts.path not in {"", "/"} or parts.query or parts.fragment or parts.port is None):
        raise RuntimeError("DEVBENCH_URL must be a plain loopback http://host:port base URL")
    return urlunsplit((parts.scheme, parts.netloc, "", "", ""))


def identity(state: dict) -> tuple:
    if (state.get("plugin") != "devbench" or type(state.get("pid")) is not int or state["pid"] <= 0
            or type(state.get("port")) is not int or not 1 <= state["port"] <= 65535
            or not isinstance(state.get("exe"), str) or not state["exe"]
            or type(state.get("vr")) is not bool
            or not isinstance(state.get("version"), str) or not state["version"]):
        raise RuntimeError("inspect state lacks a verified DevBench instance identity")
    return tuple(state[key] for key in ("pid", "port", "exe", "vr", "version"))


def remaining(deadline: float) -> float:
    budget = deadline - time.monotonic()
    if budget <= 0:
        raise RuntimeError("recipe time budget exhausted; no further mutations will be dispatched")
    return budget


def tool(url: str, name: str, args: dict, deadline: float) -> dict:
    budget = remaining(deadline)
    # No redirects/retries, especially for mutations. These are socket limits,
    # not hard cancellation of a server task or arbitrary streaming response.
    response = requests.post(f"{url}/api/tool/{name}", json=args,
                             timeout=(min(1.0, budget / 2), min(10.0, budget / 2)),
                             allow_redirects=False)
    if response.status_code != 200:
        raise RuntimeError(f"{name} failed with HTTP {response.status_code}; no automatic retry")
    body = response.json()
    if not isinstance(body, dict) or "error" in body:
        raise RuntimeError(f"{name} returned a malformed or error receipt")
    return body


@dataclass(frozen=True)
class Instance:
    url: str
    binding: tuple


def select_instance(candidates: list[Instance], runtime: str | None) -> Instance:
    acceptable = [candidate for candidate in candidates
                  if runtime is None or candidate.binding[3] == (runtime == "vr")]
    if len(acceptable) != 1:
        shown = [(item.url, item.binding) for item in candidates]
        raise RuntimeError(f"expected one acceptable instance, found {len(acceptable)}: {shown}; "
                           "select DEVBENCH_URL or --port/--runtime explicitly")
    return acceptable[0]


def discover(runtime: str | None, port: int | None, deadline: float) -> Instance:
    explicit = os.environ.get("DEVBENCH_URL")
    urls = ([normalize_url(explicit)] if explicit else
            [f"http://127.0.0.1:{p}" for p in ([port] if port else range(8920, 8926))])
    candidates, unknown = [], []
    for url in urls:
        try:
            state = tool(url, "inspect", {"kind": "state"}, deadline)
            binding = identity(state)
            if binding[1] != urlsplit(url).port or (port is not None and binding[1] != port):
                raise RuntimeError("answering port differs from the selected URL/--port")
            candidates.append(Instance(url, binding))
        except requests.Timeout as error:
            if explicit or port is not None:
                raise
            unknown.append(f"{url}: {error}")
        except requests.ConnectionError:
            if explicit or port is not None:
                raise
        except (requests.RequestException, RuntimeError, ValueError) as error:
            if explicit or port is not None:
                raise
            unknown.append(f"{url}: {error}")
    if unknown:
        raise RuntimeError(f"discovery has unverified candidates: {unknown}; choose an explicit URL/port")
    return select_instance(candidates, runtime)


class BoundClient:
    def __init__(self, instance: Instance, deadline: float):
        self.instance, self.deadline = instance, deadline
        self.last_frame = -1
        self.mutation_issued = False

    def call(self, name: str, args: dict) -> dict:
        return tool(self.instance.url, name, args, self.deadline)

    def state(self) -> dict:
        state = self.call("inspect", {"kind": "state"})
        if identity(state) != self.instance.binding:
            raise RuntimeError("answering instance changed; stop before further mutation/capture")
        frame = state.get("frame")
        if type(frame) is not int or frame < self.last_frame:
            raise RuntimeError("game frame is missing or regressed; binding/readiness uncertain")
        self.last_frame = frame
        return state

    def papyrus(self, function: str, args: list, self_id: str = "0x14", *, mutate=False):
        if mutate:
            self.state()
            self.mutation_issued = True
        body = self.call("papyrus", {"action": "call", "script": "ObjectReference",
                                   "function": function, "self": {"form": self_id}, "args": args,
                                   "timeoutMs": max(1, min(3000, int(remaining(self.deadline) * 1000)))})
        if body.get("called") is not True or "returned" not in body:
            raise RuntimeError(f"{function} lacks a successful callback receipt")
        return body["returned"]


def wait_until(client: BoundClient, label: str, condition) -> None:
    try:
        while True:
            remaining(client.deadline)
            if condition():
                return
            time.sleep(min(0.1, remaining(client.deadline)))  # cadence, not readiness proof
    except (RuntimeError, requests.RequestException) as error:
        raise RuntimeError(f"could not verify {label}: {error}") from error


def reference(client: BoundClient, selector: dict) -> dict:
    refs = client.call("inspect", {"kind": "refs", **selector}).get("refs")
    if not isinstance(refs, list) or len(refs) != 1:
        raise RuntimeError("target must resolve unambiguously; use a specific FormID/EditorID")
    target = refs[0]
    if not isinstance(target, dict):
        raise RuntimeError("target receipt is not an object")
    form_id(target.get("formId"))
    vector(target.get("position"))
    return target


def standoff_distance(target: dict, override: float | None) -> float:
    bounds = target.get("bounds")
    distance = override if override is not None else (
        max(200.0, math.dist(vector(bounds.get("min")), vector(bounds.get("max"))) * 1.5)
        if isinstance(bounds, dict) else 350.0)
    if not finite_number(distance) or not 0 < distance <= 100000:
        raise RuntimeError("standoff distance must be finite and in (0, 100000] game units")
    return distance


def angle_error(actual: float, expected: float) -> float:
    return abs((actual - expected + 180.0) % 360.0 - 180.0)


def positive_float(value: str) -> float:
    parsed = float(value)
    if not finite_number(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return parsed


def finite_float(value: str) -> float:
    parsed = float(value)
    if not finite_number(parsed):
        raise argparse.ArgumentTypeError("must be finite")
    return parsed


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--model", help="unique model-path substring; ambiguous matches fail")
    target.add_argument("--formid", help="hex FormID")
    target.add_argument("--editorid", help="EditorID")
    parser.add_argument("--distance", type=positive_float, help="standoff game units, at most100000")
    parser.add_argument("--height-offset", type=finite_float, default=0.0,
                        help="player-foot offset from target origin, +/-100000; eye height comes from the camera")
    parser.add_argument("--runtime", choices=("vr", "se"), help="se selects non-VR SE/AE")
    parser.add_argument("--port", type=int)
    parser.add_argument("--provider", help="exact registered capture provider; required if more than one")
    parser.add_argument("--timeout", type=positive_float, default=90.0, help="total recipe budget, at most120s")
    parser.add_argument("--apply", action="store_true", help="permit mutation in your owned development session")
    args = parser.parse_args(argv)
    if (args.timeout > 120 or abs(args.height_offset) > 100000
            or (args.distance is not None and args.distance > 100000)):
        parser.error("timeout may not exceed120s; distance/absolute height offset may not exceed100000")
    if args.port is not None and not 1 <= args.port <= 65535:
        parser.error("port must be1..65535")
    if not (args.model or args.formid or args.editorid):
        parser.error("target selector may not be empty")
    if args.apply and args.port is None and not os.environ.get("DEVBENCH_URL"):
        parser.error("--apply requires an explicit --port or DEVBENCH_URL; discovery is inspection-only")
    return args


def run(args) -> None:
    deadline = time.monotonic() + args.timeout
    instance = discover(args.runtime, args.port, deadline)
    print("selected:", json.dumps(dict(zip(("pid", "port", "exe", "vr", "version"), instance.binding))))
    client = BoundClient(instance, deadline)
    try:
        if client.state().get("playerLoaded") is not True:
            raise RuntimeError("no loaded player; prepare the session through its owner/controller first")
        target = reference(client, {"model": args.model, "limit": 2} if args.model else
                           {"formId": args.formid or args.editorid})
        target_id = form_id(target["formId"])
        if target_id == "0x00000014":
            raise RuntimeError("the player cannot be its own framing target")
        position = vector(target["position"])
        distance = standoff_distance(target, args.distance)
        expected_position = vector([position[0], position[1] - distance, position[2] + args.height_offset])
        print("plan:", json.dumps({"target": target_id, "model": target.get("model"), "standoff": expected_position}))
        if not args.apply:
            print("inspection only; --apply is required for movement/camera/capture")
            return
        providers = client.call("capture", {"kind": "providers"}).get("providers")
        if not isinstance(providers, list) or not all(isinstance(item, str) and item for item in providers):
            raise RuntimeError("capture provider inventory is malformed")
        provider = args.provider or (providers[0] if len(providers) == 1 else None)
        if provider is None or provider not in providers:
            raise RuntimeError("a conclusive capture needs an explicit or sole registered provider; "
                               "native fallback is always inconclusive, so stop before movement")
        cell_id = form_record(target.get("cell"))
        world = client.papyrus("GetWorldSpace", [], target_id)
        context_key, context_id = ("cell", cell_id) if world is None else ("worldspace", form_record(world))
        camera = client.call("camera", {"action": "get"})
        if camera.get("freeCam") is not False or camera.get("orbit") is True:
            raise RuntimeError("free camera/orbit active or unavailable; do not take over another camera owner")
        client.papyrus("MoveTo", [{"form": target_id}, 0.0, -distance, args.height_offset, False], mutate=True)

        def pose_ready():
            if client.state().get("playerLoaded") is not True:
                return False
            scene = client.call("inspect", {"kind": "scene"})
            if scene.get("playerLoaded") is not True:
                return False
            context = scene.get(context_key)
            if not isinstance(context, dict) or form_record(context) != context_id:
                return False
            if math.dist(vector(scene.get("position")), expected_position) > 2.0:
                return False
            current = reference(client, {"formId": target_id})
            if (form_record(current.get("cell")) != cell_id
                    or math.dist(vector(current["position"]), position) > 2.0):
                raise RuntimeError("target moved or changed cell; stop, do not issue a corrective move")
            return client.papyrus("Is3DLoaded", [], target_id) is True

        wait_until(client, "loaded target/context and standoff position", pose_ready)
        yaw = client.papyrus("GetAngleZ", [])
        heading = client.papyrus("GetHeadingAngle", [{"form": target_id}])
        if not finite_number(yaw) or not finite_number(heading):
            raise RuntimeError("heading/yaw receipt is not a finite number")
        # GetHeadingAngle is relative to current yaw (vanilla ObjectReference.psc).
        wanted_yaw = (yaw + heading) % 360.0
        client.papyrus("SetAngle", [0.0, 0.0, wanted_yaw], mutate=True)

        def facing_ready():
            if not pose_ready():
                return False
            current_yaw = client.papyrus("GetAngleZ", [])
            residual = client.papyrus("GetHeadingAngle", [{"form": target_id}])
            return (finite_number(current_yaw) and finite_number(residual)
                    and angle_error(current_yaw, wanted_yaw) <= 1.0 and abs(residual) <= 1.0)

        wait_until(client, "applied player facing", facing_ready)
        before_pov = client.state()["frame"]
        client.mutation_issued = True
        applied = client.call("camera", {"action": "setPov", "pov": "third"})
        if applied.get("requestedPov") != "third" or applied.get("pov") != "third":
            raise RuntimeError("camera receipt does not prove applied third-person POV")
        wait_until(client, "later game frame and third-person camera readback",
                   lambda: client.state()["frame"] > before_pov
                   and client.call("camera", {"action": "get"}).get("pov") == "third")
        if not facing_ready():
            raise RuntimeError("pose/facing changed before capture")
        camera = client.call("camera", {"action": "get"})
        if camera.get("pov") != "third" or camera.get("freeCam") is not False or camera.get("orbit") is True:
            raise RuntimeError("camera changed before capture")
        if provider not in client.call("capture", {"kind": "providers"}).get("providers", []):
            raise RuntimeError("selected capture provider disappeared; no fallback")
        capture_state = client.state()  # identity rechecked immediately before capture
        checkpoint = f"goto_mesh_{target_id[2:]}_{time.time_ns()}"
        shot = client.call("capture", {"kind": provider, "checkpointId": checkpoint,
                                       "timeoutMs": max(1, min(60000, int(remaining(deadline) * 1000)))})
        if (shot.get("ok") is not True or shot.get("provider") != provider
                or shot.get("checkpointId") != checkpoint or shot.get("inconclusive") is not False
                or type(shot.get("bytes")) is not int or shot["bytes"] <= 0
                or not isinstance(shot.get("path"), str) or not shot["path"]
                or type(shot.get("frame")) is not int or shot["frame"] < capture_state["frame"]):
            raise RuntimeError(f"capture missing conclusive artifact/frame evidence: {shot}")
        client.state()  # detect replacement during capture; never relabel it a success
        print("capture artifact (visual qualification still required):", json.dumps(shot))
    except (RuntimeError, ValueError, requests.RequestException):
        if client.mutation_issued:
            print("mutation was dispatched and may have completed; inspect partial state before any retry; "
                  "no automatic rollback/replay", file=sys.stderr)
        raise


def main() -> None:
    args = parse_args()
    try:
        run(args)
    except (RuntimeError, ValueError, requests.RequestException) as error:
        sys.exit(f"stopped: {error}")


if __name__ == "__main__":
    main()
