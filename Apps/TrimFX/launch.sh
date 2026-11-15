#!/bin/sh
# TrimFX's launcher, run by MustardOS through mux_launch.sh. Keeps all four cores online at full
# speed while it runs, and puts the governor back after.
APP=trimfx
cd "$(dirname "$0")"

CPU=/sys/devices/system/cpu
gov_was=$(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null)
for c in 0 1 2 3; do
    [ -f $CPU/cpu$c/online ] && echo 1 > $CPU/cpu$c/online 2>/dev/null
done
echo performance > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null

# MustardOS's SETUP_APP has already put its own SDL2 first
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/usr/lib"

# Optional settings (remove the # to use one):
# export TFX_BUFFER=256         # lower latency (512 by default; 1024 if it crackles)
# export TFX_AUDIO=Scarlett     # output: the device whose name has this in it
# export TFX_AUDIO=internal     # output: the speaker / headphones (by default they stay silent with no USB output)
# export TFX_INPUT=Scarlett     # input: the same, for the capture side
# export TFX_JOYMAP=A=1,B=0     # another firmware's button numbers
# export TFX_LEDS=0             # leave the LEDs alone
# export TFX_LED_BAR=meter      # top bar: the output level meter instead of the chain
# export TFX_LOG=2              # a status line every second in log.txt, and every button press
# export TFX_PIN=0              # let the audio threads run on any core (by default: output on core 3, input on core 2)
# export TFX_UPDATE=0           # never check GitHub for a new version (an update replaces this file:
#                               #   set it again after)

# exit status 75: the app has just updated itself, so start the new version
while :; do
    ./"$APP" > log.txt 2>&1
    [ $? -eq 75 ] || break
done
sync

[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
