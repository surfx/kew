#!/bin/bash
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
ROOT_DIR=$(dirname "$SCRIPT_DIR")
"$SCRIPT_DIR/kill.sh"
sudo pacman -S --needed --noconfirm opusfile chafa
cd "$ROOT_DIR" && make clean && make && sudo make install
