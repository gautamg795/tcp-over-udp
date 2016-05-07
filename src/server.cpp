#include "Packet.h"                     // for Packet, add_seq, get_isn, etc

#include <algorithm>
#include <exception>
#include <cerrno>                       // for errno
#include <deque>
#include <sys/time.h>                   // for timeval
#include <cstring>                      // for strerror
#include <fstream>                      // for ifstream
#include <iostream>                     // for operator<<, basic_ostream, etc
#include <netdb.h>                      // for addrinfo, gai_strerror, etc
#include <netinet/in.h>                 // for IPPROTO_UDP
#include <sys/socket.h>                 // for bind, recv, send, etc
#include <unistd.h>                     // for close, ssize_t

bool establish_connection(int sockfd, uint32_t& seq_out);
bool send_file(int sockfd, const char* filename, uint32_t seq);
bool close_connection(int sockfd, uint32_t seq);
static timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 500000 };
static timeval close_timeout = { .tv_sec = 1, .tv_usec = 0 };
static timeval no_timeout  = { .tv_sec = 0, .tv_usec = 0 };

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cout << "Usage: " << argv[0] << " port-number file-name\n";
        return 1;
    }
    char* port = argv[1];
    char* filename = argv[2];
    int sockfd = -1;

    addrinfo hints, *res;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    int ret = getaddrinfo("localhost", port, &hints, &res);
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
        if (bind(sockfd, ptr->ai_addr, ptr->ai_addrlen) < 0)
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
    freeaddrinfo(res);
    bool run = true;
    struct sockaddr reset;
    reset.sa_family = AF_UNSPEC;
    while (run)
    {
        /* std::ifstream infile(filename); */
        connect(sockfd, &reset, sizeof(reset));
        uint32_t seq = 0;
        run = establish_connection(sockfd, seq) && send_file(sockfd, filename, seq);
        if (run)
            std::cout << "done sending file\n";
    }
    std::cout << "quitting\n";
}

bool establish_connection(int sockfd, uint32_t& seq_out)
{
    sockaddr_storage client_storage;
    sockaddr* client = (sockaddr*)&client_storage;
    socklen_t client_len = sizeof(client_storage);
    Packet in;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));
    ssize_t bytes_read = recvfrom(sockfd, (void*)&in, sizeof(in), 0, client, &client_len);
    if (bytes_read < 0)
    {
        std::cerr << "recvfrom(): " << std::strerror(errno) << std::endl;
        return false;
    }
    else if (!in.headers.syn)
    {
        std::cerr << "Non-syn packet received\n";
        return false;
    }
    else if (bytes_read < (ssize_t)Packet::HEADER_SZ)
    {
        std::cerr << "Incomplete packet received\n";
        return false;
    }
    if (connect(sockfd, client, client_len) < 0)
    {
        std::cerr << "connect(): " << std::strerror(errno) << std::endl;
        return false;
    }
    std::cerr << "rcv: " << in << std::endl;
    Packet out;
    out.headers.ack = out.headers.syn = true;
    out.headers.ack_number = add_seq(in.headers.seq_number, 1);
    out.headers.seq_number = get_isn();
    std::cerr << "snd: " << out << std::endl;
    while (true)
    {
        int ret = send(sockfd, (void*)&out, out.HEADER_SZ, 0);
        if (ret < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            continue;
        }
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout,
                sizeof(rcv_timeout));
        bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        if (bytes_read < 0)
        {
            std::cerr << "timeout\n";
            continue;
        }
        if (!in.headers.ack ||
                in.headers.ack_number != add_seq(out.headers.seq_number, 1))
        {
            continue;
        }
        std::cerr << "rcv: " << in << std::endl;
        seq_out = in.headers.ack_number;
        break;
    }
    std::cerr << "handshake complete\n";
    return true;
}

bool send_file(int sockfd, const char* filename, uint32_t seq)
{
    std::chrono::milliseconds timeout(500);
    std::ifstream infile(filename, std::ifstream::binary);
    if (!infile)
    {
        throw std::runtime_error("invalid file");
    }
    unsigned int cwnd = 1;
    unsigned int ssthresh = 1;
    std::deque<PacketWrapper> window;
    timeval cur_timeout = { .tv_sec = 0, .tv_usec = 0 };
    uint32_t last_seq = seq;
    while (true)
    {
        if (infile)
        {
            if (window.size() < cwnd)
            {
                for (size_t i = window.size(); i < cwnd && infile; i++)
                {
                    Packet p;            
                    ssize_t initial = infile.tellg();
                    p.headers.seq_number = add_seq(seq, infile.tellg());
                    infile.read(p.data, p.DATA_SZ);
                    if (infile.eof())
                    {
                        infile.clear();
                        p.headers.data_len = (ssize_t)infile.tellg() - initial;
                        infile.setstate(std::ifstream::eofbit | std::ifstream::failbit);
                    }
                    else
                    {
                        p.headers.data_len = (ssize_t)infile.tellg() - initial;
                    }
                    window.emplace_back(std::move(p));
                }
            }
        }
        else if (window.empty())
        {
            return close_connection(sockfd, last_seq);
        }
        for (auto& p : window)
        {
            if (p.sent)
            {
                continue;
            }
            int ret = send(sockfd, (void*)&p.packet, p.packet.HEADER_SZ +
                    p.packet.headers.data_len, 0);
            if (ret < 0)
            {
                std::cerr << "send(): " << std::strerror(errno) << std::endl;
                return false;
            }
            p.sent = true;
            p.send_time = now();
            std::cout << "Sending data packet " << p.packet.headers.seq_number
                      << ' ' << cwnd << ' ' << ssthresh << std::endl;
            std::cout << p.packet << std::endl;
        }
        if (window.empty()) continue;
        cur_timeout.tv_usec = std::max((long)std::chrono::duration_cast<std::chrono::microseconds>(
                    timeout - (now() - window.front().send_time)).count(), 0l);
        Packet in;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &cur_timeout, sizeof(cur_timeout)) < 0)
        {
            std::cerr << "setsockopt(): " << std::strerror(errno) << std::endl;
        }
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            continue;
        }
        else if (bytes_read < 0)
        {
            std::cerr << "timeout\n";
            window.front().sent = false;
            continue;
        }
        std::cout << "Receiving ack packet " << in.headers.ack_number << std::endl;
        std::cout << in << std::endl;
        auto it = std::find_if(window.begin(), window.end(),
                [&in](const PacketWrapper& elem) -> bool {
                    return add_seq(elem.packet.headers.seq_number,
                            elem.packet.headers.data_len) ==
                            in.headers.ack_number;
                });
        if (it == window.end())
        {
            std::cerr << "got non-matching ack??" << std::endl;
            continue;
        }
        last_seq = std::max(last_seq, (uint32_t)in.headers.ack_number);
        window.erase(window.begin(), std::next(it));
    }
}

bool close_connection(int sockfd, uint32_t seq)
{
    Packet in, out;
    out.headers.fin = true;
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
        else if (!in.headers.ack || !in.headers.fin || in.headers.ack_number != add_seq(seq, 1))
        {
            std::cerr << "Unexpected finack\n";
            continue;
        }
        break;
    }
    out.clear();
    seq = out.headers.seq_number = in.headers.ack_number;
    out.headers.ack = true;
    out.headers.ack_number = add_seq(in.headers.seq_number, 1);
    while (true)
    {
        if (send(sockfd, (void*)&out, out.HEADER_SZ, 0) < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            return false;
        }
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &close_timeout, sizeof(close_timeout));
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (bytes_read < 0)
        {
            return true;
        }
        continue;
    }
    return true;
}
