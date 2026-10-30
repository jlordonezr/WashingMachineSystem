#!/bin/sh
# Builds the Runbook and the Quick Card PDFs from manuals/src/*.html with
# headless Google Chrome. Usage: sh manuals/build.sh
# Set CHROME to the browser binary if it is not in the default macOS place.
set -e
cd "$(dirname "$0")"
CHROME=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
PROFILE=$(mktemp -d)
build() {
  "$CHROME" --headless=new --disable-gpu --no-pdf-header-footer --user-data-dir="$PROFILE" \
    --print-to-pdf="$2" "file://$(pwd)/src/$1" 2>/dev/null
  echo "built $2"
}
build Runbook.html ReuseWater_Runbook.pdf
build QuickCard.html ReuseWater_Quick_Card.pdf
rm -rf "$PROFILE"
