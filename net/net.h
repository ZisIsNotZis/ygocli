#ifndef YGOCLI_NET_H
#define YGOCLI_NET_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// gframe-compatible TCP framing. Every packet on the wire:
//   uint16_t packet_len   (little-endian; includes the 1-byte proto, NOT itself)
//   uint8_t  proto        (CTOS_* / STOC_* / MSG_* as appropriate)
//   uint8_t  payload[packet_len - 1]
namespace net {

// Returns a listening socket fd, or -1 on error.
int listen(const std::string& ip, uint16_t port);

// Accept a connection on a listening socket. Returns client fd or -1.
int accept(int listen_fd);

// Connect to a host:port. Returns connected fd or -1.
int connect(const std::string& host, uint16_t port);

// Read one framed packet into buf. Returns payload length (packet_len - 1)
// on success, 0 on clean EOF, -1 on error/timeout. On success buf[0] is the
// proto byte and the payload follows. timeout_ms <= 0 means block indefinitely.
int read_packet(int fd, std::vector<uint8_t>& buf, int timeout_ms = -1);

// Send one framed packet (proto byte + payload). Returns payload length on
// success, -1 on error.
int write_packet(int fd, uint8_t proto, const uint8_t* data, size_t len);

// Shortcut: send a packet with no payload.
int send_packet(int fd, uint8_t proto);

// Close a socket fd (no-op for -1).
void close(int fd);

} // namespace net

#endif // YGOCLI_NET_H
