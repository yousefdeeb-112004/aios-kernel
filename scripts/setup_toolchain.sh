#!/bin/bash
set -e
sudo apt update
sudo apt install -y build-essential gcc-multilib qemu-system-x86
echo "Done! make && make run"
