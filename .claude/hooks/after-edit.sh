#!/bin/sh
# PostToolUse hook: regenerates DashboardPage.h when the dashboard or its translations change.
f=$(python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path",""))' 2>/dev/null)
case "$f" in
  */web/dashboard.html|*/web/es.json)
    cd "${CLAUDE_PROJECT_DIR:-.}/FSM_WashingMachineSystem" && python3 tools/build_dashboard.py >&2 || {
      echo "build_dashboard.py failed" >&2; exit 2; } ;;
esac
exit 0
