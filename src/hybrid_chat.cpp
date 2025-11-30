#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <net/ethernet.h>
#include <net/if.h>
#include <netpacket/packet.h> // <--- ADDED: Required for sockaddr_ll
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// --- Configuration ---
const int TCP_PORT = 9000;
const uint16_t ETH_P_CUSTOM = 0x88B5; // Our data protocol

// We removed ETH_P_8021Q definition because it is already in <net/ethernet.h>
const int TARGET_VLAN_ID = 10;   // Arbitrary VLAN
const int PRIORITY_CRITICAL = 7; // Highest Priority (0-7)

// --- Structures ---

// We must pack this struct to prevent compiler padding
struct vlan_header
{
    uint16_t tpid; // Tag Protocol Identifier (0x8100)
    uint16_t tci;  // Tag Control Information (Prio + CFI + VID)
} __attribute__((packed));

struct InterfaceInfo
{
    int index;
    unsigned char mac[6];
    std::string name;
};

// --- Helper Functions ---

InterfaceInfo get_local_mac(const char *iface_name)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    InterfaceInfo info;
    info.name = iface_name;

    strcpy(ifr.ifr_name, iface_name);

    // Get Index
    ioctl(sock, SIOCGIFINDEX, &ifr);
    info.index = ifr.ifr_ifindex;

    // Get MAC
    ioctl(sock, SIOCGIFHWADDR, &ifr);
    memcpy(info.mac, ifr.ifr_hwaddr.sa_data, 6);

    close(sock);
    return info;
}

std::string mac_to_string(unsigned char *mac)
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// --- Phase 1: TCP Handshake ---

// Returns the MAC address of the connected peer
void run_tcp_server(InterfaceInfo &my_info, unsigned char *peer_mac_out)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Allow address reuse
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 1);

    std::cout << "[Control Plane] Waiting for client connection on TCP " << TCP_PORT << "..." << std::endl;

    int new_socket = accept(server_fd, NULL, NULL);

    // 1. Send my MAC
    send(new_socket, my_info.mac, 6, 0);

    // 2. Receive client MAC
    recv(new_socket, peer_mac_out, 6, 0);

    std::cout << "[Control Plane] Handshake complete. Client is at " << mac_to_string(peer_mac_out) << std::endl;

    close(new_socket);
    close(server_fd);
}

void run_tcp_client(const char *server_ip, InterfaceInfo &my_info, unsigned char *peer_mac_out)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, server_ip, &serv_addr.sin_addr);

    std::cout << "[Control Plane] Connecting to " << server_ip << "..." << std::endl;
    while (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        std::cout << "Retrying..." << std::endl;
        sleep(1);
    }

    // 1. Receive Server MAC
    recv(sock, peer_mac_out, 6, 0);

    // 2. Send my MAC
    send(sock, my_info.mac, 6, 0);

    std::cout << "[Control Plane] Handshake complete. Server is at " << mac_to_string(peer_mac_out) << std::endl;
    close(sock);
}

// --- Phase 2: Low Level L2 Data ---

void send_raw_tagged(InterfaceInfo &my_info, unsigned char *dest_mac)
{
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

    std::string msg = "HIGH PRIORITY DATA";

    // Calculate sizes
    // Header = Ethernet(14) + VLAN(4)
    int header_size = sizeof(struct ether_header) + sizeof(struct vlan_header);
    std::vector<uint8_t> buffer(header_size + msg.size());

    // --- Construct Frame ---

    // 1. Destination MAC (Unicast to the peer we found via TCP)
    memcpy(buffer.data(), dest_mac, 6);

    // 2. Source MAC
    memcpy(buffer.data() + 6, my_info.mac, 6);

    // 3. VLAN Tag (Insert at byte 12)
    struct vlan_header *vh = (struct vlan_header *)(buffer.data() + 12);

    // Use the system defined ETH_P_8021Q
    vh->tpid = htons(ETH_P_8021Q);

    // TCI Calculation:
    // Priority (3 bits) << 13 | CFI (1 bit) << 12 | VLAN ID (12 bits)
    uint16_t tci = (PRIORITY_CRITICAL << 13) | TARGET_VLAN_ID;
    vh->tci = htons(tci);

    // 4. Real EtherType (Insert at byte 16)
    uint16_t *proto_field = (uint16_t *)(buffer.data() + 16);
    *proto_field = htons(ETH_P_CUSTOM);

    // 5. Payload
    memcpy(buffer.data() + header_size, msg.c_str(), msg.size());

    // --- Send ---
    struct sockaddr_ll socket_address;
    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sll_family = AF_PACKET;
    socket_address.sll_ifindex = my_info.index;
    socket_address.sll_halen = ETH_ALEN;
    memcpy(socket_address.sll_addr, dest_mac, 6);

    std::cout << "[Data Plane] Sending L2 frames with VLAN Priority " << PRIORITY_CRITICAL << "..." << std::endl;

    while (true)
    {
        sendto(sockfd, buffer.data(), buffer.size(), 0,
               (struct sockaddr *)&socket_address, sizeof(socket_address));
        usleep(500000); // 2Hz
    }
}

void listen_raw(InterfaceInfo &my_info)
{
    // We listen for ALL traffic to catch the VLAN frames,
    // because sometimes the kernel doesn't strip the tag for raw sockets.
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

    uint8_t buffer[2048];
    std::cout << "[Data Plane] Listening for Custom Protocol..." << std::endl;

    while (true)
    {
        int len = recv(sockfd, buffer, 2048, 0);

        // Check if it's a VLAN frame (TPID at byte 12 is 0x8100)
        uint16_t tpid = ntohs(*(uint16_t *)(buffer + 12));

        // Use system defined ETH_P_8021Q
        if (tpid == ETH_P_8021Q)
        {
            // It is tagged. The REAL protocol is at byte 16.
            uint16_t real_proto = ntohs(*(uint16_t *)(buffer + 16));

            if (real_proto == ETH_P_CUSTOM)
            {
                // Extract TCI to see priority
                uint16_t tci = ntohs(*(uint16_t *)(buffer + 14));
                int priority = (tci >> 13) & 0x7; // Top 3 bits

                char *payload = (char *)(buffer + 18); // 14 (eth) + 4 (vlan)

                std::cout << "Recv [VLAN Prio " << priority << "]: "
                          << std::string(payload, len - 18) << std::endl;
            }
        }
        // Also handle untagged frames just in case switch stripped it
        else
        {
            uint16_t proto = ntohs(*(uint16_t *)(buffer + 12));
            if (proto == ETH_P_CUSTOM)
            {
                char *payload = (char *)(buffer + 14);
                std::cout << "Recv [Untagged]: " << std::string(payload, len - 14) << std::endl;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: sudo ./hybrid_chat <interface> <mode> [server_ip]\n"
                  << "  Server: sudo ./hybrid_chat eth0 server\n"
                  << "  Client: sudo ./hybrid_chat eth0 client 192.168.1.50\n";
        return 1;
    }

    InterfaceInfo my_info = get_local_mac(argv[1]);
    std::string mode = argv[2];
    unsigned char peer_mac[6];

    // --- PHASE 1: HANDSHAKE ---
    if (mode == "server")
    {
        run_tcp_server(my_info, peer_mac);
        // Server becomes receiver in this example
        listen_raw(my_info);
    }
    else if (mode == "client")
    {
        if (argc < 4)
        {
            std::cerr << "Client needs server IP\n";
            return 1;
        }
        run_tcp_client(argv[3], my_info, peer_mac);
        // Client becomes sender
        send_raw_tagged(my_info, peer_mac);
    }

    return 0;
}