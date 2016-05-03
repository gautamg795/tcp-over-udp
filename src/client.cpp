#include "Packet.h"
#include <cstring>
#include <string>
#include <thread>
#include <iostream>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cout << "Usage: " << argv[0] << " server-host port\n";
        return 1;
    }
    char* hostname = argv[1];
    char* port = argv[2];
    int sockfd = -1;

    addrinfo hints, *res;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    int ret = getaddrinfo(hostname, port, &hints, &res);
    if (ret < 0)
    {
        std::cerr << "getaddrinfo(): " << gai_strerror(ret) << std::endl;
        return 1;
    }
    auto ptr = res;
    for (; ptr != nullptr; ptr = ptr->ai_next)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0)
        {
            std::cerr << "socket(): " << std::strerror(errno) << std::endl;
            continue;
        }
        
        break;
    }
    if (ptr == nullptr)
    {
        std::cerr << "Could not open a socket\n";
        return 1;
    }
    sockaddr* dest = ptr->ai_addr;
    socklen_t dest_len = ptr->ai_addrlen;
    Packet out;
    out.syn = true;
    out.seq_number = get_isn();
    sendto(sockfd, (void*)&out, out.HEADER_SZ + out.data_len, 0, dest, dest_len);
    Packet in;
    ssize_t bytes_read = recvfrom(sockfd, (void*)&in, sizeof(in), 0, dest, &dest_len);
    std::cout << "got " << bytes_read << " bytes\n"
        << in << std::endl;
}
