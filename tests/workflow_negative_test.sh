#!/bin/sh
# Run workflow mutations and checksum fixtures without executing the SDK.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 "$project/tests/workflow_regression_test.py"
