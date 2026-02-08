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

# Underline colors (SGR 58/59)
printf "\r\nUnderline colors:\r\n"
printf "\033[4;58:2::255:100:100mRed underline\033[0m\r\n"
printf "\033[4;58:2::100:255:100mGreen underline\033[0m\r\n"
printf "\033[4;58:2::100:100:255mBlue underline\033[0m\r\n"
printf "\033[4:3;58:2::255:200:50mCurly orange\033[0m\r\n"
printf "\033[4:2;58:5:196mDouble red (256-color)\033[0m\r\n"
printf "\033[4;58:2::255:0:0mRed\033[59m then default\033[0m\r\n"

# Combined attributes
printf "\r\nCombined attributes:\r\n"
printf "\033[1;3;4;31mBold, Italic, Underlined, Red\033[0m\r\n"
printf "\033[1;32;42mBold Green on Green Background\033[0m\r\n"
