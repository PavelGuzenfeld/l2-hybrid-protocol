// ipc_l2.cpp
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <net/ethernet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Configuration
const char *IFACE = "lo";         // Loopback
const uint16_t PROTO_ID = 0xAAAA; // Unique ID for this specific IPC channel

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: sudo ./ipc_l2 <send|recv>\n";
        return 1;
    }

    // 1. Create Raw Socket (Layer 2)
    // We bind ONLY to our specific protocol to filter out any other system noise
    int sock = socket(AF_PACKET, SOCK_RAW, htons(PROTO_ID));

    // Get Interface Index for 'lo'
    int ifindex = if_nametoindex(IFACE);

    // Bind specifically to 'lo' (Critical! Otherwise we catch eth0 traffic too)
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(PROTO_ID);
    sll.sll_ifindex = ifindex;
    bind(sock, (struct sockaddr *)&sll, sizeof(sll));

    if (std::string(argv[1]) == "recv")
    {
        std::cout << "IPC Receiver Listening on Proto 0xAAAA...\n";

        // Huge buffer for Loopback Jumbo Frames
        std::vector<uint8_t> buffer(70000);

        while (true)
        {
            // We don't need 'from_addr' because we know it's from localhost
            int len = recv(sock, buffer.data(), buffer.size(), 0);

            if (len > 14)
            { // 14 bytes = Ethernet Header size
                // Skip header, print payload
                char *payload = (char *)(buffer.data() + 14);
                std::cout << "Got " << (len - 14) << " bytes: "
                          << std::string(payload, std::min(len - 14, 50)) << "..." << std::endl;
            }
        }
    }
    else
    { // Sender
        std::string msg = "High performance L2 IPC message";

        // Packet = Header (14) + Data
        std::vector<uint8_t> packet(14 + msg.size());

        // Fill Dummy Header (MACs are ignored on lo, but header MUST exist)
        struct ether_header *eh = (struct ether_header *)packet.data();
        memset(eh->ether_dhost, 0, 6); // Ignored
        memset(eh->ether_shost, 0, 6); // Ignored
        eh->ether_type = htons(PROTO_ID);

        memcpy(packet.data() + 14, msg.c_str(), msg.size());

        // Target Address
        struct sockaddr_ll addr;
        memset(&addr, 0, sizeof(addr));
        addr.sll_family = AF_PACKET;
        addr.sll_ifindex = ifindex;
        addr.sll_halen = ETH_ALEN;

        sendto(sock, packet.data(), packet.size(), 0, (struct sockaddr *)&addr, sizeof(addr));
        std::cout << "Message sent via Loopback L2.\n";
    }
    close(sock);
    return 0;
}