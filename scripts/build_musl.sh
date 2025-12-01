#!/bin/bash
# scripts/build_musl.sh
# Build fully static musl binaries using Docker + Alpine
#
# This is the only reliable way to get musl C++ binaries on a glibc system.
# musl-gcc on Ubuntu/Debian doesn't support C++ (no libstdc++ headers).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== l2net musl static build ===${NC}"
echo ""

# check for docker
if ! command -v docker &> /dev/null; then
    echo -e "${RED}error: docker not found${NC}"
    echo "install docker: https://docs.docker.com/engine/install/"
    exit 1
fi

# check if docker daemon is running
if ! docker info &> /dev/null; then
    echo -e "${RED}error: docker daemon not running${NC}"
    echo "start docker: sudo systemctl start docker"
    exit 1
fi

cd "$PROJECT_DIR"

OUTPUT_DIR="build/musl-alpine/bin"

echo -e "${YELLOW}building with docker + alpine...${NC}"
echo ""

# build using docker
docker build \
    -f Dockerfile.musl \
    -t l2net-musl-builder \
    --target builder \
    .

# extract binaries from the built image
echo ""
echo -e "${YELLOW}extracting binaries...${NC}"

# create a container and copy files out
CONTAINER_ID=$(docker create l2net-musl-builder)
mkdir -p "$OUTPUT_DIR"
docker cp "$CONTAINER_ID:/src/build/musl-alpine/bin/." "$OUTPUT_DIR/"
docker rm "$CONTAINER_ID" > /dev/null

echo ""
echo -e "${GREEN}=== build complete ===${NC}"
echo ""
echo "binaries:"
ls -lh "$OUTPUT_DIR/"
echo ""

# verify they're static
echo "verification:"
for bin in "$OUTPUT_DIR"/*; do
    if [ -f "$bin" ] && [ -x "$bin" ]; then
        echo -n "  $(basename "$bin"): "
        if file "$bin" | grep -q "statically linked"; then
            echo -e "${GREEN}static ✓${NC}"
        else
            echo -e "${YELLOW}dynamic (may still be portable)${NC}"
        fi
    fi
done

echo ""
echo -e "output directory: ${GREEN}$OUTPUT_DIR${NC}"