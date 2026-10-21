# Friend or Foe native co-op research

Original-code research checkpoint, separate from the coop-anything documentation hub.
Start with [README.md](README.md) and [status and evidence](docs/STATUS.md).
This is not a playable release.

The exact Windows executable and full English/Russian content packs are required
for the isolated game harness. They are excluded from this source repository.
All game assets, captures, saves, account identifiers, runtime logs and binaries
are denied by the root allowlist. Never force-add these files.

Earlier scenario5 evidence: a7,200-frame English/Russian native keyboard/controller
and three-area-swap gate passed with selected progress checked. Both games,
launchers and mock relay helpers exited0, with zero discarded stream bytes.
The quiet unlimited-frame real-input path reached361 ticks (four120-tick
observations), then closing only the owned host made the peer exit normally.
No gameplay input fixture or diagnostic image capture was enabled in that check.

The private GUI and optional3,600-frame automated connection-test path passed
own-code fixtures and helper flows. dist/fof-research-20261021.zip contains nine
explicitly allowlisted own-runtime/manifest/instruction files, with no game,
Steam library, save, log or account data. It is a Tokyo research package.
Actual two-account delivery, full campaign state, physical input usability,
persistent save/resume and reconnect remain unfinished or unverified.

Own-code fixtures: compatibility/test_preflight.py, qa/run_pair.py --selftest,
analysis/native/score_pair.py --selftest, steam/test_relay.py and
steam/test_api_abi.py. Build the own Steam ABI mock before the last fixture;
it never loads Steam. Game runs require new isolated test sandboxes and full
content preflight. Test timeout or forced job termination is never a QA pass.

The current source adds selected native mode state, full canonical area paths,
campaign flags and a strict pause/resume fixture. Game validation belongs to the
exact source/scenario version that ran; the status page records scope and failures.
The profile-store foundation has synthetic tests but no campaign runtime integration.
The nine-file private package predates these changes and is not offered as a release.

The source allowlist and `qa/prepare_publication.py` independently check exports.
Historical private Git commits are excluded from the public source snapshot.
See [the roadmap](docs/ROADMAP.md) for remaining implementation and release gates.
