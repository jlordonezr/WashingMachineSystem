#!/bin/sh
# Stop hook: runs the full PC test suite when firmware/app files changed.
# Exit 2 sends the failures back to Claude so it fixes them before finishing.
input=$(cat)
echo "$input" | grep -Eq '"stop_hook_active" *: *true' && exit 0   # avoid loops
cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0
git status --porcelain -- FSM_WashingMachineSystem | grep -Eq '\.(ino|cpp|h|html|json|js|py|sh)$' || exit 0
out=$(sh FSM_WashingMachineSystem/tests/run_all.sh 2>&1); rc=$?
if [ $rc -ne 0 ]; then
  echo "$out" | tail -60 >&2
  echo "tests/run_all.sh failed. Fix the failures (or explain why they are expected) before finishing." >&2
  exit 2
fi
exit 0
