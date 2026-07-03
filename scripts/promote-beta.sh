#!/usr/bin/env bash
# Promote the beta channel: fast-forward main to the current beta tip.
# Fails (on purpose) if main has commits beta lacks — reconcile first, e.g.
# merge main into beta, verify /beta/, then promote again.
set -euo pipefail
git fetch origin
echo "beta tip:  $(git log -1 --oneline origin/beta)"
echo "main tip:  $(git log -1 --oneline origin/main)"
git push origin origin/beta:refs/heads/main
echo "main fast-forwarded to beta — pages will redeploy both channels."
