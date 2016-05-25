#ifndef PACKET_H
#define PACKET_H

#include <algorithm>                    // for uniform_int_distribution, move
#include <chrono>                       // for high_resolution_clock
#include <cstddef>                      // for size_t
#include <cstdint>                      // for uint32_t
#include <cstring>                      // for memset
#include <iomanip>                      // for setw
#include <iostream>                     // for operator<<, basic_ostream, etc
#include <random>                       // for mt19937, random_device, etc

struct Packet
{
    struct {
    bool ack : 1;
    bool syn : 1;
    bool fin : 1;
    uint16_t ack_number;
    uint16_t seq_number;
    uint16_t data_len;
    } headers;
    static const size_t MSS       = 1024;
    static const size_t HEADER_SZ = sizeof(headers);
    static const size_t DATA_SZ   = MSS - HEADER_SZ;
    static const size_t SEQ_MAX   = 30720;
    Packet()
    {
        static_assert(sizeof(Packet) == HEADER_SZ + DATA_SZ,
                "Incorrect packet size");
        static_assert(sizeof(Packet) <= 1024,
                "Packet larger than 1024 bytes");
        clear();
    }
    Packet(const Packet&) = delete; // Copying a Packet will be slow
    Packet& operator=(const Packet&) = delete;
    Packet(Packet&&) = default; // Moving a Packet will be fast!
    Packet& operator=(Packet&&) = default;
    void clear() { std::memset(&headers, 0, HEADER_SZ); }
    char data[DATA_SZ];
};

/**
 * structure used by server to keep track of sent packets, what time they were
 * sent, if they were retransmitted, etc
 */
struct PacketWrapper
{
    // 'using x = y' is like 'typedef y x' and gives us the shorthand time_point
    // to represent the type returned by the now() function
    using time_point = decltype(std::chrono::high_resolution_clock::now());
    PacketWrapper(Packet&& p) :
        packet(std::move(p)), sent(false), retransmit(false) {}
    Packet packet;
    time_point send_time;
    bool sent;
    bool retransmit;
};

/*
 * Inline Implementations
 */

/**
 * ostream operator for outputting a packet
 */
inline
std::ostream& operator<<(std::ostream& os, const Packet& p)
{
    os << "ack: " << p.headers.ack << "|fin: " << p.headers.fin << "|syn: "
       << p.headers.syn << "|ack_number: " << std::setw(5) << p.headers.ack_number
       << "|seq_number: " << std::setw(5) << p.headers.seq_number << "|data_len: "
       << p.headers.data_len;
    return os;
}

/**
 * Generates a random sequence number in range [0, SEQ_MAX]
 */
inline
uint32_t get_isn()
{
    // Create the random devices and generators--static so they are only
    // initialized once. These are more random than C-style rand()
    static std::random_device rd;
    static std::mt19937 rndgen(rd());
    static std::uniform_int_distribution<> dist(0, Packet::SEQ_MAX);
    // Return a value from the uniform distribution
    return dist(rndgen);
}

/**
 * Handles modulo addition to the sequence number
 */
inline
uint32_t add_seq(uint32_t base, uint32_t add)
{
    return (base + add) % Packet::SEQ_MAX;
}

/**
 * Returns the current time
 */
inline
PacketWrapper::time_point now()
{
    return std::chrono::high_resolution_clock::now();
}

/**
 * Converts a std::chrono duration to a timeval
 */
template <typename Duration>
timeval to_timeval(Duration&& d)
{
    using namespace std::chrono;
    seconds sec = duration_cast<seconds>(d);
    if (sec.count() < 0ll)
    {
        sec = sec.zero();
    }
    return
    {
        .tv_sec = sec.count(),
        .tv_usec = (int)duration_cast<microseconds>(d-sec).count()
    };
}

#endif
