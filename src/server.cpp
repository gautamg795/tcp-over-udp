#include "Packet.h"                     // for Packet, add_seq, get_isn, etc

#include <algorithm>                    // for max, find_if
#include <cassert>                      // TODO: delete me
#include <cerrno>                       // for errno
#include <chrono>                       // for microseconds, duration
#include <csignal>                      // for sigaction
#include <cstddef>                      // for size_t
#include <cstdint>                      // for uint32_t
#include <cstring>                      // for strerror
#include <fstream>                      // for ifstream
#include <iostream>                     // for operator<<, basic_ostream, etc
#include <iterator>                     // for next
#include <list>                         // for list
#include <stdexcept>                    // for runtime_error

#include <netdb.h>                      // for addrinfo, gai_strerror, etc
#include <netinet/in.h>                 // for IPPROTO_UDP
#include <sys/socket.h>                 // for bind, recv, send, etc
#include <sys/time.h>                   // for timeval
#include <unistd.h>                     // for close, ssize_t

/*
 * Static Variables
 */
static timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 500000 };
static timeval close_timeout = { .tv_sec = 0, .tv_usec = 750000 };
static timeval no_timeout  = { .tv_sec = 0, .tv_usec = 0 };
static bool keep_running = true;

/*
 * Function Declarations
 */
bool establish_connection(int sockfd, uint32_t& seq_out);
bool send_file(int sockfd, const char* filename, uint32_t seq);
bool close_connection(int sockfd, uint32_t seq);

enum class Mode {
    SS, // slow start
    CA, // congestion avoidance
    FR  // fast recovery
};

/*
 * Implementations
 */
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

    // Make the socket and bind
    addrinfo hints, *res;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    int ret = getaddrinfo(nullptr, port, &hints, &res);
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
    // We make a 'reset' sockaddr used to reset the socket's connection state
    // Remember that UDP sockets can't actually connect to anything, but calling
    // connect() on a socket sets its defaults so that send/receive don't need
    // to be given a target every time. We need to reset those defaults between
    // connections, and connecting to AF_UNSPEC does that
    sockaddr reset;
    reset.sa_family = AF_UNSPEC;
    // Set up signal handler to end the run loop
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) { keep_running = false; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    while (keep_running)
    {
        connect(sockfd, &reset, sizeof(reset));
        uint32_t seq = 0;
        establish_connection(sockfd, seq) && send_file(sockfd, filename, seq);
    }
    close(sockfd);
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
        if (errno == EINTR)
        {
            return false;
        }
        std::cerr << "recvfrom(): " << std::strerror(errno) << std::endl;
        return false;
    }
    else if (!in.headers.syn)
    {
        return false;
    }
    else if (bytes_read < (ssize_t)Packet::HEADER_SZ)
    {
        return false;
    }
    if (connect(sockfd, client, client_len) < 0)
    {
        std::cerr << "connect(): " << std::strerror(errno) << std::endl;
        return false;
    }
    Packet out;
    out.headers.ack = out.headers.syn = true;
    out.headers.ack_number = add_seq(in.headers.seq_number, 1);
    out.headers.seq_number = get_isn();
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
            continue;
        }
        if (!in.headers.ack ||
                in.headers.ack_number != add_seq(out.headers.seq_number, 1))
        {
            continue;
        }
        seq_out = in.headers.ack_number;
        break;
    }
    return true;
}

bool send_file(int sockfd, const char* filename, uint32_t seq)
{
    Mode current_mode = Mode::SS;
    std::chrono::milliseconds timeout(500);
    std::ifstream infile(filename, std::ifstream::binary);
    if (!infile)
    {
        throw std::runtime_error("invalid file");
    }
    uint32_t cwnd = 1024;
    uint32_t cwnd_used = 0;
    uint32_t ssthresh = 30720;
    uint32_t duplicate_acks = 0;
    std::list<PacketWrapper> window;
    timeval cur_timeout = { .tv_sec = 0, .tv_usec = 0 };
    uint32_t last_seq = seq;
    while (true)
    {
        while (cwnd_used < cwnd && infile)
        {
            Packet p;
            p.headers.seq_number = add_seq(seq, infile.tellg());
            infile.read(p.data, std::min((size_t)Packet::DATA_SZ,
                                         (size_t)(cwnd - cwnd_used)));
            p.headers.data_len = (ssize_t)infile.gcount();
            if (p.headers.data_len == 0)
            {
                break;
            }
            window.emplace_back(std::move(p));
            cwnd_used += p.headers.data_len;
        }
        if (window.empty())
        {
            assert(cwnd_used == 0); // TODO: delete me
            return close_connection(sockfd, last_seq);
        }
        for (auto& p : window)
        {
            if (p.sent)
            {
                if (now() - p.send_time > timeout)
                {
                    p.sent = false;
                    p.retransmit = true;
                }
                else
                {
                    continue;
                }
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
            std::cout << "Sending data packet " << std::setw(6)
                      << p.packet.headers.seq_number << ' ' << std::setw(5)
                      << cwnd << ' ' << std::setw(5) << ssthresh
                      << (p.retransmit ? " Retransmission" : "") << std::endl;
        }
        cur_timeout = to_timeval(timeout - (now() - window.front().send_time));
        Packet in;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &cur_timeout, sizeof(cur_timeout)) < 0)
        {
            std::cerr << "setsockopt(): " << std::strerror(errno) << std::endl;
        }
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0 && errno != EAGAIN)
        {
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (bytes_read < 0)
        {
            window.front().sent = false;
            window.front().retransmit = true;
            ssthresh = cwnd / 2;
            cwnd = Packet::DATA_SZ;
            current_mode = Mode::SS;
            continue;
        }
        std::cout << "Receiving ack packet " << std::setw(5)
                  << in.headers.ack_number << std::endl;
        auto it = std::find_if(window.begin(), window.end(),
                [&in](const PacketWrapper& elem) -> bool {
                    return add_seq(elem.packet.headers.seq_number,
                            elem.packet.headers.data_len) ==
                            in.headers.ack_number;
                });
        if (it == window.end())
        {
            if (current_mode == Mode::FR)
            {
                cwnd += Packet::DATA_SZ;
                window.front().sent = false;
                window.front().retransmit = true;
            }
            else if (++duplicate_acks == 3)
            {
                duplicate_acks = 0;
                window.front().sent = false;
                window.front().retransmit = true;
                ssthresh = cwnd / 2;
                cwnd = ssthresh + 3 * Packet::DATA_SZ;
                current_mode = Mode::FR;
            }
            else if (current_mode == Mode::SS)
            {
                cwnd += Packet::DATA_SZ;
            }
            else if (current_mode == Mode::CA)
            {
                cwnd += std::max(1,
                        (int)std::round(Packet::DATA_SZ * (double)Packet::DATA_SZ / cwnd));
            }
            cwnd = std::min((uint32_t)Packet::SEQ_MAX / 2, cwnd);
            cwnd = std::min((uint32_t)in.headers.window_sz, cwnd);
            continue;
        }
        last_seq = in.headers.ack_number;
        switch(current_mode)
        {
            case Mode::SS:
            {
                cwnd += Packet::DATA_SZ;
                break;
            }
            case Mode::CA:
            {
                cwnd += std::max(1,
                        (int)std::round(Packet::DATA_SZ * (double)Packet::DATA_SZ / cwnd));
                break;
            }
            case Mode::FR:
            {
                cwnd = ssthresh;
                duplicate_acks = 0;
                current_mode = Mode::CA;
                break;
            }
        }
        cwnd = std::min((uint32_t)Packet::SEQ_MAX / 2, cwnd);
        cwnd = std::min((uint32_t)in.headers.window_sz, cwnd);
        if (cwnd >= ssthresh)
        {
            current_mode = Mode::CA;
        }
        duplicate_acks = 0;
        for (auto begin = window.begin(), end = std::next(it); begin != end; )
        {
            cwnd_used -= begin->packet.headers.data_len;
            begin = window.erase(begin);
        }
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
            continue;
        }
        else if (!in.headers.ack || !in.headers.fin || in.headers.ack_number != add_seq(seq, 1))
        {
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
        if (bytes_read < 0)
        {
            if (errno == EAGAIN || errno == ECONNREFUSED) // Client got ack and went away
            {
                return true;
            }
            else
            {
                std::cerr << "recv(): " << std::strerror(errno) << std::endl;
                return false;
            }
        }
        if (in.headers.fin && in.headers.ack)
        {
            continue;
        }
        return true;
    }
    return true;
}
