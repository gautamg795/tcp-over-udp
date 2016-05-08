#include "Packet.h"
#include <fstream>
#include <cstring>
#include <string>
#include <thread>
#include <iostream>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <unistd.h>

static timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 500000 };
static timeval no_timeout  = { .tv_sec = 0, .tv_usec = 0 };
bool establish_connection(int sockfd, uint32_t& ack_out, uint32_t& seq_out);
bool receive_file(int sockfd, uint32_t ack, uint32_t seq);
bool close_connection(int sockfd, uint32_t ack, uint32_t seq);

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
        sockfd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
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
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout,
               sizeof(rcv_timeout));
    connect(sockfd, ptr->ai_addr, ptr->ai_addrlen);
    freeaddrinfo(res);
    uint32_t ack, seq;
    establish_connection(sockfd, ack, seq);
    receive_file(sockfd, ack, seq);
    close(sockfd);
}

bool establish_connection(int sockfd, uint32_t& ack_out, uint32_t& seq_out)
{
    Packet out;
    Packet in;
    out.headers.syn = true;
    out.headers.seq_number = get_isn();
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    while (true)
    {
        if (send(sockfd, (void*)&out, out.HEADER_SZ, 0) < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            return false;
        }
        int ret = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (ret < 0 && errno != EAGAIN)
        {
            std::cerr << "recvfrom(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (ret < 0)
        {
            std::cerr << "timeout\n";
            continue;
        }
        if (!in.headers.syn || !in.headers.ack || 
                in.headers.ack_number != add_seq(out.headers.seq_number, 1))
        {
            std::cerr << "Unexpected packet received\n";
            continue;
        }
        std::cerr << "rcv: " << in << std::endl;
        break;
    }
    out.clear();
    out.headers.ack = true;
    out.headers.seq_number = in.headers.ack_number;
    seq_out = add_seq(in.headers.ack_number, 1);
    ack_out = out.headers.ack_number = add_seq(in.headers.seq_number, 1);
    std::cerr << "snd: " << out << std::endl;
    send(sockfd, (void*)&out, out.HEADER_SZ, 0);
    std::cerr << "handshake complete\n";
    return true;
}

bool receive_file(int sockfd, uint32_t ack, uint32_t seq)
{
    std::chrono::milliseconds timeout(500);
    auto send_time = now();
    timeval cur_timeout = { .tv_sec = 0, .tv_usec = 0 };
    std::ofstream outfile("outfile", std::ofstream::binary);
    Packet out;
    Packet in;
    bool first = true;
    while (true)
    {
        if (!first)
        {
            out.headers.ack = true;
            out.headers.ack_number = ack;
            std::cerr << "Sending ACK packet " << ack << std::endl;
            send_time = now();
            send(sockfd, (void*)&out, out.HEADER_SZ, 0);
        }
        else
            first = false;
        cur_timeout.tv_usec = std::max(
                (long)std::chrono::duration_cast<std::chrono::microseconds>(
                    timeout - (now() - send_time)).count(), 0l);
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &cur_timeout, sizeof(cur_timeout));
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (bytes_read < 0)
        {
            std::cerr << "timeout\n";
            continue;
        }
        if (in.headers.fin)
        {
            return close_connection(sockfd, add_seq(in.headers.seq_number, 1), seq);
        }
        std::cout << "Received data packet " << in.headers.seq_number << std::endl;
        std::cout << in << std::endl;
        if (in.headers.seq_number != ack)
        {
            continue;
        }
        outfile.write(in.data, in.headers.data_len);
        ack = add_seq(ack, in.headers.data_len);
    }
    return true;
}

bool close_connection(int sockfd, uint32_t ack, uint32_t seq)
{
    return true;
/*    Packet in, out;
    out.headers.fin = out.headers.ack = true;
    out.headers.ack_number = ack;
    out.headers.seq_number = seq;
    while (true)
    {
        if (send(sockfd, (void*)&out, out.HEADER_SZ, 0) < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            return false;
        }
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (bytes_read < 0)
        {
            std::cerr << "timeout\n";
            continue;
        }
        else if (!in.headers.ack || in.headers.ack_number != add_seq(seq, 1))
        {
            std::cerr << "Unexpected ack\n";
            continue;
        }
        return true;
    }
*/
}
