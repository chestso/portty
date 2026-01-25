#!/bin/sh
# Window title test
echo -e "\033]0;bloom-term Test Window - Basic\007"
echo -e "Window title set to: bloom-term Test Window - Basic"
sleep 2
echo -e "\033]0;bloom-term Test Window - Updated\007"
echo -e "Window title updated to: bloom-term Test Window - Updated"
sleep 2
echo -e "\033]0;bloom-term\007"
echo -e "Window title reset"
