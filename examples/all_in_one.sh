#!/bin/sh
# Comprehensive test combining all features
printf "\033[2J\r" # Clear screen
printf "\r\n"
printf "\033]0;bloom-term Comprehensive Test\007\r" # Set window title
printf "\r\n"

printf "\033[1;36m=== bloom-term Comprehensive Test ===\033[0m\r\n"
printf "\r\n"

# Colors and attributes
printf "\033[1mBold \033[3mItalic \033[4mUnderline \033[7mReverse\033[0m\r\n"
printf "\033[31mRed \033[32mGreen \033[33mYellow \033[34mBlue \033[35mMagenta \033[36mCyan\033[0m\r\n"
printf "\033[41mRed BG \033[42mGreen BG \033[43mYellow BG \033[44mBlue BG\033[0m\r\n"
printf "\r\n"

# Cursor movement
printf "\r\n\033[10;20HCursor at (10,20)\r\n"
printf "\033[12;1HCursor at (12,1)\r\n"
printf "\r\n"

# Box drawing
printf "\r\n\033[14;1H┌─────────────────────────────┐\r\n"
printf "\033[15;1H│ Unicode Box Drawing Test    │\r\n"
printf "\033[16;1H├─────────────────────────────┤\r\n"
printf "\033[17;1H│ Works with bloom-term      │\r\n"
printf "\033[18;1H└─────────────────────────────┘\r\n"
printf "\r\n"

# Emoji and symbols
printf "\r\n\033[20;1HEmoji test: 😀 🐱 🌍 🚀\r\n"
printf "\033[21;1HSymbols: ∑ ∫ ∞ ≈ ≠ ≤ ≥\r\n"
printf "\r\n"

# 256-color test
printf "\r\n\033[23;1H256-color gradient:\r\n"
for i in {0..15}; do
	for j in {0..15}; do
		color=$((i * 16 + j))
		printf "\033[48;5;${color}m  \033[0m"
	done
	printf "\r\n"
done
printf "\r\n"

# Truecolor test
printf "\r\n\033[30;1HTruecolor gradient (red):\r\n"
for i in {0..79}; do
	r=$((i * 255 / 79))
	printf "\033[48;2;${r};0;0m \033[0m"
done
printf "\r\n"
printf "\r\n"

printf "\033[32;1HTruecolor gradient (green):\r\n"
for i in {0..79}; do
	g=$((i * 255 / 79))
	printf "\033[48;2;0;${g};0m \033[0m"
done
printf "\r\n"
printf "\r\n"

printf "\033[34;1HTruecolor gradient (blue):\r\n"
for i in {0..79}; do
	b=$((i * 255 / 79))
	printf "\033[48;2;0;0;${b}m \033[0m"
done
printf "\r\n"
printf "\r\n"

printf "\r\n\r\n\033[38;1HTest complete! All features demonstrated.\r\n"
printf "\033[39;1HPress any key to exit...\r\n"
read -n 1
