"""Host-independent recipe checks: fake HTTP/clock only, no live game fixtures."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest


_path = Path(__file__).parents[1] / "http" / "examples" / "goto_mesh.py"
_spec = importlib.util.spec_from_file_location("devbench_goto_mesh_example", _path)
mesh = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = mesh
_spec.loader.exec_module(mesh)


def state(frame=1):
    return {"plugin": "devbench", "pid": 123, "port": 8921, "exe": "SkyrimVR.exe",
            "vr": True, "version": "fixture", "playerLoaded": True, "frame": frame}


@pytest.mark.parametrize("arguments", [[], ["--model", "a", "--formid", "0x1"],
                                      ["--formid", "0x1", "--distance", "nan"],
                                      ["--formid", "0x1", "--distance", "inf"],
                                      ["--formid", "0x1", "--distance", "0"],
                                      ["--formid", "0x1", "--timeout", "121"],
                                      ["--formid", "0x1", "--port", "65536"]])
def test_invalid_arguments_fail_before_network(arguments):
    with pytest.raises(SystemExit):
        mesh.parse_args(arguments)


def test_apply_requires_explicit_instance(monkeypatch):
    monkeypatch.delenv("DEVBENCH_URL", raising=False)
    with pytest.raises(SystemExit):
        mesh.parse_args(["--formid", "0x1", "--runtime", "vr", "--apply"])


def test_discovery_refuses_ambiguity_and_runtime_mismatch():
    vr = mesh.Instance("http://127.0.0.1:8921", mesh.identity(state()))
    flat = mesh.Instance("http://127.0.0.1:8920", (124, 8920, "SkyrimSE.exe", False, "fixture"))
    with pytest.raises(RuntimeError):
        mesh.select_instance([vr, flat], None)
    assert mesh.select_instance([vr, flat], "vr") == vr
    with pytest.raises(RuntimeError):
        mesh.select_instance([flat], "vr")
    with pytest.raises(RuntimeError):
        mesh.identity({**state(), "plugin": "not-devbench"})


@pytest.mark.parametrize("url", ["https://127.0.0.1:8921", "http://example.com:8921",
                                 "http://user:secret@localhost:8921", "http://localhost:8921/api",
                                 "http://localhost:8921/?x=1"])
def test_url_rejects_remote_credentials_and_nonbase_routes(url):
    with pytest.raises(RuntimeError):
        mesh.normalize_url(url)


def test_instance_change_and_frame_regression_stop(monkeypatch):
    client = mesh.BoundClient(mesh.Instance("http://127.0.0.1:8921", mesh.identity(state())), float("inf"))
    monkeypatch.setattr(client, "call", lambda *args: state(5))
    client.state()
    monkeypatch.setattr(client, "call", lambda *args: state(4))
    with pytest.raises(RuntimeError, match="regressed"):
        client.state()
    monkeypatch.setattr(client, "call", lambda *args: {**state(6), "pid": 999})
    with pytest.raises(RuntimeError, match="instance changed"):
        client.state()


class FakeGame:
    def __init__(self, *, never_ready=False, bad_move=False, inconclusive=False):
        self.events = []
        self.position = [0.0, 0.0, 0.0]
        self.yaw = 10.0
        self.pov = "first"
        self.frame = 1
        self.pending_pose_reads = 2
        self.never_ready, self.bad_move, self.inconclusive = never_ready, bad_move, inconclusive

    def tool(self, url, name, args, deadline):
        mesh.remaining(deadline)
        self.events.append((name, args))
        self.frame += 1
        if name == "inspect":
            if args["kind"] == "state":
                return state(self.frame)
            if args["kind"] == "refs":
                return {"refs": [{"formId": "0x00000001", "position": [100.0, 400.0, 0.0],
                                  "cell": {"formId": "0x00000002"}}]}
            if args["kind"] == "scene":
                self.pending_pose_reads -= 1
                if self.pending_pose_reads <= 0 and not self.never_ready:
                    self.position = [100.0, 50.0, 64.0]
                return {"playerLoaded": True, "cell": {"formId": "0x00000002"}, "position": self.position}
        if name == "papyrus":
            function = args["function"]
            returned = None
            if function == "MoveTo" and self.bad_move:
                return {"called": False, "returned": None}
            if function == "Is3DLoaded":
                returned = True
            if function == "GetAngleZ":
                returned = self.yaw
            if function == "GetHeadingAngle":
                returned = 50.0 - self.yaw
            if function == "SetAngle":
                self.yaw = args["args"][2]
            return {"called": True, "returned": returned}
        if name == "camera":
            if args["action"] == "setPov":
                self.pov = args["pov"]
                return {"requestedPov": self.pov, "pov": self.pov}
            return {"pov": self.pov, "freeCam": False, "orbit": False}
        if name == "capture":
            if args["kind"] == "providers":
                return {"providers": ["fixture"]}
            return {"ok": True, "provider": "fixture", "checkpointId": args["checkpointId"],
                    "bytes": 100, "path": "fixture.png", "frame": self.frame, "inconclusive": self.inconclusive}
        raise AssertionError((name, args))


def prepare(monkeypatch, game):
    ticks = iter(range(10000))
    monkeypatch.setattr(mesh.time, "monotonic", lambda: next(ticks) * 0.1)
    monkeypatch.setattr(mesh.time, "sleep", lambda _: None)
    monkeypatch.setattr(mesh, "tool", game.tool)
    monkeypatch.setattr(mesh, "discover", lambda *args: mesh.Instance("http://127.0.0.1:8921", mesh.identity(state())))
    monkeypatch.delenv("DEVBENCH_URL", raising=False)
    return mesh.parse_args(["--formid", "0x1", "--port", "8921", "--apply"])


def mutations(game, function):
    return [args for name, args in game.events if name == "papyrus" and args["function"] == function]


def test_success_uses_one_move_readbacks_and_relative_heading(monkeypatch):
    game = FakeGame()
    mesh.run(prepare(monkeypatch, game))
    assert len(mutations(game, "MoveTo")) == 1
    assert mutations(game, "MoveTo")[0]["args"] == [{"form": "0x00000001"}, 0.0, -350.0, 64.0, False]
    assert mutations(game, "SetAngle")[0]["args"] == [0.0, 0.0, 50.0]
    assert not any(name == "console" for name, _ in game.events)
    assert sum(name == "capture" and args["kind"] != "providers" for name, args in game.events) == 1


@pytest.mark.parametrize("options", [{"never_ready": True}, {"bad_move": True}, {"inconclusive": True}])
def test_uncertain_or_inconclusive_result_stops_without_mutation_replay(monkeypatch, options):
    game = FakeGame(**options)
    with pytest.raises(RuntimeError):
        mesh.run(prepare(monkeypatch, game))
    assert len(mutations(game, "MoveTo")) == 1
    if not options.get("inconclusive"):
        assert not any(name == "capture" and args["kind"] != "providers" for name, args in game.events)


def test_inspection_default_never_mutates(monkeypatch):
    game = FakeGame()
    args = prepare(monkeypatch, game)
    args.apply = False
    mesh.run(args)
    assert not any(name in {"papyrus", "camera", "capture", "console"} for name, _ in game.events)
