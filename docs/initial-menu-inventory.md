# Initial menu inventory

The exact8b startup qualified in Mapping's Skyrim31444 on19October2026, but
`menu/list` returned empty while a direct MainMenu read reported open. Deferred
startup installs the event sink after menus may already exist. Past open events
are not replayed by the engine.

InstallGameEvents, called by the deferred main-thread Publish phase, now subscribes
and seeds the same production inventory from UI::menuMap entries with a non-null
menu whose OnStack predicate is true. This matches pinned CommonLib
abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7 UI::IsMenuOpen (not factory registration or
mere instance existence). No main-thread query runs on listener/watchdog threads.
Seeding emits no synthetic menu/recording events or NewGame/Calendar callbacks.
Normal open/close callbacks continue updating the inventory.

A generation check rejects a snapshot if an event arrived during collection,
preserving that event instead of replacing it with stale data. If UI is unavailable
or the seed races, inventoryInitialized=false explicitly means incomplete;
openMenus contains only observed names, messageBoxOpen is null if not positively
observed. After a successful seed, existing boolean absence semantics are retained.
Negative noMenu/noModal/noBlockingMenu guards fail closed on incomplete inventory.
No UI operation, menu opening, game restart or fake event is used to initialize it.

The focused `Menu inventory:` offline cases exercise the actual production-used
container/serialization/negative guards: already-open MainMenu, unavailable seed,
subsequent events, deterministic intervening event, known-empty UI and snapshot
ownership/deduplication. They do not execute actual RE::UI traversal or qualify a
new installed runtime. Broker compiles the actual engine adapter; future requester
acceptance at a genuinely needed adoption boundary checks initial menu presence.
Old8b startup and7/5passes remain exact historical evidence, not a verdict on this
new source. No cap/session/loader scheduling/CSX/bridge/recipe/pin changes.
