#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
"${CXX:-g++}" -std=c++20 -O0 -Wall -Wextra -Wpedantic -pthread -I. \
    server.cpp application.cpp engine_state.cpp notification_engine.cpp rule_collection.cpp \
    -o build/intraday-server
