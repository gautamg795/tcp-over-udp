#include "Packet.h"

#include <cassert>                      // TODO: delete me
#include <algorithm>                    // for max
#include <cerrno>                       // for errno
#include <chrono>                       // for microseconds
#include <cstdint>                      // for uint32_t
#include <cstring>                      // for strerror
#include <fstream>                      // for ofstream
#include <iostream>                     // for cout, cerr, etc
#include <unordered_map>                // for unordered_map

#include <netdb.h>                      // for addrinfo, getaddrinfo, etc
#include <netinet/in.h>                 // for IPPROTO_UDP
#include <sys/socket.h>                 // for bind, recv, send, etc
#include <sys/time.h>                   // for timeval
#include <unistd.h>                     // for close

/*
 * Static Variables
 */
// how long to wait on rcv by default
static timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 500000 };
// how long to wait after sending FIN-ACK for final ACK
static timeval close_timeout = { .tv_sec = 0, .tv_usec = 750000 };

/*
 * Function Declarations
 */
bool establish_connection(int sockfd, uint32_t& ack_out, uint32_t& seq_out);
bool receive_file(int sockfd, uint32_t ack, uint32_t seq);
bool close_connection(int sockfd, uint32_t ack, uint32_t seq);

/*
 * Implementations
 */
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

    // Make the socket and bind it as usual
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
    // Set the initial 500ms timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout,
               sizeof(rcv_timeout));
    // "Connect" -- on a UDP socket, this just sets the default parameters for
    // send and receive (UDP doesn't actually have connections)
    connect(sockfd, ptr->ai_addr, ptr->ai_addrlen);
    freeaddrinfo(res);
    // Clean up memory
    uint32_t ack, seq;
    // Establish connection (handshake) then receive the file if that succeeded
    // ack and seq are passed between the two functions so they know where the
    // previous function left off
    establish_connection(sockfd, ack, seq) && receive_file(sockfd, ack, seq);
    close(sockfd);
}

/**
 * @param sockfd the socket to send/receive on
 * @param ref ack_out is set to the acknowledgment number after handshake
 * @param ref seq_out is set to the sequence number after handshake
 *
 * @return true on success, false otherwise
 */
bool establish_connection(int sockfd, uint32_t& ack_out, uint32_t& seq_out)
{
    Packet out;
    Packet in;
    out.headers.syn = true;
    // Generate the initial sequence number randomly
    out.headers.seq_number = get_isn();
    out.headers.window_sz = Packet::SEQ_MAX;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    // Set the timeout appropriately
    while (true)
    {
        // Send the initial SYN packet
        if (send(sockfd, (void*)&out, out.HEADER_SZ, 0) < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            return false;
        }
        // Try to receive a response
        int ret = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (ret < 0)
        {
            // recv returns EAGAIN if we timed out
            if (errno == EAGAIN)
            {
                continue;
            }
            // ICMP message meaning server doesn't exist -- but not guaranteed
            // we can't count on ICMP being right, so we just try again
            else if (errno == ECONNREFUSED)
            {
                continue;
            }
            // Some other error
            std::cerr << "recvfrom(): " << std::strerror(errno) << std::endl;
            return false;
        }
        // We expect a SYN-ACK back, where the ack number is our seq + 1
        if (!in.headers.syn || !in.headers.ack ||
                in.headers.ack_number != add_seq(out.headers.seq_number, 1))
        {
            continue;
        }
        break;
    }
    // Prepare the next outbound ACK and send it
    out.clear();
    out.headers.ack = true;
    out.headers.seq_number = in.headers.ack_number;
    out.headers.window_sz = Packet::SEQ_MAX;
    seq_out = add_seq(in.headers.ack_number, 1);
    ack_out = out.headers.ack_number = add_seq(in.headers.seq_number, 1);
    send(sockfd, (void*)&out, out.HEADER_SZ, 0);
    return true;
}

/**
 * @param sockfd the socket to send/receive on
 * @param ack the client's current acknowledgment number
 * @param seq the client's current sequence number
 *
 * @return true on success, false otherwise
 */
bool receive_file(int sockfd, uint32_t ack, uint32_t seq)
{
    // I switch to std::chrono times here rather than timeval because it's
    // friendlier for doing comparisons and math
    std::chrono::milliseconds timeout(500);
    auto send_time = now(); // now() is a function returning the current time
    timeval cur_timeout = { .tv_sec = 0, .tv_usec = 0 };
    // Open a fstream for the output file
    std::ofstream outfile("received.file", std::ofstream::binary);
    // Used to cache out of order packets by sequence number
    std::unordered_map<uint32_t, Packet> packet_cache;
    Packet out;
    Packet in;
    // A bit hacky; on the first iteration we just receive right away so this
    // bool helps handle that
    bool first = true;
    bool retransmit = false;
    while (true)
    {
        if (!first)
        {
            // Send the acknowledgment for the last received packet
            out.headers.ack = true;
            out.headers.ack_number = ack;
            out.headers.window_sz = Packet::SEQ_MAX;
            std::cout << "Sending ACK packet " << std::setw(7)
                      << ack << (retransmit ? " Retransmission" : "") 
                      << std::endl;
            send_time = now();
            send(sockfd, (void*)&out, out.HEADER_SZ, 0);
        }
        else
            first = false;
        // The timeout is 500ms - (current time - send time)
        // i.e., 500ms - (time already elapsed since we sent the packet)
        // using std::chrono allows us to do subtraction like this, then we
        // store the result back in a timeval for setsockopt to use
        cur_timeout = to_timeval(timeout - (now() - send_time));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &cur_timeout, sizeof(cur_timeout));
        // Receive a packet
        in.clear();
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0)
        {
            if (errno == EAGAIN)
            {
                retransmit = true;
                continue;
            }
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        // If we get a FIN packet, get ready to close the connection
        if (in.headers.fin)
        {
            return close_connection(sockfd, add_seq(in.headers.seq_number, 1), seq);
        }
        std::cout << "Received data packet " << std::setw(5)
                  << in.headers.seq_number << std::endl;
        // Is this the packet we expected?
        if (in.headers.seq_number != ack)
        {
            retransmit = true;
            // No, so check if the packet is a duplicate from earlier, or from
            // the future

            // If the packet definitely isn't part of the window (is a
            // duplicate), discard it
            // We know that the window size will never be more than 30720/2
            if ((ack < Packet::SEQ_MAX / 2 &&
                        in.headers.seq_number > add_seq(ack, Packet::SEQ_MAX / 2))
                    ||
                    (in.headers.seq_number < ack &&
                     (ack - in.headers.seq_number) < Packet::SEQ_MAX))
            {
                continue;
            }
            // If it is in our window, cache the packet for later
            packet_cache.emplace(in.headers.seq_number, std::move(in));
            continue;
        }
        else
        {
            // If it was the expected in-order packet, write its data to the
            // fstream
            outfile.write(in.data, in.headers.data_len);
            ack = add_seq(ack, in.headers.data_len);
            // Now, check to see if our cache contains the next in-order packet
            decltype(packet_cache)::iterator it;
            // While we can find the next packet in our cache, write it to the
            // file
            while ((it = packet_cache.find(ack)) != packet_cache.end())
            {
                in = std::move(it->second);
                outfile.write(in.data, in.headers.data_len);
                ack = add_seq(ack, in.headers.data_len);
                packet_cache.erase(it);
            }
            retransmit = false;
        }
    }
    return true;
}

/**
 * @param sockfd the socket to send/receive on
 * @param ack the client's current acknowledgment number
 * @param seq the client's current sequence number
 *
 * @return true on success, false otherwise
 */
bool close_connection(int sockfd, uint32_t ack, uint32_t seq)
{
    Packet in, out;
    out.headers.fin = out.headers.ack = true;
    out.headers.ack_number = ack;
    out.headers.seq_number = seq;
    out.headers.window_sz = Packet::SEQ_MAX;
    while (true)
    {
        // Write the FIN-ACK
        if (send(sockfd, (void*)&out, out.HEADER_SZ, 0) < 0)
        {
            std::cerr << "send(): " << std::strerror(errno) << std::endl;
            return false;
        }
        // Wait up to close_timeout seconds for an ACK from the server
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &close_timeout, sizeof(close_timeout));
        ssize_t bytes_read = recv(sockfd, (void*)&in, sizeof(in), 0);
        if (bytes_read < 0)
        {
            if (errno == EAGAIN)
            {
                // Timeout is okay--the server's ACK probably didn't make it
                return true;
            }
            std::cerr << "recv(): " << std::strerror(errno) << std::endl;
            return false;
        }
        else if (!in.headers.ack || in.headers.ack_number != add_seq(seq, 1))
        {
            continue;
        }
        return true;
    }
}
