#!/usr/bin/env bash
# rtplot uninstall script.
#
# Usage:
#   ./scripts/uninstall.sh [path/to/install_manifest.txt]
#   cmake --build build --target uninstall
set -u

manifest="${1:-build/install_manifest.txt}"

if [ ! -f "$manifest" ]; then
  echo "install_manifest.txt not found: $manifest" >&2
  echo "Run 'cmake --install build' first." >&2
  exit 1
fi

removed=0
while IFS= read -r file; do
  if [ -e "$file" ] || [ -L "$file" ]; then
    rm -f -- "$file"
    echo "removed: $file"
    removed=$((removed + 1))
  fi
done < "$manifest"

echo "rtplot uninstall complete: $removed file(s) removed."
