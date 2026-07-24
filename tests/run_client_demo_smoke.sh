#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

SERVER="${BUILD_DIR}/server_main"
CLIENT="${BUILD_DIR}/client_main"

TARGET="127.0.0.1:19090"

LOG_DIR="${BUILD_DIR}/test-logs"
mkdir -p "${LOG_DIR}"

SERVER_LOG="${LOG_DIR}/server_smoke.log"
CLIENT_LOG="${LOG_DIR}/client_smoke.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "[smoke] starting server..."
"${SERVER}" >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

sleep 1

echo "[smoke] running client..."
timeout 15s "${CLIENT}" "${TARGET}" >"${CLIENT_LOG}" 2>&1

echo "[smoke] checking client output..."

grep -q "Connected to ${TARGET}" "${CLIENT_LOG}"
grep -q "Calculator.Add result: 1 + 2 = 3" "${CLIENT_LOG}"
grep -q "Chat.Generate finished successfully" "${CLIENT_LOG}"

echo "[smoke] passed"