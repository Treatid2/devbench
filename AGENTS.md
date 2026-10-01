# Local fork ownership

This checkout is the registered source repository for the local DevBench fork.
Read [docs/local-management.md](docs/local-management.md) before source,
integration, PR or delivery work. The DevBench manager owns those changes;
requesting tasks supply requirements and own runtime acceptance.

Do not replace the experimental integration with a feature branch based on
upstream alone. Do not build, deploy or launch a modlist to duplicate requester
testing. Compile exact committed candidates through Build Broker; it owns
managed build scratch, retained dependencies and artifact receipts.

Preserve unrelated edits and original handoff commits. Changes to the reviewed
fork head go through PR and automatic review. Experimental integration is a
separate, explicitly identified delivery lane, not a review verdict.
