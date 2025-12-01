#!/bin/bash
# run_remote_benchmark.sh - convenience wrapper for remote benchmarking
# usage: ./run_remote_benchmark.sh <remote_ip> <interface>

set -e

# colors for output because we're not animals
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # no color

print_banner() {
    echo -e "${CYAN}"
    echo "╔═══════════════════════════════════════════════════════════════╗"
    echo "║          L2NET Remote Benchmark Suite                         ║"
    echo "║          Layer 2 Latency & Throughput Testing                 ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
}

print_usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Required:"
    echo "  -h, --host <ip>         Remote host IP address"
    echo "  -i, --interface <name>  Network interface (same on local and remote)"
    echo ""
    echo "Optional:"
    echo "  -u, --user <username>   SSH username (default: current user)"
    echo "  -p, --password          Prompt for SSH password"
    echo "  -k, --key <path>        Path to SSH private key"
    echo "  -r, --remote-if <name>  Remote interface if different from local"
    echo "  -s, --sizes <list>      Payload sizes (default: 64,256,1024,1400,4096,8192)"
    echo "  -n, --packets <n>       Packets per test (default: 10000)"
    echo "  -o, --output <prefix>   Output file prefix"
    echo "  -v, --verbose           Verbose output"
    echo "  --vlan <id>             VLAN ID"
    echo "  --priority <n>          VLAN priority (0-7)"
    echo ""
    echo "Examples:"
    echo "  $0 -h 192.168.1.100 -i eth0 -u admin -p"
    echo "  $0 -h 10.0.0.50 -i enp0s3 -k ~/.ssh/id_rsa -s 64,1400,8192 -n 50000"
    echo ""
}

# defaults
REMOTE_HOST=""
LOCAL_IF=""
REMOTE_IF=""
SSH_USER="${USER}"
SSH_PASS=""
SSH_KEY=""
PAYLOAD_SIZES="64,256,1024,1400,4096,8192"
PACKETS="10000"
OUTPUT_PREFIX=""
VERBOSE=""
VLAN_ID=""
VLAN_PRIORITY=""

# find the binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="${SCRIPT_DIR}/../build/bin/l2net_remote_benchmark"
REMOTE_NODE="${SCRIPT_DIR}/../build/bin/l2net_remote_node"

# check for binary in common locations
if [[ ! -x "$BINARY" ]]; then
    BINARY="${SCRIPT_DIR}/l2net_remote_benchmark"
fi
if [[ ! -x "$BINARY" ]]; then
    BINARY="$(which l2net_remote_benchmark 2>/dev/null || true)"
fi

if [[ ! -x "$REMOTE_NODE" ]]; then
    REMOTE_NODE="${SCRIPT_DIR}/l2net_remote_node"
fi
if [[ ! -x "$REMOTE_NODE" ]]; then
    REMOTE_NODE="$(which l2net_remote_node 2>/dev/null || true)"
fi

# parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)
            REMOTE_HOST="$2"
            shift 2
            ;;
        -i|--interface)
            LOCAL_IF="$2"
            shift 2
            ;;
        -r|--remote-if)
            REMOTE_IF="$2"
            shift 2
            ;;
        -u|--user)
            SSH_USER="$2"
            shift 2
            ;;
        -p|--password)
            echo -n "SSH Password: "
            read -s SSH_PASS
            echo ""
            shift
            ;;
        -k|--key)
            SSH_KEY="$2"
            shift 2
            ;;
        -s|--sizes)
            PAYLOAD_SIZES="$2"
            shift 2
            ;;
        -n|--packets)
            PACKETS="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_PREFIX="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE="--verbose"
            shift
            ;;
        --vlan)
            VLAN_ID="$2"
            shift 2
            ;;
        --priority)
            VLAN_PRIORITY="$2"
            shift 2
            ;;
        --help)
            print_usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            print_usage
            exit 1
            ;;
    esac
done

# validate required arguments
if [[ -z "$REMOTE_HOST" ]]; then
    echo -e "${RED}Error: --host is required${NC}"
    print_usage
    exit 1
fi

if [[ -z "$LOCAL_IF" ]]; then
    echo -e "${RED}Error: --interface is required${NC}"
    print_usage
    exit 1
fi

# use same interface for remote if not specified
if [[ -z "$REMOTE_IF" ]]; then
    REMOTE_IF="$LOCAL_IF"
fi

# validate binaries exist
if [[ ! -x "$BINARY" ]]; then
    echo -e "${RED}Error: l2net_remote_benchmark not found${NC}"
    echo "Please build the project first:"
    echo "  mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

if [[ ! -x "$REMOTE_NODE" ]]; then
    echo -e "${RED}Error: l2net_remote_node not found${NC}"
    echo "Please build the project first"
    exit 1
fi

# check if running as root (required for raw sockets)
if [[ $EUID -ne 0 ]]; then
    echo -e "${YELLOW}Warning: Not running as root. Raw socket operations require root.${NC}"
    echo "Restarting with sudo..."
    exec sudo "$0" "$@"
fi

print_banner

echo -e "${GREEN}Configuration:${NC}"
echo "  Remote Host:     $REMOTE_HOST"
echo "  SSH User:        $SSH_USER"
echo "  Local Interface: $LOCAL_IF"
echo "  Remote Interface: $REMOTE_IF"
echo "  Payload Sizes:   $PAYLOAD_SIZES"
echo "  Packets/Test:    $PACKETS"
echo ""

# build command
CMD="$BINARY"
CMD+=" --remote-host $REMOTE_HOST"
CMD+=" --ssh-user $SSH_USER"
CMD+=" --local-iface $LOCAL_IF"
CMD+=" --remote-iface $REMOTE_IF"
CMD+=" --binary $REMOTE_NODE"
CMD+=" --payload-sizes $PAYLOAD_SIZES"
CMD+=" --packets $PACKETS"

if [[ -n "$SSH_PASS" ]]; then
    CMD+=" --ssh-pass '$SSH_PASS'"
fi

if [[ -n "$SSH_KEY" ]]; then
    CMD+=" --ssh-key $SSH_KEY"
fi

if [[ -n "$OUTPUT_PREFIX" ]]; then
    CMD+=" --output $OUTPUT_PREFIX"
fi

if [[ -n "$VERBOSE" ]]; then
    CMD+=" $VERBOSE"
fi

if [[ -n "$VLAN_ID" ]]; then
    CMD+=" --vlan $VLAN_ID"
fi

if [[ -n "$VLAN_PRIORITY" ]]; then
    CMD+=" --priority $VLAN_PRIORITY"
fi

echo -e "${CYAN}Starting benchmark...${NC}"
echo ""

# run the benchmark
eval "$CMD"

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"