//--------------------------------------------------------------------------
// Copyright (C) 2015-2025 Cisco and/or its affiliates. All rights reserved.
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

// tcp_connector_module.cc author Ed Borgoyn <eborgoyn@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "tcp_connector_module.h"

#include <sstream>
#include <string>

#include "log/messages.h"
#include "main/thread_config.h"

using namespace snort;
using namespace std;

static const Parameter tcp_connector_params[] =
{
    { "connector", Parameter::PT_STRING, nullptr, nullptr,
      "connector name" },

    { "address", Parameter::PT_ADDR, nullptr, nullptr,
      "address of the remote end-point" },

    { "ports", Parameter::PT_INT_LIST, "65535", nullptr,
      "list of ports of the remote end-point" },

    { "setup", Parameter::PT_ENUM, "call | answer", nullptr,
      "stream establishment" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const PegInfo tcp_connector_pegs[] =
{
    { CountType::SUM, "messages", "total messages" },
    { CountType::END, nullptr, nullptr }
};

extern THREAD_LOCAL SimpleStats tcp_connector_stats;
extern THREAD_LOCAL ProfileStats tcp_connector_perfstats;

//-------------------------------------------------------------------------
// tcp_connector module
//-------------------------------------------------------------------------

TcpConnectorModule::TcpConnectorModule() :
    Module(TCP_CONNECTOR_NAME, TCP_CONNECTOR_HELP, tcp_connector_params, true)
{ }

ProfileStats* TcpConnectorModule::get_profile() const
{ return &tcp_connector_perfstats; }

static void fill_ports(vector<string>& ports, const string& s)
{
    string port;
    stringstream ss(s);

    while ( ss >> port )
        ports.push_back(port);
}

bool TcpConnectorModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("connector") )
        config->connector_name = v.get_string();

    else if ( v.is("address") )
        config->address = v.get_string();

    else if ( v.is("ports") )
        fill_ports(config->ports, v.get_string());

    else if ( v.is("setup") )
    {
        switch ( v.get_uint8() )
        {
        case 0:
            config->setup = TcpConnectorConfig::CALL;
            break;
        case 1:
            config->setup = TcpConnectorConfig::ANSWER;
            break;
        default:
            return false;
        }
    }

    return true;
}

ConnectorConfig::ConfigSet TcpConnectorModule::get_and_clear_config()
{
    return std::move(config_set);
}

bool TcpConnectorModule::begin(const char*, int, SnortConfig*)
{
    if ( !config )
    {
        config = std::make_unique<TcpConnectorConfig>();
        config->direction = Connector::CONN_DUPLEX;
    }

    return true;
}

bool TcpConnectorModule::end(const char*, int idx, SnortConfig*)
{
    if (idx != 0)
    {
        if ( config->ports.size() > 1 and config->ports.size() < ThreadConfig::get_instance_max() )
        {
            ParseError("The number of ports specified is insufficient to cover all threads. "
                "Number of threads: %d.", ThreadConfig::get_instance_max());
            return false;
        }

        config_set.emplace_back(std::move(config));
    }

    return true;
}

const PegInfo* TcpConnectorModule::get_pegs() const
{ return tcp_connector_pegs; }

PegCount* TcpConnectorModule::get_counts() const
{ return (PegCount*)&tcp_connector_stats; }

