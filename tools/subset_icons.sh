#!/usr/bin/env bash
# Build assets/fonts/icons.ttf: a tiny subset of Material Design Icons that
# holds only the glyphs the UI uses. The full font is ~1.3 MB; the subset is a
# few KB, which keeps flash small and glyph lookups fast.
#
# Usage: tools/subset_icons.sh path/to/materialdesignicons-webfont.ttf
# Needs fonttools (pip install fonttools).
#
# When you add an icon: add its codepoint here AND a matching #define in
# components/deck_ui/include/deck_icons.h, then rerun this script.
set -euo pipefail

src="${1:?usage: $0 path/to/mdi.ttf}"
root="$(cd "$(dirname "$0")/.." && pwd)"

codepoints=(
    F018D  # console
    F0354  # language-markdown
    F0A9A  # calculator-variant
    F0595  # weather-partly-cloudy
    F0590  # weather-cloudy
    F0493  # cog
    F030C  # keyboard
    F0310  # keyboard-off
    F0479  # sd
    F05A9  # wifi
    F05AA  # wifi-off
    F0079  # battery
    F0084  # battery-charging
    F008E  # battery-outline
    F06A5  # power-plug
    F0141  # chevron-left
    F061A  # chip
    F035B  # memory
    F0150  # clock-outline
    F00DF  # brightness-6
    F03D8  # palette
    F0379  # monitor
    F0026  # alert
    F140B  # lightning-bolt
    F02DC  # home
    F006E  # backspace
    F02DA  # history
    F097B  # keyboard-outline
    F048D  # server-network
    F0599  # weather-sunny
    F0594  # weather-night
    F0F31  # weather-night-partly-cloudy
    F0591  # weather-fog
    F0597  # weather-rainy
    F0596  # weather-pouring
    F0598  # weather-snowy
    F0F36  # weather-snowy-heavy
    F067F  # weather-snowy-rainy
    F067E  # weather-lightning-rainy
    F0592  # weather-hail
    F059D  # weather-windy
    F059C  # weather-sunset-up
    F059B  # weather-sunset-down
    F058E  # water-percent
    F029A  # gauge
    F050F  # thermometer
    F034E  # map-marker
    F0450  # refresh
    F054A  # umbrella
)

unicodes=$(printf 'U+%s,' "${codepoints[@]}")
pyftsubset "$src" \
    --unicodes="${unicodes%,}" \
    --no-hinting \
    --output-file="$root/assets/fonts/icons.ttf"
echo "wrote assets/fonts/icons.ttf ($(wc -c < "$root/assets/fonts/icons.ttf") bytes)"
