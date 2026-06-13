#!/usr/bin/env bash
# Font scale-up across the HMI UI (user-stories.md "UI Updates" + Lukas
# 2026-06-12: "double the font size on all screens").
#
# Net mapping vs the ORIGINAL sizes: 12->24, 14->28, 16/20->36, 24->48.
# This script applies the second step (from the intermediate 16/18 bump);
# it is NOT idempotent — run once per mapping change.
set -euo pipefail
cd "$(dirname "$0")/src/ui"

FILES=$(find . -name '*.cpp' -o -name '*.h')

# Largest first so freshly-written targets aren't re-mapped.
sed -i 's/lv_font_montserrat_24/lv_font_montserrat_48/g' $FILES
sed -i 's/lv_font_montserrat_20/lv_font_montserrat_36/g' $FILES
sed -i 's/lv_font_montserrat_18/lv_font_montserrat_28/g' $FILES
sed -i 's/lv_font_montserrat_16/lv_font_montserrat_24/g' $FILES

echo "remaining font usage:"
grep -rho 'lv_font_montserrat_[0-9]*' . | sort | uniq -c | sort -rn
