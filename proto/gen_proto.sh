#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PROTO_DIR="${PROJECT_ROOT}/proto"
OUT_DIR="${PROJECT_ROOT}/generated/proto"

mkdir -p "${OUT_DIR}"

protoc \
  -I "${PROTO_DIR}" \
  --cpp_out="${OUT_DIR}" \
  "${PROTO_DIR}/rpc_meta.proto" \
  "${PROTO_DIR}/calculator.proto"

echo "Generated protobuf files into ${OUT_DIR}"