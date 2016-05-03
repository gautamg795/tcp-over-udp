#ifndef PACKET_H
#define PACKET_H

#include <cstdint>                      // for uint32_t
#include <cstring>                      // for memset
#include <iostream>                     // for operator<<, basic_ostream, etc
#include <random>                       // for mt19937, random_device, etc
#include <cstddef>                      // for size_t

struct Packet
{
    static const size_t HEADER_SZ = 16;
    static const size_t DATA_SZ = 1008;
    static const size_t SEQ_MAX = 30720;
    Packet() 
    { 
        static_assert(sizeof(Packet) == HEADER_SZ + DATA_SZ,
                "Incorrect packet size");
        static_assert(sizeof(Packet) <= 1024,
                "Packet larger than 1024 bytes");
        clear();
    }
    void clear() { std::memset(this, 0, sizeof(*this)); }
    bool ack : 1;
    bool syn : 1;
    bool fin : 1;
    uint32_t ack_number;
    uint32_t seq_number;
    uint32_t data_len;
    char data[DATA_SZ];
};

inline
std::ostream& operator<<(std::ostream& os, const Packet& p)
{
    os << "ack: " << p.ack << "|fin: " << p.fin << "|syn: " << p.syn
       << "|ack_number: " << p.ack_number << "|seq_number: " << p.seq_number
       << "|data_len: " << p.data_len << "|data: " << p.data << std::endl;
    return os;
}

inline
uint32_t get_isn()
{
    static std::random_device rd;
    static std::mt19937 rndgen(rd());
    static std::uniform_int_distribution<> dist(0, Packet::SEQ_MAX);
    return dist(rndgen);
}

inline
uint32_t add_seq(uint32_t base, uint32_t add)
{
    return (base + add) % Packet::SEQ_MAX;
}

#endif
