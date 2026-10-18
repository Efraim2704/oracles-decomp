#!/bin/sh
# Regenerate everything the European ROMs need after the C or the USA symbols change.
#
# usage: tools/eu_regen.sh AGES_EU.gbc SEASONS_EU.gbc
#
# Needs the USA ROMs in roms/ and the disassembly built (ref/oracles-disasm/{ages,seasons}.sym).
# ref/eu/*.sym and src/hooks/{extra,alias}*_eu.sym come from tools/eu_sym.py and only change when
# the USA symbols move; everything after them is rebuilt here:
#   1. tools/eu_hooks.py: the USA hooks the EU ROM can run unchanged -> generated[_seasons]_eu.txt
#   2. tools/eu_gen.py: C for the rest, from the EU ROM -> src/game/eu/gen_*.c, generated*_eu_gen.txt
#   3. tools/gen_hooks.py: the EU hook tables
#   4. tools/gen_syms.py: the EU symbol columns
#   5. tools/codemap.py: the EU code maps (the USA ones are rewritten too)
#   6. tools/eu_operands.py: every SYM() pointer the shared C builds, against the EU operands
set -e
AGES_EU=$1
SEASONS_EU=$2
[ -f "$AGES_EU" ] && [ -f "$SEASONS_EU" ] || { echo "usage: $0 AGES_EU.gbc SEASONS_EU.gbc" >&2; exit 2; }
cd "$(dirname "$0")/.."
R="roms/Legend of Zelda, The - Oracle of"
# The labels eu_gen.py names in European code are read back by eu_hooks.py, so the last run's would
# place a few USA hooks only because eu_gen.py named their entry once, and the two would swap each
# run. Start every run without them.
for g in "" _seasons; do
  printf '# entry points tools/eu_gen.py names in European code (regenerated)\n' > "src/hooks/labels${g}_eu.sym"
done
python3 tools/eu_hooks.py ages "$R Ages (USA, Australia).gbc" "$AGES_EU"
python3 tools/eu_hooks.py seasons "$R Seasons (USA, Australia).gbc" "$SEASONS_EU"
python3 tools/eu_gen.py ages "$AGES_EU"
python3 tools/eu_gen.py seasons "$SEASONS_EU"
python3 tools/gen_hooks.py ref/eu/ages_eu.sym src/hooks/generated_eu.txt+src/hooks/generated_eu_gen.txt src/hooks/table_eu.h
python3 tools/gen_hooks.py ref/eu/seasons_eu.sym src/hooks/generated_seasons_eu.txt+src/hooks/generated_seasons_eu_gen.txt src/hooks/table_seasons_eu.h
python3 tools/gen_syms.py
python3 tools/codemap.py "$R Ages (USA, Australia).gbc" ref/oracles-disasm/ages.sym \
  "$R Seasons (USA, Australia).gbc" ref/oracles-disasm/seasons.sym "$AGES_EU" "$SEASONS_EU"
python3 tools/eu_operands.py ages "$AGES_EU"
python3 tools/eu_operands.py seasons "$SEASONS_EU"
