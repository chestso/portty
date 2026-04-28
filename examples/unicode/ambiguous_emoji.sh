#!/bin/sh
# Emoji presentation: text-default vs emoji-default codepoints.
#
# Two distinct Unicode groups, often confused:
#
# Group A — Emoji_Presentation=No (text-default, ambiguous width)
#   Bare base: 1 cell, monochrome.
#   With VS16 (U+FE0F): 2 cells, emoji presentation.
#   This is the "ambiguous" case bloom's emoji width paradigm covers.
#
# Group B — Emoji_Presentation=Yes (emoji-default, already wide)
#   Bare base: 2 cells, emoji presentation.
#   With VS16: still 2 cells. VS16 is a no-op here.
#   Listed in bloom-vt's UAX #11 WIDE table; not ambiguous.
#
# See README.md "Emoji Width Paradigm".
printf "Group A — text-default (bare=1 cell, VS16=2 cells):\r\n"
printf "\r\n"
printf "  Bare    FE0F    Name\r\n"
printf "  ⚠  vs  ⚠️   Warning sign\r\n"
printf "  ☀  vs  ☀️   Sun\r\n"
printf "  ☁  vs  ☁️   Cloud\r\n"
printf "  ☂  vs  ☂️   Umbrella\r\n"
printf "  ☃  vs  ☃️   Snowman\r\n"
printf "  ☄  vs  ☄️   Comet\r\n"
printf "  ☎  vs  ☎️   Telephone\r\n"
printf "  ☘  vs  ☘️   Shamrock\r\n"
printf "  ☠  vs  ☠️   Skull and crossbones\r\n"
printf "  ☢  vs  ☢️   Radioactive\r\n"
printf "  ☣  vs  ☣️   Biohazard\r\n"
printf "  ☮  vs  ☮️   Peace\r\n"
printf "  ☯  vs  ☯️   Yin yang\r\n"
printf "  ♻  vs  ♻️   Recycling\r\n"
printf "  ⛏  vs  ⛏️   Pick\r\n"
printf "  ⛩  vs  ⛩️   Shinto shrine\r\n"
printf "  ⛰  vs  ⛰️   Mountain\r\n"
printf "  ⛱  vs  ⛱️   Umbrella on ground\r\n"
printf "  ⛷  vs  ⛷️   Skier\r\n"
printf "  ⛸  vs  ⛸️   Ice skate\r\n"
printf "  ⛹  vs  ⛹️   Person bouncing ball\r\n"
printf "\r\n"
printf "Group A alignment check (bare = 1 cell, VS16 = 2 cells):\r\n"
printf "  |⚠|☀|☁|☘|⛏|  bare (1 cell each)\r\n"
printf "  |A|B|C|D|E|  ASCII reference (1 cell each)\r\n"
printf "  |⚠️|☀️|☁️|☘️|⛏️|  with VS16 (2 cells each)\r\n"
printf "  |AB|CD|EF|GH|IJ|  ASCII reference (2 chars each)\r\n"
printf "\r\n"
printf "Group B — emoji-default (bare=2 cells, VS16 no-op):\r\n"
printf "\r\n"
printf "  Bare    FE0F    Name\r\n"
printf "  ♈  vs  ♈️   Aries\r\n"
printf "  ⚡  vs  ⚡️   High voltage\r\n"
printf "  ⚽  vs  ⚽️   Soccer ball\r\n"
printf "  ⚾  vs  ⚾️   Baseball\r\n"
printf "  ⛄  vs  ⛄️   Snowman without snow\r\n"
printf "  ⛅  vs  ⛅️   Sun behind cloud\r\n"
printf "  ⛎  vs  ⛎️   Ophiuchus\r\n"
printf "  ⛔  vs  ⛔️   No entry\r\n"
printf "  ⛲  vs  ⛲️   Fountain\r\n"
printf "  ⛳  vs  ⛳️   Flag in hole\r\n"
printf "  ⛵  vs  ⛵️   Sailboat\r\n"
printf "  ⛺  vs  ⛺️   Tent\r\n"
printf "  ⛽  vs  ⛽️   Fuel pump\r\n"
printf "\r\n"
printf "Group B alignment check (both rows = 2 cells each):\r\n"
printf "  |⚡|⚽|⛄|⛔|⛽|  bare (2 cells each)\r\n"
printf "  |⚡️|⚽️|⛄️|⛔️|⛽️|  with VS16 (2 cells each, identical)\r\n"
printf "  |AB|CD|EF|GH|IJ|  ASCII reference (2 chars each)\r\n"
