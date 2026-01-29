#!/bin/sh
# Window title test
echo -e "\033]0;bloom-terminal Test Window - Basic\007"
echo -e "Window title set to: bloom-terminal Test Window - Basic"
sleep 2
echo -e "\033]0;bloom-terminal Test Window - Updated\007"
echo -e "Window title updated to: bloom-terminal Test Window - Updated"
sleep 2
echo -e "\033]0;bloom-terminal\007"
echo -e "Window title reset"
