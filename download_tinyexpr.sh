#!/bin/bash

# Script to download the latest tinyexpr source files from GitHub

set -e  # Exit on any error

TINYEXPR_C_URL="https://github.com/codeplea/tinyexpr/raw/refs/heads/logic/tinyexpr.c"
TINYEXPR_H_URL="https://github.com/codeplea/tinyexpr/raw/refs/heads/logic/tinyexpr.h"

echo "Downloading tinyexpr files from GitHub..."

# Download tinyexpr.c
echo "Downloading tinyexpr.c..."
if command -v curl >/dev/null 2>&1; then
    curl -L -o tinyexpr.c "$TINYEXPR_C_URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O tinyexpr.c "$TINYEXPR_C_URL"
else
    echo "Error: Neither curl nor wget is available. Please install one of them."
    exit 1
fi

# Download tinyexpr.h
echo "Downloading tinyexpr.h..."
if command -v curl >/dev/null 2>&1; then
    curl -L -o tinyexpr.h "$TINYEXPR_H_URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O tinyexpr.h "$TINYEXPR_H_URL"
else
    echo "Error: Neither curl nor wget is available. Please install one of them."
    exit 1
fi

echo "Successfully downloaded tinyexpr.c and tinyexpr.h"
