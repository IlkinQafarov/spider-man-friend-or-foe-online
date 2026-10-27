# Roadmap

The project has a published **v1.1.0-beta**. Historical evidence includes real players completing Tokyo 1 together over Steam and a shortened co-op run through all 20 mission endings. See [Status](STATUS.md#progress-estimate) for project progress notes and [Testing](TESTING.md) for QA procedures.

## Delivered: mid-mission joining

v1.1.0-beta enables joining the host's current mission without restarting its area. Automated English/Russian checks passed cancellation, exhausted-history refusal, first arrival under simulated bad internet, swapped-host reconnect, and admission into Doc Ock's arena after its native area exit and intro. The player build passed startup/normal-close and bot-refusal checks. See [Status](STATUS.md) for exact evidence and failed attempts.

These checks are on one PC. The next priorities are broader real-world validation and persistence, rather than additional optional features.

## Remaining work

1. Verify shared saves, Continue on both sides, and recovery after a failure or restart.
2. Test a new mid-mission join with two real PCs over Steam, including physical keyboards and controllers. The earlier real-player Steam test covered Tokyo 1, not mid-mission admission.
3. Check ordinary human play through later missions and complete end-to-end sessions on clean PCs, including normal exits and no forced cleanup.
4. Continue watching the two rare desync reports: the Rhino intro under lag and the start of Transylvania 1. They were each observed once and have not been reproduced.
5. Joining through the Steam friends list instead of a lobby code remains optional.

The all-mission bot run and focused joining checks do not establish later-mission human play, arbitrary network conditions or all long-history reconstructions.
