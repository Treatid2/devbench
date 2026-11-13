# Local DevBench management

This is the local fork's operating agreement, not upstream project policy.

## Owners and routing

| Responsibility | Accountable chat |
| --- | --- |
| DevBench design, source custody, PRs, review corrections, integration and upstream submission | [DevBench owner](codex://threads/01a0f1f3-3c7a-7b43-8325-943472dc4778) |
| Feature intake and placement across Auto-Tools, DevBench and CSX | [Auto Main](codex://threads/01a08408-070f-7613-8db6-7166b281a685) |
| Deterministic compile, dependency retention, source admission and artifact custody | [Build Broker manager](codex://threads/01a0ccd6-76b6-75f2-bb91-454094d6812f) |
| Runtime environment, deployment coordination, purpose-specific tests and acceptance | The requesting/receiving task |

CSX source-owner routing is not yet established here. Ask Auto Main to resolve
cross-layer placement with the relevant CSX owner; do not invent one.
Auto Main manages feature intake for now, but is not a mandatory proxy for
ordinary calls to an already available DevBench runtime API.

Prefer an existing documented, discovered and callable API when it represents
the operation. Missing a convenience wrapper or dedicated menu tool is not a
blanket blocker. Report exact typed-schema or argument-shape mismatches and
unsafe uncertainty narrowly; never guess encodings or infer a bypass. Runtime
transport selection remains governed by the requesting task's protocol.

Requesters should send the desired behavior, use case, current failure/evidence,
acceptance criteria, urgency and requester identity. They need not build a PR.
An existing source candidate is optional: provide its immutable commit, base,
worktree, changed paths, evidence and known validation limits. The DevBench owner
must explicitly accept custody. A sent message alone is not active ownership.
After acceptance, avoid competing source/PR/integration delivery; the requester
continues to own requirements and runtime feedback.

## Two delivery lanes

Feature branches stay focused and retain original handoff identities. Our own
changes to reviewed fork `main` use local PRs against the fork head, automatic
review and correction until pass. Experimental integration supports requester
use and live testing before that pass; it is not reviewed-head promotion.
Evaluate findings, bind the verdict to the exact reviewed commit, and complete
the review transaction before promotion. Submit reviewed, tested changes
upstream after rebasing onto the current upstream head, with accurate compile
and requester-test evidence. A substantive rebase/conflict-resolution change
requires proportionate checking and a fresh focused review of the changed
source; an old pass does not cover different code. No pass, successful
compile or upstream submission implies runtime acceptance or upstream merge.

Upstream `main` is a trusted baseline. Synchronize its accepted history without
another code-review cycle or redundant compilation/runtime testing. Before
updating refs, verify current tips and ancestry and retain the previous head.
If our main has unique changes, compare only those changes with the corresponding
upstream implementation, retain the better-supported behavior and preserve
review provenance. Do not reset or replace the experimental integration.

Do not import an unmerged upstream PR unless the human explicitly directs it.
The six already-downloaded PRs (105, 106, 107, 109, 110 and 111) are a finite
grandfathered inventory: continue their local correction/review cycles as our
work. This exception grants no authority to fetch and integrate other PRs.
Post concise source-supported suggestions from those reviews on the relevant
upstream PR, distinguishing proposed/implemented/passed changes and avoiding
duplicate findings. Suggestions need not await a correction pass; publication
of our correction code still does. Product changes promoted to reviewed fork main must
also be represented in upstream main or a PR against it. Reconcile existing
upstream PR coverage before creating duplicates. Local-only governance docs
remain on their management branch until their upstream-coverage disposition
is explicit; do not silently exempt them or publish local chat/path guidance
upstream. PASS_WITH_FINDINGS is acceptable only after evaluation establishes
that no product correction is required. Otherwise preserve the verdict and
perform the authorised correction work with a fresh exact-source review.

The DevBench owner is responsible for existing as well as new DevBench reviews.
Transfer Auto Main's exact review inventory, pending work and human guidance
with provenance; use supported per-review delegation where necessary, never
another chat's credentials. Explicitly acknowledge custody so competing review
submissions stop without losing existing reports or correction work.

Submit one focused PR or bite-size local equivalent per review; do not bundle
independent reviews. A transport ZIP containing one scope and its necessary
context/evidence is not an aggregate review. Trusted upstream baseline adoption
does not enter this review queue. Evaluate findings as guidance to
improve maintainability, clarity, design, robustness and correctness, not merely
as bugs to fix or a checklist to silence. Record accepted improvements and
reasoned dispositions, verify proportionately, and continue authorised cycles
until PASS or evaluated PASS_WITH_FINDINGS, bound to the exact reviewed source.

Respect the review service's 30-minute creation gate. Maintain a finite queue
and give initial pending submissions priority so correction cycles cannot
starve untouched work. Use a temporary thread heartbeat only until all items
in the first tranche have initial review IDs or verified exact focused passes;
then pause it. Review returns wake the owner thereafter: evaluate and update
the returned item, and submit another eligible pending scope during that wake.
Never poll an active review or create duplicates. Retain already-active broad
reviews as historical evidence; they do not replace focused reviews.

`codex/devbench-all-integrated` is the experimental composition. Include
independent requested features there for early use without claiming that they
have passed review. Merge onto its actual owned head; never substitute an
upstream-only feature branch and lose previously integrated APIs. Retain the
previous composition and artifacts for rollback. Record component commits,
merge/conflict resolutions and any exclusions for each new composition.

At custody transfer on 2026-09-30 the integration was
`a1c00560e6bd922470f5e43c3946a2edf6e274f7`, containing upstream PRs
105, 106, 107, 109, 110 and 111. Its historical composition receipt is
`L:\Codex\artifacts\devbench\integration\compositions\a1c00560e6bd922470f5e43c3946a2edf6e274f7.json`.
This is provenance, not a permanently current head or reviewed status.

Keep three inventory classes separate: trusted upstream baseline, explicitly
imported unmerged upstream PRs, and our own changes. Historical inventory labels
and review verdicts remain immutable evidence, not permission to import more
upstream work or imply that imported PRs were our passed outbound submissions.
The human approved this workflow correction on 2026-10-03. Local and remote
fork main were fast-forwarded from f3e6a1a4958d733b11d7da17e41cc1dc7aa097d8
to upstream 32823e465c18a676afc9048ac948aa22a616abd0 (39 accepted commits,
zero fork-only main commits). GitHub marked baseline PR1 MERGED at that exact
head; no baseline review was required. Experimental integration remained
9e8ee3d04f2acca29c3378f9e03c138e15e8babe. These are dated receipts, not
permanently current tips. Governance documentation remains on local management
branches, outside product main and upstream publication.

## Compile without redundant testing

Use the installed Build Broker skill and current `get_build_capabilities`.
Invoke `compile_devbench_release` with the full immutable candidate commit,
including for PR corrections. The registered repository is
`L:\Codex\projects\code\DevBench`; its worktrees share the Git object store.
Dirty checkout changes are not build inputs. Ordinary native/header/docs and
non-executed test-source descendants are supported under qualified controls.
Changed xmake recipes, locks or submodule pins require broker-manager
assessment/capture. A rejected candidate is not permission for a local build.

The broker owns completion, managed build allocations, dependency reuse and
verified DLL/PDB publication. Do not wrap it in another polling supervisor,
rebuild retained dependencies for each review, or compile the baseline and
claim it proves a feature. Preserve the returned exact commit/tree, BuildKey,
artifact hashes/paths and terminal receipt. Cache hits prove issued artifact
identity, not byte-level reproducibility.

The DevBench owner performs source checks and review; it does not install into
a modlist, launch the game or repeat the receiving task's runtime assay.
Requester tests may run on the experimental composition before review, with
feedback supplied to the owner. Delivery of artifacts does not authorize any
other task to alter a shared runtime. Coordinate environment ownership before
deployment. Only ask for separate owner-run tests when they cover a concrete
gap the requesting task is not already covering.

## Delivery receipt and state

Retain a bounded request/delivery record on `L:\Codex\artifacts\devbench`:
requester and explicit custody acceptance; original and owner candidate IDs;
integration base/head/tree and component list; PR URL/base/head; compile
BuildKey/receipt and artifact hashes; review ID/package digest/verdict/head;
requester acceptance evidence; deployment identity if supplied by its owner;
upstream PR URL and merge status. Record exclusions and rollback identity.

Report these states independently: source accepted, implemented, compiled,
experimental integration available, review submitted/returned/passed,
requester runtime accepted, fork merged, upstream submitted/merged. Mark each
pending, failed, superseded or complete as applicable. Never describe a queued
handoff, successful compile or installed-file hash match as successful testing.
Review and compile service receipts remain authoritative for their own stages;
this receipt is a correlated index, not a replacement for them.

The owner supplies the completed artifact/reference and known limits to the
requester, receives purpose-specific feedback, and owns resulting source fixes.
Waiting review delivery uses the review service's return mechanism; do not
create duplicate review requests or polling chats while one is pending.
