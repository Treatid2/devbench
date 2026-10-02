# devbench HTTP integration tests

A starter pytest suite that drives the devbench REST API against a **running**
Skyrim with the plugin loaded. It is **CI-safe**: when no game/server is
reachable, the whole suite is _skipped_ (never failed), and individual tests are
skipped when the capability they exercise isn't present in the live build.

## Requirements

```sh
pip install -r requirements.txt
```

(`pytest`, `requests` — Python 3.9+.)

## Running

```sh
pytest tests/http -v
```

If you have multiple game instances or a non-default port, point the suite at
the right server explicitly:

```sh
# PowerShell
$env:DEVBENCH_URL = "http://127.0.0.1:8921"; pytest tests/http -v

# bash
DEVBENCH_URL=http://127.0.0.1:8921 pytest tests/http -v
```

## Mesh-reference capability qualification and framing example

The two mesh capability tests are opt-in, read-only qualifications. They no
longer discover a convenient nearby mesh or pass on an empty optional-field
loop. Supply a known, loaded positive reference with a nonempty model path,
parent cell and nondegenerate finite ordered bounds. The fixture also binds the
current answering PID and VR flag. Replace the illustrative values below with
your session's observed identities; these are not a provided game fixture.

```powershell
$env:DEVBENCH_URL = "http://127.0.0.1:8921"
$env:DEVBENCH_BOOTSTRAP = "off"
$env:DEVBENCH_MESH_FIXTURE = '{"pid":12345,"vr":true,"formId":"0x00000001","model":"ActualLoadedMesh.nif","cellFormId":"0x00000002"}'
pytest tests/http/test_inspect.py -v -k "reports_cell_and_model or model_filter"
```

Without `DEVBENCH_MESH_FIXTURE`, this entire capability is explicitly skipped as
**NOT QUALIFIED**. A green suite with that skip is not mesh qualification. Once
configured, absent model/cell/bounds, filter failure or instance mismatch fails
instead of skipping. These tests use the explicit URL and their own read-only
client, not the shared first-hit discovery or player bootstrap. Other tests in
the suite retain their existing bootstrap policy. Choose a fixture whose full
model path has fewer than 1000 loaded matches so positive membership can be
proved without truncation; test evidence should retain fixture and producer
identities, pass/failure/skip counts and source/artifact boundaries.

`examples/goto_mesh.py` is a separate **manual mutating example**, not this
qualification and not part of CI. It defaults to inspection only. `--apply`
requires an explicit `DEVBENCH_URL` or `--port` and ownership of the loaded
development session; it does not acquire controller/environment leases. Runtime
selectors narrow inspection discovery, which refuses ambiguous or unverified
answers. The selected pid/port/exe/vr/version is printed and rechecked around
movement and capture. The example issues one typed `MoveTo`, verifies loaded
context and position, applies/readbacks facing and third-person POV, and checks
a later game frame and capture result. No fixed sleep is completion evidence.
A sole or explicitly selected registered capture provider must be available
before movement; `--provider` selects among multiple providers. Native fallback
is always marked inconclusive by the host, so this qualification-oriented
example stops before mutation when no conclusive provider is available. Provider
inventory is not proof of runtime health; the final correlated artifact receipt
and subsequent visual inspection remain necessary.
The total recipe budget is capped at 120 seconds; request timeouts are bounded
socket limits, not proof that a timed-out engine task was cancelled. Uncertain
mutation stops without replay or rollback. Inspect the partial state before any
retry. Bounds-based distance is a rough heuristic, not collision/occlusion or
projected fitting; the observed game frame is not GPU presentation or visual
acceptance. This tool does not install, launch, save or reload a game.
The default height offset is zero for player feet; the camera already supplies
eye height. `--height-offset` makes a fixture-specific adjustment explicit.
If collision/terrain/physics prevent the requested position being stable within
two game units, verification fails rather than silently capturing another pose.

Host-independent fake-HTTP/clock recipe checks live separately under
`tests/unit/test_goto_mesh.py` and can be run by a receiving task without a game:

```sh
pytest tests/unit/test_goto_mesh.py -q
```

Source inspection or a compiled native DLL does not claim these assertions or
the HTTP/example qualification passed. No native rebuild is required merely to
change this Python recipe/test logic; retain the actual unchanged native
producer commit/receipt instead of labelling it a build of the Python head.

## Held-key movement checks

`test_input_holds.py` is opt-in. Set `DEVBENCH_TEST_INPUT=1` and
`DEVBENCH_URL`, then run `pytest tests/http/test_input_holds.py -v`. Load an
unpaused development save with the player on foot and clear ground ahead.
Avoid other input during the test. It moves the player using the configured
Forward key, checks continued movement, and verifies that release and lease
expiry stop it. It releases its own keys without saving or reloading.

### Shutdown

Run the Windows shutdown test separately. It exits Skyrim without saving and
checks that the process terminates successfully within 30 seconds:

```powershell
$env:DEVBENCH_URL = "http://127.0.0.1:8920"
$env:DEVBENCH_TEST_SHUTDOWN = "1"
pytest tests/http/test_shutdown.py -v
Remove-Item Env:DEVBENCH_TEST_SHUTDOWN
```

## How discovery works

The base URL is resolved in this order (first hit wins):

1. **`DEVBENCH_URL`** environment variable (e.g. `http://127.0.0.1:8921`).
2. **`runtime.json`** published by a running instance at
   `<GameData>/SKSE/Plugins/devbench/runtime.json` (`{"port": <int>}`) — known
   Steam SE/VR `Data` paths are probed. The plugin writes this so fixed-URL
   clients can find an auto-incremented port.
3. **Port probe** of `127.0.0.1:8920..8925` (the default 8920 auto-increments
   when busy, e.g. multiple game instances).

A candidate is only accepted once `POST /api/tool/inspect {"kind":"state"}`
returns JSON carrying a `plugin` field (liveness check).

## Skip rules (why the suite stays green)

- **No server reachable** → entire session skipped.
- **No in-world save loaded** → the suite **bootstraps one itself** (see below),
  so `@pytest.mark.requires_player` tests run; they skip only if the bootstrap is
  disabled (`DEVBENCH_BOOTSTRAP=off`) or fails.
- **Capability absent on this branch** → a test skips when its tool is missing
  from `GET /api/tools`, or when a needed action/kind is absent from the live
  `inputSchema` enum (e.g. `menu` `open`).

## Self-contained bootstrap

If a server is reachable but at the **main menu**, the suite drives the game into
a playable state itself (an already-loaded game is used as-is and never
disturbed). Controlled by `DEVBENCH_BOOTSTRAP`:

| Value           | Behavior                                                                               |
| --------------- | -------------------------------------------------------------------------------------- |
| `coc` (default) | Run the `recipes/bootstrap.json` recipe (`coc` into a cell + wait) — save-independent. |
| `save`          | Load `DEVBENCH_SAVE`, or the most recent save if unset.                                |
| `off`           | Don't drive the game; `requires_player` tests skip (the old "human controls state").   |

`DEVBENCH_COC_CELL` overrides the `coc` target (default `WhiterunDragonsreach`).
The `coc` path reuses the same recipe loader + `scenario` runner the recipe tests
use (`load_recipe`/`run_recipe` in `conftest.py`) — there's one place that knows
how to play a recipe.

## Layout

| File                        | Covers                                                                                                                                    |
| --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `conftest.py`               | discovery, `base_url`/`client`/`tool_schema` fixtures, `requires_player`, skip helpers                                                    |
| `test_smoke.py`             | discovery (`/api/tools`, `inspect` present) + console/camera/game smoke                                                                   |
| `test_inspect.py`           | `inspect` state / vm / scene / refs                                                                                                       |
| `test_papyrus.py`           | `papyrus` list / describe / call (globals + members) + error                                                                              |
| `test_menu.py`              | `menu` list, and the open→list→close round-trip (guarded)                                                                                 |
| `visual.py`                 | BATCH/corpus SSIM/threshold/ROI scoring across many recordings — pure, no server needed. NOT the primary mod-author interface — see below |
| `test_visual.py`            | unit tests for `visual.py` against synthetic PNGs — no server, no game                                                                    |
| `test_visual_regression.py` | live: replay a recording's `meta.checkpoints`, batch-score captures against `goldens/`                                                    |
| `goldens/`                  | reference images for `test_visual_regression.py` — see `goldens/README.md`                                                                |
| `examples/`                 | hand-run example recipes (not collected by pytest) — see below                                                                            |

**Native single-checkpoint verdict (the primary interface):** the `capture` tool's `golden`/
`threshold`/`regions` args — normally supplied via `record{action:"replay", goldens:{"<id>":
{golden,threshold?,regions?}}}` — return `{ssim, threshold, passed}` inline in the same HTTP/MCP
call that ran the replay. No Python, no second process. `visual.py`/`test_visual_regression.py`
above are for batch/corpus work (many recordings, report generation, `--visual-update` across a
whole tree) — reach for them only when that's actually the job.

## Examples

`examples/combat_arena.py` is a hand-run recipe (not a CI test — it mutates game
state heavily and runs ~1 min) that drives, forces, and **resolves** a 3-way
combat scene through the REST API. It `tgm`s the player, spawns three factions on
the player, then each tick gathers the survivors onto the player
(`ObjectReference.MoveTo`) and forces the 3-way (`Actor.StartCombat`), reporting
live per-team counts until one team is left standing.

```sh
# with an in-world save already loaded:
python tests/http/examples/combat_arena.py
# -> ... WINNER: Stormcloaks (3/5 survived)
```

Its header documents the Skyrim-engine constraints it works around (high-process
zone, scatter/de-aggro, unreliable faction hostility, `player.`-only console
commands). It relies on devbench padding omitted optional args — `MoveTo` and
`StartCombat` both have optionals and otherwise dispatch-but-no-op. Use it as a
template for scripting your own combat/AI scenarios.

## Environment variables

| Variable       | Effect                                                               |
| -------------- | -------------------------------------------------------------------- |
| `DEVBENCH_URL` | Skip discovery and use this base URL (e.g. `http://127.0.0.1:8921`). |
