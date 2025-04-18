//--------------------------------------------------------------------------
// Copyright (C) 2017-2025 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// u2_packet.h author Russ Combs <rucombs@cisco.com>

#ifndef U2_PACKET_H
#define U2_PACKET_H

// generate eth:ip:tcp headers for compatibility  with legacy tool chains
// that must have packets

#include "protocols/eth.h"
#include "protocols/ipv4.h"
#include "protocols/ipv6.h"
#include "protocols/tcp.h"
#include "protocols/udp.h"

#include "main/snort_types.h"

namespace snort
{
struct Packet;

class SO_PUBLIC U2PseudoHeader
{
public:
    U2PseudoHeader(const Packet*);

    uint8_t* get_data();
    uint16_t get_size();
    uint16_t get_dsize();

private:
    void cook_eth(const Packet*, eth::EtherHdr&);
    void cook_ip4(const Packet*, ip::IP4Hdr&);
    void cook_ip6(const Packet*, ip::IP6Hdr&);
    void cook_tcp(const Packet*, tcp::TCPHdr&);
    void cook_udp(const Packet*, udp::UDPHdr&);

    static const unsigned offset = 2;
    static const unsigned max_size =
        offset + sizeof(eth::EtherHdr) + sizeof(ip::IP6Hdr) + sizeof(tcp::TCPHdr);

    uint16_t size = 0;
    uint16_t dsize = 0;

    union
    {
        struct
        {
            uint8_t align[offset];
            eth::EtherHdr eth;
            ip::IP4Hdr ip4;
            union
            {
                tcp::TCPHdr tcp;
                udp::UDPHdr udp;
            } proto;
        } v4;

        struct
        {
            uint8_t align[offset];
            eth::EtherHdr eth;
            ip::IP6Hdr ip6;
            union
            {
                tcp::TCPHdr tcp;
                udp::UDPHdr udp;
            } proto;
        } v6;

        uint8_t buf[max_size];
    } u;
};
}
#endif

