#!/bin/bash
# run_remote_benchmark.sh - convenience wrapper for remote benchmarking
# usage: ./run_remote_benchmark.sh [options]

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
    echo "  -i, --interface <n>     Network interface (same on local and remote)"
    echo ""
    echo "Authentication (one required):"
    echo "  -p, --password          Prompt for SSH password interactively"
    echo "  --ssh-pass <pass>       SSH password directly (less secure)"
    echo "  -k, --key <path>        Path to SSH private key"
    echo ""
    echo "Optional:"
    echo "  -u, --user <username>   SSH username (default: current user)"
    echo "  -r, --remote-if <n>     Remote interface if different from local"
    echo "  -s, --sizes <list>      Payload sizes (default: 64,256,1024,1400,4096,8192)"
    echo "  -n, --packets <n>       Packets per test (default: 10000)"
    echo "  -o, --output <prefix>   Output file prefix"
    echo "  -v, --verbose           Verbose output"
    echo "  --vlan <id>             VLAN ID"
    echo "  --priority <n>          VLAN priority (0-7)"
    echo "  --skip-sudo-setup       Skip remote sudoers configuration"
    echo ""
    echo "Examples:"
    echo "  $0 -h 192.168.1.100 -i eth0 -u admin -p"
    echo "  $0 -h 192.168.1.100 -i eth0 -u admin --ssh-pass 'secret'"
    echo "  $0 -h 10.0.0.50 -i enp0s3 -k ~/.ssh/id_rsa -s 64,1400,8192 -n 50000"
    echo ""
}

# run ssh command with proper auth
# usage: run_ssh "command"
run_ssh() {
    local cmd="$1"
    if [[ -n "$SSH_KEY" ]]; then
        ssh -i "$SSH_KEY" -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
            "${SSH_USER}@${REMOTE_HOST}" "$cmd"
    elif [[ -n "$SSH_PASS" ]]; then
        sshpass -p "$SSH_PASS" ssh -o StrictHostKeyChecking=accept-new \
            "${SSH_USER}@${REMOTE_HOST}" "$cmd"
    else
        echo -e "${RED}Error: no SSH credentials available${NC}"
        return 1
    fi
}

# run ssh command with tty allocation (needed for interactive sudo)
# usage: run_ssh_tty "command"
run_ssh_tty() {
    local cmd="$1"
    if [[ -n "$SSH_KEY" ]]; then
        ssh -tt -i "$SSH_KEY" -o StrictHostKeyChecking=accept-new \
            "${SSH_USER}@${REMOTE_HOST}" "$cmd"
    elif [[ -n "$SSH_PASS" ]]; then
        sshpass -p "$SSH_PASS" ssh -tt -o StrictHostKeyChecking=accept-new \
            "${SSH_USER}@${REMOTE_HOST}" "$cmd"
    else
        echo -e "${RED}Error: no SSH credentials available${NC}"
        return 1
    fi
}

# setup sudoers on remote host for passwordless l2net_remote_node execution
setup_remote_sudo() {
    echo -e "${CYAN}[*] checking remote sudo configuration...${NC}"
    
    local sudoers_line="${SSH_USER} ALL=(ALL) NOPASSWD: /tmp/l2net_remote_node *"
    local sudoers_file="/etc/sudoers.d/l2net"
    
    # first, do a real test - can we actually run sudo -n on the binary?
    # upload a tiny test script first since binary might not exist yet
    if run_ssh "sudo -n true" 2>/dev/null; then
        echo -e "${GREEN}[✓] sudo NOPASSWD already working for ${SSH_USER}${NC}"
        return 0
    fi
    
    # sudo -n failed, let's diagnose why
    echo -e "${YELLOW}[!] sudo NOPASSWD not working, diagnosing...${NC}"
    
    # check what groups the user is in
    local user_groups
    user_groups=$(run_ssh "groups" 2>/dev/null)
    
    # check if user is in wheel or sudo group (these often override user-specific rules)
    local in_wheel=0
    local in_sudo_group=0
    if echo "$user_groups" | grep -qw "wheel"; then
        in_wheel=1
        echo -e "${YELLOW}[!] user is in 'wheel' group${NC}"
    fi
    if echo "$user_groups" | grep -qw "sudo"; then
        in_sudo_group=1
        echo -e "${YELLOW}[!] user is in 'sudo' group${NC}"
    fi
    
    # check if sudoers.d directory exists
    if ! run_ssh "test -d /etc/sudoers.d" 2>/dev/null; then
        echo -e "${RED}[✗] /etc/sudoers.d not found on remote host${NC}"
        echo -e "${YELLOW}    please manually configure sudoers on remote host${NC}"
        return 1
    fi
    
    echo -e "${CYAN}[*] configuring sudoers on remote host...${NC}"
    echo -e "${YELLOW}    (you may be prompted for your sudo password on the remote host)${NC}"
    
    # if user is in wheel/sudo group, we need to handle the override issue
    # sudoers uses last-match-wins, so group rules can override user rules
    if [[ $in_wheel -eq 1 ]] || [[ $in_sudo_group -eq 1 ]]; then
        echo -e "${YELLOW}[!] detected group membership that may override NOPASSWD rules${NC}"
        echo -e "${CYAN}[*] configuring group NOPASSWD to avoid override issue...${NC}"
        
        # the cleanest fix: make the group NOPASSWD too (for dev machines)
        # we'll create a drop-in that gives wheel/sudo NOPASSWD
        local group_fix=""
        if [[ $in_wheel -eq 1 ]]; then
            group_fix="%wheel ALL=(ALL:ALL) NOPASSWD: ALL"
        elif [[ $in_sudo_group -eq 1 ]]; then
            group_fix="%sudo ALL=(ALL:ALL) NOPASSWD: ALL"
        fi
        
        echo -e "${YELLOW}[!] this will enable NOPASSWD for the wheel/sudo group (dev machine setting)${NC}"
        echo -e "${YELLOW}    if this is a production machine, configure manually instead${NC}"
        
        # create drop-in file with high priority (zz- prefix ensures it's read last)
        if run_ssh_tty "echo '${group_fix}' | sudo tee /etc/sudoers.d/zz-nopasswd > /dev/null && sudo chmod 440 /etc/sudoers.d/zz-nopasswd"; then
            echo -e "${GREEN}[✓] group NOPASSWD rule created${NC}"
        else
            echo -e "${RED}[✗] failed to create group NOPASSWD rule${NC}"
            echo -e "${YELLOW}    please manually run on remote host:${NC}"
            echo -e "    sudo visudo"
            echo -e "    # add at the END of the file (after %wheel or %sudo line):${NC}"
            echo -e "    ${group_fix}"
            return 1
        fi
    else
        # user not in wheel/sudo, just add the specific rule
        if run_ssh_tty "echo '${sudoers_line}' | sudo tee ${sudoers_file} > /dev/null && sudo chmod 440 ${sudoers_file}"; then
            echo -e "${GREEN}[✓] sudoers rule created${NC}"
        else
            echo -e "${RED}[✗] failed to create sudoers rule${NC}"
            echo -e "${YELLOW}    please manually run on remote host:${NC}"
            echo -e "    sudo bash -c \"echo '${sudoers_line}' > ${sudoers_file} && chmod 440 ${sudoers_file}\""
            return 1
        fi
    fi
    
    # validate it actually works now
    sleep 1  # give sudo time to reload
    if run_ssh "sudo -n true" 2>/dev/null; then
        echo -e "${GREEN}[✓] verified: sudo NOPASSWD is working${NC}"
        return 0
    else
        echo -e "${RED}[✗] sudo NOPASSWD still not working after configuration${NC}"
        echo -e "${YELLOW}    please check /etc/sudoers manually on remote host${NC}"
        echo -e "${YELLOW}    common issues:${NC}"
        echo -e "${YELLOW}      - sudoers syntax error (run 'sudo visudo' to check)${NC}"
        echo -e "${YELLOW}      - rule ordering (NOPASSWD rules must come AFTER group rules)${NC}"
        echo -e "${YELLOW}      - requiretty setting (add 'Defaults:${SSH_USER} !requiretty')${NC}"
        return 1
    fi
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
SKIP_SUDO_SETUP=""

# find the binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# check common build locations in order of likelihood
BINARY=""
REMOTE_NODE=""

SEARCH_DIRS=(
  # colcon/ament typical
  "build/l2net/bin"
  "install/l2net/bin"
  "install/l2net/lib/l2net"

  # generic "build/*/bin" layout (colcon/other multi-package build trees)
  "build/*/bin"

  # normal CMake presets / common conventions
  "build/release/bin"
  "build/Release/bin"
  "build/bin"
  "build"
  "cmake-build-release/bin"
  "cmake-build-release"
)

# helper: check a directory for both binaries
check_dir_for_bins() {
    local dir="$1"

    if [[ -x "$dir/l2net_remote_benchmark" && -z "$BINARY" ]]; then
        BINARY="$dir/l2net_remote_benchmark"
    fi

    if [[ -x "$dir/l2net_remote_node" && -z "$REMOTE_NODE" ]]; then
        REMOTE_NODE="$dir/l2net_remote_node"
    fi
}

# search
for build_dir in "${SEARCH_DIRS[@]}"; do
    # Expand globs safely (build/*/bin)
    for expanded in "$PROJECT_ROOT"/$build_dir; do
        if [[ -d "$expanded" ]]; then
            check_dir_for_bins "$expanded"
        fi
        if [[ -n "$BINARY" && -n "$REMOTE_NODE" ]]; then
            break 2
        fi
    done
done

# fallback to PATH
if [[ -z "$BINARY" ]]; then
    BINARY="$(command -v l2net_remote_benchmark 2>/dev/null || true)"
fi
if [[ -z "$REMOTE_NODE" ]]; then
    REMOTE_NODE="$(command -v l2net_remote_node 2>/dev/null || true)"
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
        --ssh-pass)
            SSH_PASS="$2"
            shift 2
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
        --skip-sudo-setup)
            SKIP_SUDO_SETUP="1"
            shift
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

# validate authentication
if [[ -z "$SSH_PASS" && -z "$SSH_KEY" ]]; then
    echo -e "${RED}Error: authentication required. Use -p, --ssh-pass, or -k${NC}"
    print_usage
    exit 1
fi

# validate binaries exist
if [[ -z "$BINARY" || ! -x "$BINARY" ]]; then
    echo -e "${RED}Error: l2net_remote_benchmark not found${NC}"
    echo "Searched in:"
    for build_dir in "${SEARCH_DIRS[@]}"; do
        echo "  - $PROJECT_ROOT/$build_dir"
    done
    echo ""
    echo "Please build the project first. For plain CMake builds:"
    echo "  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build/release -j"
    echo ""
    echo "For colcon builds:"
    echo "  colcon build --packages-select l2net --cmake-args -DCMAKE_BUILD_TYPE=Release"
    exit 1
fi

if [[ -z "$REMOTE_NODE" || ! -x "$REMOTE_NODE" ]]; then
    echo -e "${RED}Error: l2net_remote_node not found${NC}"
    echo "Searched in:"
    for build_dir in "${SEARCH_DIRS[@]}"; do
        echo "  - $PROJECT_ROOT/$build_dir"
    done
    echo ""
    echo "Please build the project first. For plain CMake builds:"
    echo "  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build/release -j"
    echo ""
    echo "For colcon builds:"
    echo "  colcon build --packages-select l2net --cmake-args -DCMAKE_BUILD_TYPE=Release"
    exit 1
fi

# check for sshpass if using password auth
if [[ -n "$SSH_PASS" ]] && ! command -v sshpass &> /dev/null; then
    echo -e "${RED}Error: sshpass is required for password authentication${NC}"
    echo "Install with: apt install sshpass"
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
echo "  Remote Host:      $REMOTE_HOST"
echo "  SSH User:         $SSH_USER"
echo "  Local Interface:  $LOCAL_IF"
echo "  Remote Interface: $REMOTE_IF"
echo "  Payload Sizes:    $PAYLOAD_SIZES"
echo "  Packets/Test:     $PACKETS"
echo "  Binary:           $BINARY"
echo "  Remote Node:      $REMOTE_NODE"
echo ""

# setup sudo on remote host (unless skipped)
if [[ -z "$SKIP_SUDO_SETUP" ]]; then
    if ! setup_remote_sudo; then
        echo -e "${RED}Failed to configure remote sudo. Use --skip-sudo-setup to bypass.${NC}"
        exit 1
    fi
    echo ""
fi

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