<!--
Thanks for the PR! A few things that make review much faster:

- One logical change per PR, small diffs.
- Anything touching the chain or shaders MUST include the validation evidence
  (selftest vs goldens / PASS run) — see CONTRIBUTING.md.
- No weights, no binaries, no secrets, no third-party code without provenance.
-->

## What & why

<!-- What does this change and what problem does it solve? Link the issue. -->

## Validation performed

<!-- e.g. m11d --selftest vs goldens (features bit-exact, head meandiff ...),
     45-frame m8blive PASS, _smoke/_ui_smoke results, live game test -->

## Scope check

- [ ] ASCII-only comments/identifiers (host quirk)
- [ ] New subsystem lives in its own module (OOP rule, ~600-line soft cap)
- [ ] main.cpp not grown for a new concern
- [ ] Weights / reference code / binaries not committed
- [ ] Docs updated if behavior changed
