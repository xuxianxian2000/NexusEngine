#include "nexus/net/udp_transport.h"
#include "nexus/core/log.h"

#include <cstring>
#include <algorithm>
#include <arpa/inet.h>

namespace nexus::net {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Server mode
// ─────────────────────────────────────────────────────────────────────────────

bool UdpTransport::listen(u16 port) {
    if (listening_ || connected_) {
        NX_ERROR("UdpTransport::listen: already active");
        return false;
    }

    if (!socket_.open()) {
        NX_ERROR("UdpTransport::listen: failed to open socket");
        return false;
    }

    Address bind_addr("0.0.0.0", port);
    if (!socket_.bind(bind_addr)) {
        NX_ERROR("UdpTransport::listen: failed to bind to port {}", port);
        socket_.close();
        return false;
    }

    if (!socket_.set_non_blocking(true)) {
        NX_ERROR("UdpTransport::listen: failed to set non-blocking");
        socket_.close();
        return false;
    }

    listening_ = true;
    NX_INFO("UdpTransport: listening on port {}", port);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Client mode
// ─────────────────────────────────────────────────────────────────────────────

bool UdpTransport::connect(const std::string& host, u16 port) {
    if (listening_ || connected_) {
        NX_ERROR("UdpTransport::connect: already active");
        return false;
    }

    if (!socket_.open()) {
        NX_ERROR("UdpTransport::connect: failed to open socket");
        return false;
    }

    // Bind to any local port
    Address local_addr("0.0.0.0", 0);
    if (!socket_.bind(local_addr)) {
        NX_ERROR("UdpTransport::connect: failed to bind local socket");
        socket_.close();
        return false;
    }

    if (!socket_.set_non_blocking(true)) {
        NX_ERROR("UdpTransport::connect: failed to set non-blocking");
        socket_.close();
        return false;
    }

    // Create a peer entry for the server
    Address server_addr(host, port);
    u64 key = make_peer_key(server_addr);
    server_key_ = key;

    RemotePeer peer;
    peer.address = server_addr;
    peer.id = next_conn_id_++;
    peer.last_recv = std::chrono::steady_clock::now();
    peers_[key] = std::move(peer);

    // Send connection request
    send_protocol(server_addr, PacketType::ConnectionRequest, 0, 0, 0);

    NX_INFO("UdpTransport: connecting to {}:{}", host, port);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Close
// ─────────────────────────────────────────────────────────────────────────────

void UdpTransport::close() {
    if (!socket_.is_open()) {
        return;
    }

    // Send disconnect to all peers
    for (auto& [key, peer] : peers_) {
        send_protocol(peer.address, PacketType::Disconnect,
                      peer.local_sequence, peer.remote_sequence, peer.ack_bits);
    }

    peers_.clear();
    socket_.close();
    listening_ = false;
    connected_ = false;
    server_key_ = 0;
    next_conn_id_ = 1;

    NX_INFO("UdpTransport: closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Send
// ─────────────────────────────────────────────────────────────────────────────

bool UdpTransport::send(ConnectionId conn, const void* data, u32 size,
                         PacketFlag flags) {
    std::lock_guard<std::mutex> lock(mutex_);

    RemotePeer* peer = find_peer_by_id(conn);
    if (!peer) {
        NX_WARN("UdpTransport::send: unknown connection {}", conn);
        return false;
    }

    bool reliable = (flags & PacketFlag::Reliable);
    PacketType type = reliable ? PacketType::DataReliable : PacketType::Data;
    u32 seq = ++peer->local_sequence;

    send_protocol(peer->address, type, seq,
                  peer->remote_sequence, peer->ack_bits, data, size);

    if (reliable) {
        PendingPacket pending;
        pending.sequence = seq;
        pending.data.assign(static_cast<const u8*>(data),
                            static_cast<const u8*>(data) + size);
        pending.send_time = std::chrono::steady_clock::now();
        peer->pending_reliable.push_back(std::move(pending));
    }

    return true;
}

void UdpTransport::broadcast(const void* data, u32 size, PacketFlag flags) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, peer] : peers_) {
        bool reliable = (flags & PacketFlag::Reliable);
        PacketType type = reliable ? PacketType::DataReliable : PacketType::Data;
        u32 seq = ++peer.local_sequence;

        send_protocol(peer.address, type, seq,
                      peer.remote_sequence, peer.ack_bits, data, size);

        if (reliable) {
            PendingPacket pending;
            pending.sequence = seq;
            pending.data.assign(static_cast<const u8*>(data),
                                static_cast<const u8*>(data) + size);
            pending.send_time = std::chrono::steady_clock::now();
            peer.pending_reliable.push_back(std::move(pending));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll
// ─────────────────────────────────────────────────────────────────────────────

void UdpTransport::poll() {
    if (!socket_.is_open()) {
        return;
    }

    u8 buffer[MAX_PACKET_SIZE];

    for (u32 i = 0; i < MAX_RECV_PER_POLL; ++i) {
        Address sender;
        i32 bytes = socket_.recv_from(sender, buffer, MAX_PACKET_SIZE);
        if (bytes <= 0) {
            break;
        }
        process_packet(sender, buffer, static_cast<u32>(bytes));
    }

    check_timeouts();
    retransmit_reliable();
}

u32 UdpTransport::connection_count() const {
    return static_cast<u32>(peers_.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Packet Processing
// ─────────────────────────────────────────────────────────────────────────────

void UdpTransport::process_packet(const Address& from, const u8* data, u32 size) {
    ProtocolHeader header{};
    if (!read_header(data, size, header)) {
        return; // Too small or invalid
    }

    if (header.protocol_id != PROTOCOL_ID) {
        return; // Not our protocol
    }

    switch (header.type) {
        case PacketType::ConnectionRequest:
            handle_connection_request(from, header);
            break;
        case PacketType::ConnectionAccept:
            handle_connection_accept(header);
            break;
        case PacketType::Disconnect:
            handle_disconnect(from);
            break;
        case PacketType::Data:
        case PacketType::DataReliable: {
            u64 key = make_peer_key(from);
            RemotePeer* peer = find_peer(key);
            if (peer) {
                bool reliable = (header.type == PacketType::DataReliable);
                const u8* payload = data + HEADER_SIZE;
                u32 payload_size = size - HEADER_SIZE;
                handle_data(*peer, payload, payload_size, reliable, header);
            }
            break;
        }
        case PacketType::Ack: {
            u64 key = make_peer_key(from);
            RemotePeer* peer = find_peer(key);
            if (peer) {
                handle_ack(*peer, header);
            }
            break;
        }
        case PacketType::Ping:
            handle_ping(from, header);
            break;
        case PacketType::Pong: {
            u64 key = make_peer_key(from);
            RemotePeer* peer = find_peer(key);
            if (peer) {
                peer->last_recv = std::chrono::steady_clock::now();
            }
            break;
        }
        case PacketType::ConnectionDeny:
            NX_WARN("UdpTransport: connection denied by {}:{}", from.host, from.port);
            break;
    }
}

void UdpTransport::handle_connection_request(const Address& from,
                                              const ProtocolHeader& header) {
    if (!listening_) {
        // We're not a server; deny
        send_protocol(from, PacketType::ConnectionDeny, 0, 0, 0);
        return;
    }

    u64 key = make_peer_key(from);
    if (find_peer(key)) {
        // Already connected; resend accept (they may have missed it)
        RemotePeer* existing = find_peer(key);
        send_protocol(from, PacketType::ConnectionAccept,
                      0, existing->remote_sequence, existing->ack_bits);
        return;
    }

    // Accept the new connection
    RemotePeer peer;
    peer.address = from;
    peer.id = next_conn_id_++;
    peer.last_recv = std::chrono::steady_clock::now();
    ConnectionId id = peer.id;
    peers_[key] = std::move(peer);

    send_protocol(from, PacketType::ConnectionAccept, 0, 0, 0);

    NX_INFO("UdpTransport: new connection {} from {}:{}", id, from.host, from.port);

    if (on_connect_) {
        on_connect_(id);
    }

    (void)header;
}

void UdpTransport::handle_connection_accept(const ProtocolHeader& header) {
    if (connected_) {
        return; // Already connected
    }

    RemotePeer* peer = find_peer(server_key_);
    if (!peer) {
        return;
    }

    connected_ = true;
    peer->last_recv = std::chrono::steady_clock::now();

    NX_INFO("UdpTransport: connected to server (conn={})", peer->id);

    if (on_connect_) {
        on_connect_(peer->id);
    }

    (void)header;
}

void UdpTransport::handle_disconnect(const Address& from) {
    u64 key = make_peer_key(from);
    auto it = peers_.find(key);
    if (it == peers_.end()) {
        return;
    }

    ConnectionId id = it->second.id;
    peers_.erase(it);

    if (!listening_) {
        connected_ = false;
    }

    NX_INFO("UdpTransport: peer {} disconnected", id);

    if (on_disconnect_) {
        on_disconnect_(id);
    }
}

void UdpTransport::handle_data(RemotePeer& peer, const u8* payload, u32 size,
                                bool reliable, const ProtocolHeader& header) {
    peer.last_recv = std::chrono::steady_clock::now();

    // Update remote ack state from the header
    update_remote_ack(peer, header.ack, header.ack_bits);

    // Update our tracking of remote sequence
    u32 seq = header.sequence;
    if (seq > peer.remote_sequence) {
        u32 shift = seq - peer.remote_sequence;
        if (shift <= 32) {
            peer.ack_bits <<= shift;
            peer.ack_bits |= 1u;
        } else {
            peer.ack_bits = 1u;
        }
        peer.remote_sequence = seq;
    } else {
        u32 diff = peer.remote_sequence - seq;
        if (diff > 0 && diff <= 32) {
            peer.ack_bits |= (1u << diff);
        }
    }

    // If reliable, send an ACK back
    if (reliable) {
        send_protocol(peer.address, PacketType::Ack,
                      0, peer.remote_sequence, peer.ack_bits);
    }

    // Deliver to application
    if (on_receive_ && size > 0) {
        on_receive_(peer.id, payload, size);
    }
}

void UdpTransport::handle_ack(RemotePeer& peer, const ProtocolHeader& header) {
    peer.last_recv = std::chrono::steady_clock::now();
    update_remote_ack(peer, header.ack, header.ack_bits);
}

void UdpTransport::handle_ping(const Address& from, const ProtocolHeader& header) {
    u64 key = make_peer_key(from);
    RemotePeer* peer = find_peer(key);
    if (peer) {
        peer->last_recv = std::chrono::steady_clock::now();
        send_protocol(from, PacketType::Pong,
                      header.sequence, peer->remote_sequence, peer->ack_bits);
    }
}

void UdpTransport::update_remote_ack(RemotePeer& peer, u32 ack, u32 ack_bits) {
    // Remove any pending reliable packets that have been acknowledged
    auto& pending = peer.pending_reliable;
    pending.erase(
        std::remove_if(pending.begin(), pending.end(),
            [ack, ack_bits](const PendingPacket& pkt) {
                if (pkt.sequence == ack) {
                    return true;
                }
                if (ack > pkt.sequence) {
                    u32 diff = ack - pkt.sequence;
                    if (diff <= 32 && (ack_bits & (1u << (diff - 1))) != 0) {
                        return true;
                    }
                }
                return false;
            }),
        pending.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// Timeouts and Retransmission
// ─────────────────────────────────────────────────────────────────────────────

void UdpTransport::check_timeouts() {
    auto now = std::chrono::steady_clock::now();
    std::vector<u64> timed_out;

    for (auto& [key, peer] : peers_) {
        auto elapsed = std::chrono::duration<f32>(now - peer.last_recv).count();
        if (elapsed > CONNECTION_TIMEOUT) {
            timed_out.push_back(key);
        }
    }

    for (u64 key : timed_out) {
        auto it = peers_.find(key);
        if (it == peers_.end()) {
            continue;
        }

        ConnectionId id = it->second.id;
        NX_WARN("UdpTransport: connection {} timed out", id);
        peers_.erase(it);

        if (!listening_) {
            connected_ = false;
        }

        if (on_disconnect_) {
            on_disconnect_(id);
        }
    }
}

void UdpTransport::retransmit_reliable() {
    auto now = std::chrono::steady_clock::now();

    for (auto& [key, peer] : peers_) {
        for (auto& pkt : peer.pending_reliable) {
            auto elapsed = std::chrono::duration<f32>(now - pkt.send_time).count();
            if (elapsed >= RETRANSMIT_INTERVAL) {
                send_protocol(peer.address, PacketType::DataReliable,
                              pkt.sequence, peer.remote_sequence, peer.ack_bits,
                              pkt.data.data(), static_cast<u32>(pkt.data.size()));
                pkt.send_time = now;
                pkt.retransmit_count++;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

u64 UdpTransport::make_peer_key(const Address& addr) {
    // Pack IPv4 address (as 32-bit integer) and port into a 64-bit key
    struct in_addr in{};
    if (inet_pton(AF_INET, addr.host.c_str(), &in) != 1) {
        return 0; // malformed host
    }
    u32 ip = in.s_addr; // Already in network byte order
    // Shift the full 32-bit IP into the high word so distinct addresses do not
    // collide (a 16-bit shift discarded the top 16 bits of the IP).
    return (static_cast<u64>(ip) << 32) | static_cast<u64>(addr.port);
}

UdpTransport::RemotePeer* UdpTransport::find_peer(u64 key) {
    auto it = peers_.find(key);
    return (it != peers_.end()) ? &it->second : nullptr;
}

UdpTransport::RemotePeer* UdpTransport::find_peer_by_id(ConnectionId id) {
    for (auto& [key, peer] : peers_) {
        if (peer.id == id) {
            return &peer;
        }
    }
    return nullptr;
}

void UdpTransport::send_raw(const Address& dest, const void* data, u32 size) {
    socket_.send_to(dest, data, size);
}

void UdpTransport::send_protocol(const Address& dest, PacketType type,
                                  u32 sequence, u32 ack, u32 ack_bits,
                                  const void* payload, u32 payload_size) {
    u8 buffer[MAX_PACKET_SIZE];

    if (HEADER_SIZE + payload_size > MAX_PACKET_SIZE) {
        NX_ERROR("UdpTransport: packet too large ({} bytes)", HEADER_SIZE + payload_size);
        return;
    }

    ProtocolHeader header{};
    header.protocol_id = PROTOCOL_ID;
    header.type = type;
    header.sequence = sequence;
    header.ack = ack;
    header.ack_bits = ack_bits;

    write_header(buffer, header);

    if (payload && payload_size > 0) {
        std::memcpy(buffer + HEADER_SIZE, payload, payload_size);
    }

    send_raw(dest, buffer, HEADER_SIZE + payload_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// Header Serialization (manual, no struct packing assumptions)
// ─────────────────────────────────────────────────────────────────────────────

void UdpTransport::write_header(u8* buf, const ProtocolHeader& hdr) {
    // protocol_id: 4 bytes, big-endian
    buf[0] = static_cast<u8>((hdr.protocol_id >> 24) & 0xFF);
    buf[1] = static_cast<u8>((hdr.protocol_id >> 16) & 0xFF);
    buf[2] = static_cast<u8>((hdr.protocol_id >> 8) & 0xFF);
    buf[3] = static_cast<u8>((hdr.protocol_id) & 0xFF);

    // type: 1 byte
    buf[4] = static_cast<u8>(hdr.type);

    // sequence: 4 bytes, big-endian
    buf[5] = static_cast<u8>((hdr.sequence >> 24) & 0xFF);
    buf[6] = static_cast<u8>((hdr.sequence >> 16) & 0xFF);
    buf[7] = static_cast<u8>((hdr.sequence >> 8) & 0xFF);
    buf[8] = static_cast<u8>((hdr.sequence) & 0xFF);

    // ack: 4 bytes, big-endian
    buf[9]  = static_cast<u8>((hdr.ack >> 24) & 0xFF);
    buf[10] = static_cast<u8>((hdr.ack >> 16) & 0xFF);
    buf[11] = static_cast<u8>((hdr.ack >> 8) & 0xFF);
    buf[12] = static_cast<u8>((hdr.ack) & 0xFF);

    // ack_bits: 4 bytes, big-endian
    buf[13] = static_cast<u8>((hdr.ack_bits >> 24) & 0xFF);
    buf[14] = static_cast<u8>((hdr.ack_bits >> 16) & 0xFF);
    buf[15] = static_cast<u8>((hdr.ack_bits >> 8) & 0xFF);
    buf[16] = static_cast<u8>((hdr.ack_bits) & 0xFF);
}

bool UdpTransport::read_header(const u8* buf, u32 size, ProtocolHeader& hdr) {
    if (size < HEADER_SIZE) {
        return false;
    }

    hdr.protocol_id = (static_cast<u32>(buf[0]) << 24) |
                      (static_cast<u32>(buf[1]) << 16) |
                      (static_cast<u32>(buf[2]) << 8) |
                      (static_cast<u32>(buf[3]));

    hdr.type = static_cast<PacketType>(buf[4]);

    hdr.sequence = (static_cast<u32>(buf[5]) << 24) |
                   (static_cast<u32>(buf[6]) << 16) |
                   (static_cast<u32>(buf[7]) << 8) |
                   (static_cast<u32>(buf[8]));

    hdr.ack = (static_cast<u32>(buf[9]) << 24) |
              (static_cast<u32>(buf[10]) << 16) |
              (static_cast<u32>(buf[11]) << 8) |
              (static_cast<u32>(buf[12]));

    hdr.ack_bits = (static_cast<u32>(buf[13]) << 24) |
                   (static_cast<u32>(buf[14]) << 16) |
                   (static_cast<u32>(buf[15]) << 8) |
                   (static_cast<u32>(buf[16]));

    return true;
}

} // namespace nexus::net
