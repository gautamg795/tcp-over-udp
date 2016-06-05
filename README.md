# TCP-like Transport Protocol over UDP
### CS118 Project 2 Spring '16
##### Gautam Gupta, Kelly Hosokawa, and David Pu

# Design/Architecture

## Packet

Packets were designed as a struct, `Packet`.  `Packet` has an embedded struct, `headers`, which contains all of the header info, including the ack number, sequence number, and bit fields for the `ack`, `syn`, and `fin` flags.

There is an additional struct, `PacketWrapper`, which helps the server keep track of additional details such as when the packet was sent, whether or not they were sent, and whether or not they were retransmitted.

There are five additional methods:
* `operator<<()`: Takes in an std::ostream os and a Packet& p, and writes the packet to the ostream.
* `get_isn()`: Generates a random sequence number in the range [0, SEQ_MAX].
* `add_seq()`: Adds the two parameters, and takes the modulo of the sum to ensure that the resulting sequence number is in the range [0, SEQ_MAX].
* `now()`: Returns the current time.
* `to_timeval()`: Takes in a std::chrono duration and converts it to a timeval.


## Client

The client takes in the `hostname` and `port number` from the command line.  We use `getaddrinfo()` to create and bind to the appropriate UDP socket.  At this point, we also set the initial timeout to 500ms.  In this case, since UDP is connectionless, `connect()` simply sets the default parameters for `send()` and `receive()`.

Upon successfully opening and binding to a socket, `establish_connection()` performs the handshake and receives the file if the connection was successfully established.

`establish_connection()` has three parameters: the socket to send/receive on and two unitialized `uint32_t` values - `ack_out` and `seq_out`.  We randomly generate the initial sequence number and use `setsockopt()` to set the timeout value.  We use `send()` to send the initial SYN packet and then use `recv()` to receive responses until we get the corresponding SYN-ACK.  Upon successfully receiving the SYN-ACK, we prepare and send the last ACK (the last part of the three-way handshake), and initialize `ack_out` and `seq_out` with their respective values after the handshake.

If `establish_connection()` is successful, we call `receive_file()` with three parameters: the socket and the ack/seq numbers that were initialized at the end of `establish_connection()`.  We use an `std::unordered_map packet_cache` to cache out-of-order packets and their sequence numbers.  We set the timeout value appropriately and then call `recv()` to get the next packet.  If its sequence number indicates that it was not the packet that we were expecting, we check to see if the packet is part of the current window.  If it is, we discard it.  Otherwise, we add the packet to `packet_cache`.  If the packet is the one that we were expecting, we write its data to the fstream.  We then iterate over `packet_cache` and write as many subsequent packets as we can to the file.  After each packet, we send an ack for the last received packet.  We then loop to get the next packet.  If at any time we get a FIN packet, we call `close_connection()` with the socket and the client's current `ack` and `seq` numbers.

In `close_connection()`, we prepare a packet with the client's current ack and seq numbers.  We send the FIN-ACK and wait up to `close_timeout` seconds for the corresponding ACK.

