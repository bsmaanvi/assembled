#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Wpedantic -Werror -I. \
    tests/rule_collection_test.cpp rule_collection.cpp -o build/rule-tests
./build/rule-tests
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Wpedantic -Werror -I. \
    tests/notification_engine_test.cpp notification_engine.cpp rule_collection.cpp -o build/notification-tests
./build/notification-tests
python3 tests/configuration_test.py
python3 tests/web_api_test.py
