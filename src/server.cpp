#include "Packet.h"                     // for Packet, add_seq, get_isn, etc

#include <netdb.h>                      // for addrinfo, gai_strerror, etc
#include <netinet/in.h>                 // for IPPROTO_UDP
#include <sys/socket.h>                 // for bind, recvfrom, sendto, etc
#include <unistd.h>                     // for close, ssize_t
#include <cerrno>                       // for errno
#include <cstring>                      // for strerror
#include <iostream>                     // for operator<<, basic_ostream, etc

void start_connection(sockaddr client, socklen_t client_len);

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cout << "Usage: " << argv[0] << " port-number file-name\n";
        return 1;
    }
    char* port = argv[1];
    /* char* filename = argv[2]; */
    int sockfd = -1;

    addrinfo hints, *res;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    int ret = getaddrinfo("0.0.0.0", port, &hints, &res);
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
        if (bind(sockfd, res->ai_addr, res->ai_addrlen) < 0)
        {
            close(sockfd);
            std::cerr << "bind(): " << std::strerror(errno) << std::endl;
            continue;
        }
        break;
    }
    if (ptr == nullptr)
    {
        std::cerr << "Failed to bind to any addresses\n";
        return 1;
    }
    while (true)
    {
        sockaddr_storage client; 
        socklen_t client_len;
        Packet in;
        ssize_t bytes_read = recvfrom(sockfd, (void*)&in, sizeof(in), 0,
                (sockaddr*)&client, &client_len);
        std::cout << "got " << bytes_read << " bytes\n"
            << in << std::endl;
        if (!in.syn)
            continue;
        Packet out;
        out.syn = out.ack = true;
        out.seq_number= get_isn();
        out.ack_number = add_seq(in.seq_number, 1);
        sendto(sockfd, (void*)&out, out.HEADER_SZ + out.data_len, 0, (sockaddr*)&client, client_len);
    }
}
