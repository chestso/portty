#!/bin/sh
# VS16 widening regression test.
#
# Per bloom's emoji width paradigm (README.md "Emoji Width Paradigm"):
#   - bare symbol  -> 1 cell (text presentation)
#   - symbol+VS16  -> 2 cells (emoji presentation)
#
# These four codepoints were previously rendered as 1 cell even when
# followed by U+FE0F (VS16). libvterm has no VS16-aware width API, and
# the earlier render-time override only covered the is_ambiguous_emoji
# range (U+2600-U+27BF), missing U+1F3D6/D9/DB in is_emoji_base_range.
# The fix enforces the paradigm in convert_vterm_screen_cell()
# (src/term_vt.c) so cell.width is authoritative.

printf "VS16 widening (bare = 1 cell, VS16 = 2 cells):\r\n"
printf "\r\n"
printf "  Codepoint  Bare  VS16  Name                  Range\r\n"
printf "  U+26A0     |⚠|   |⚠️|  Warning sign          is_ambiguous_emoji\r\n"
printf "  U+1F3D6    |🏖|   |🏖️|  Beach with umbrella   is_emoji_base_range\r\n"
printf "  U+1F3D9    |🏙|   |🏙️|  Cityscape             is_emoji_base_range\r\n"
printf "  U+1F3DB    |🏛|   |🏛️|  Classical building    is_emoji_base_range\r\n"
printf "\r\n"
printf "Alignment check (pipes should align vertically):\r\n"
printf "  Bare (1 cell):    |⚠|🏖|🏙|🏛|\r\n"
printf "  ASCII reference:  |A|B|C|D|\r\n"
printf "  VS16 (2 cells):   |⚠️|🏖️|🏙️|🏛️|\r\n"
printf "  ASCII reference:  |AB|CD|EF|GH|\r\n"
