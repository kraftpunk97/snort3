//--------------------------------------------------------------------------
// Copyright (C) 2014-2025 Cisco and/or its affiliates. All rights reserved.
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
// ips_sid.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "framework/decode_data.h"
#include "framework/ips_option.h"
#include "framework/module.h"

using namespace snort;

#define s_name "sid"

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "~", Parameter::PT_INT, "1:max32", nullptr,
      "signature id" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define s_help \
    "rule option to indicate signature number"

class SidModule : public Module
{
public:
    SidModule() : Module(s_name, s_help, s_params) { }
    bool set(const char*, Value&, SnortConfig*) override;

    Usage get_usage() const override
    { return DETECT; }

public:
    uint32_t sid = 0;
};

bool SidModule::set(const char*, Value& v, SnortConfig*)
{
    assert(v.is("~"));
    sid = v.get_uint32();
    return true;
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new SidModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static IpsOption* sid_ctor(Module* p, IpsInfo& info)
{
    SidModule* m = (SidModule*)p;
    IpsOption::set_sid(info, m->sid);
    return nullptr;
}

static const IpsApi sid_api =
{
    {
        PT_IPS_OPTION,
        sizeof(IpsApi),
        IPSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        s_name,
        s_help,
        mod_ctor,
        mod_dtor
    },
    OPT_TYPE_META,
    1, PROTO_BIT__NONE,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    sid_ctor,
    nullptr,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
#else
const BaseApi* ips_sid[] =
#endif
{
    &sid_api.base,
    nullptr
};

