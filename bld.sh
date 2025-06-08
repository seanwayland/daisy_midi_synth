#!/bin/bash

set -e  # Exit immediately if a command exits with a non-zero status

echo "Cleaning..."
make clean

echo "Building..."
make all

echo "Programming..."
make program

echo "Done."

