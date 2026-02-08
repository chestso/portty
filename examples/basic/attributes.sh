#!/bin/sh
# Text attributes test
printf "\033[1mBold text\033[0m\r\n"
printf "\033[2mDim text\033[0m\r\n"
printf "\033[3mItalic text\033[0m\r\n"
printf "\033[4mUnderlined text\033[0m\r\n"
printf "\033[5mBlinking text\033[0m\r\n"
printf "\033[7mInverted text\033[0m\r\n"
printf "\033[8mHidden text\033[0m\r\n"
printf "\033[9mStrikethrough text\033[0m\r\n"

# Underline styles (extended SGR 4:X)
printf "\r\nUnderline styles:\r\n"
printf "\033[4mDefault underline\033[0m\r\n"
printf "\033[4:1mSingle underline\033[0m\r\n"
printf "\033[4:2mDouble underline\033[0m\r\n"
printf "\033[4:3mCurly underline\033[0m\r\n"
printf "\033[4:4mDotted underline\033[0m\r\n"
printf "\033[4:5mDashed underline\033[0m\r\n"

# Combined attributes
printf "\r\nCombined attributes:\r\n"
printf "\033[1;3;4;31mBold, Italic, Underlined, Red\033[0m\r\n"
printf "\033[1;32;42mBold Green on Green Background\033[0m\r\n"
