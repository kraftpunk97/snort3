//--------------------------------------------------------------------------
// Copyright (C) 2016-2025 Cisco and/or its affiliates. All rights reserved.
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

// ips_sd_pattern.cc author Victor Roemer <viroemer@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cctype>
#include <climits>

#include <hs_compile.h>
#include <hs_runtime.h>

#include "detection/pattern_match_data.h"
#include "framework/cursor.h"
#include "framework/ips_option.h"
#include "framework/module.h"
#include "hash/hash_key_operations.h"
#include "helpers/hyper_scratch_allocator.h"
#include "log/messages.h"
#include "log/obfuscator.h"
#include "main/snort_config.h"
#include "profiler/profiler.h"
#include "protocols/packet.h"

#include "sd_credit_card.h"

using namespace snort;

#define s_name "sd_pattern"
#define s_help "rule option for detecting sensitive data"

#define SD_SOCIAL_PATTERN          R"([0-8]\d{2}-\d{2}-\d{4})"
#define SD_SOCIAL_NODASHES_PATTERN R"([0-8]\d{8})"
#define SD_CREDIT_PATTERN_ALL      R"(\d{4}\D?\d{4}\D?\d{2}\D?\d{2}\D?\d{3,4})"
#define SD_EMAIL_PATTERN           R"([a-zA-Z0-9!#$%&'*+\/=?^_`{|}~-]+(?:\.[a-zA-Z0-9!#$%&'*+\/=?^_`{|}~-]+)*@(?:[a-zA-Z0-9](?:[a-zA-Z0-9-]*[a-zA-Z0-9])?\.)+[a-zA-Z0-9](?:[a-zA-Z0-9-]*[a-zA-Z0-9])?)"
#define SD_US_PHONE_PATTERN        R"((?:\+?1[-\.\s]?)?\(?([2-9][0-8]\d)\)?[-\.\s]([2-9]\d{2})[-\.\s](\d{4}))"

static HyperScratchAllocator* scratcher = nullptr;

struct SdStats
{
    PegCount nomatch_threshold;
    PegCount nomatch_notfound;
    PegCount terminated;
};

const PegInfo sd_pegs[] =
{
    { CountType::SUM, "below_threshold", "sd_pattern matched but missed threshold" },
    { CountType::SUM, "pattern_not_found", "sd_pattern did not not match" },
    { CountType::SUM, "terminated", "hyperscan terminated" },
    { CountType::END, nullptr, nullptr }
};

static THREAD_LOCAL SdStats s_stats;

struct SdPatternConfig
{
    PatternMatchData pmd = { };
    hs_database_t* db;

    std::string pii;
    unsigned threshold = 1;
    bool can_be_obfuscated = false;
    bool forced_boundary = false;
    int (* validate)(const uint8_t* buf, unsigned long long buflen) = nullptr;

    bool operator==(const SdPatternConfig& rhs) const
    { return pii == rhs.pii and threshold == rhs.threshold; }

    SdPatternConfig()
    { reset(); }

    void reset()
    {
        pii.clear();
        threshold = 1;
        can_be_obfuscated = false;
        validate = nullptr;
        db = nullptr;
        pmd = { };
    }
};

static THREAD_LOCAL ProfileStats sd_pattern_perf_stats;

//-------------------------------------------------------------------------
// SSN validation functions
//-------------------------------------------------------------------------

#pragma pack(push, 0)

struct ssn_no_dashes
{
    char area[3];
    char group[2];
    char serial[4];
};

struct ssn_with_dashes
{
    char area[3];
    char dash_1;
    char group[2];
    char dash_2;
    char serial[4];
};

#pragma pack(pop)

static int validate_us_ssn_nodashes(const uint8_t* buf, unsigned long long len)
{
    if (len != sizeof(ssn_no_dashes))
        return false;

    const ssn_no_dashes* ssn = (const ssn_no_dashes*)buf;

    return strncmp(ssn->area, "000", 3)
        and strncmp(ssn->area, "666", 3)
        and strncmp(ssn->group, "00", 2)
        and strncmp(ssn->serial, "0000", 4);
}

static int validate_us_ssn(const uint8_t* buf, unsigned long long len)
{
    if (len != sizeof(ssn_with_dashes))
        return false;

    const ssn_with_dashes* ssn = (const ssn_with_dashes*)buf;

    return strncmp(ssn->area, "000", 3)
        and strncmp(ssn->area, "666", 3)
        and strncmp(ssn->group, "00", 2)
        and strncmp(ssn->serial, "0000", 4);
}

//-------------------------------------------------------------------------
// option
//-------------------------------------------------------------------------

class SdPatternOption : public IpsOption
{
public:
    SdPatternOption(const SdPatternConfig&);
    ~SdPatternOption() override;

    uint32_t hash() const override;
    bool operator==(const IpsOption&) const override;

    PatternMatchData* get_pattern(SnortProtocolId, RuleDirection) override
    { return &config.pmd; }

    EvalStatus eval(Cursor&, Packet* p) override;

    CursorActionType get_cursor_type() const override
    { return CAT_READ; }

private:
    unsigned SdSearch(const Cursor&, Packet*);
    SdPatternConfig config;
};

SdPatternOption::SdPatternOption(const SdPatternConfig& c) :
    IpsOption(s_name), config(c)
{
    if ( !scratcher->allocate(config.db) )
        ParseError("can't allocate scratch for sd_pattern '%s'", config.pii.c_str());

    config.pmd.pattern_buf = config.pii.c_str();
    config.pmd.pattern_size = config.pii.size();
    config.pmd.fp_length = config.pmd.pattern_size;
    config.pmd.fp_offset = 0;
}

SdPatternOption::~SdPatternOption()
{
    if ( config.db )
        hs_free_database(config.db);
}

uint32_t SdPatternOption::hash() const
{
    uint32_t a = config.pmd.pattern_size;
    uint32_t b = IpsOption::hash();
    uint32_t c = config.threshold;

    mix(a, b, c);

    mix_str(a, b, c, config.pii.c_str());
    //mix_str(a, b, c, config.pmd.sticky_buf.c_str());

    finalize(a, b, c);

    return c;
}

bool SdPatternOption::operator==(const IpsOption& ips) const
{
    if ( !IpsOption::operator==(ips) )
        return false;

    const SdPatternOption& rhs = static_cast<const SdPatternOption&>(ips);

    if ( config == rhs.config )
        return true;

    return false;
}

struct hsContext
{
    hsContext(const SdPatternConfig& c_, Packet* p_, const uint8_t* const start_,
        const uint8_t* _buf, unsigned int _buf_len, const char* _buf_name)
        : config(c_), packet(p_), start(start_), buf(_buf), buf_name(_buf_name), buf_len(_buf_len)
    { }

    bool has_valid_bounds(unsigned long long from, unsigned long long len)
    {
        bool left = false;
        bool right = false;

        // validate the left side

        if ( from == 0 )
            left = true;
        else if ( !::isdigit((int)buf[from-1]) )
            left = true;

        // validate the right side

        if ( from+len == buf_len )
            right = true;
        else if ( from + len < buf_len && !::isdigit((int)buf[from+len]) )
            right = true;

        return left and right;
    }

    unsigned long long last_match_from_pos = ULLONG_MAX;
    unsigned long long last_match_to_pos = 0;
    unsigned int count = 0;

    SdPatternConfig config;
    Packet* packet = nullptr;
    const uint8_t* const start = nullptr;
    const uint8_t* buf = nullptr;
    const char* buf_name;
    unsigned int buf_len = 0;
    bool buf_set = false;
};

static int hs_match(unsigned int /*id*/, unsigned long long from,
        unsigned long long to, unsigned int /*flags*/, void *context)
{
    hsContext* ctx = (hsContext*) context;

    assert(ctx);
    assert(ctx->packet);
    assert(ctx->start);

    unsigned long long len = to - from;

    if ( ctx->config.forced_boundary && !ctx->has_valid_bounds(from, len) )
        return 0;

    if ( ctx->config.validate && ctx->config.validate(ctx->buf+from, len) != 1 )
        return 0;

    if (from >= ctx->last_match_to_pos)
    {
        ctx->last_match_from_pos = from;
        ctx->count++;
    }
    else if (from != ctx->last_match_from_pos)
        return 0;

    ctx->last_match_to_pos = to;

    IpsPolicy* p = get_ips_policy();

    assert(p);

    if ( p->obfuscate_pii and ctx->config.can_be_obfuscated )
    {
        if ( !ctx->packet->obfuscator )
            ctx->packet->obfuscator = new Obfuscator();

        if ( !ctx->buf_set )
        {
            ctx->packet->obfuscator->set_buffer(ctx->buf_name);
            ctx->buf_set = true;
        }

        // FIXIT-L Make configurable or don't show any PII partials (0 for user defined??)
        uint32_t off = ctx->buf + from - ctx->start;
        ctx->packet->obfuscator->push(off, len - 4);
    }

    return 0;
}

unsigned SdPatternOption::SdSearch(const Cursor& c, Packet* p)
{
    const uint8_t* const start = c.buffer();
    const uint8_t* buf = c.start();
    unsigned int buf_len = c.length();

    hsContext ctx(config, p, start, buf, buf_len, c.get_name());

    hs_error_t stat = hs_scan(config.db, (const char*)buf, buf_len, 0,
        scratcher->get(), hs_match, (void*)&ctx);

    if ( stat == HS_SCAN_TERMINATED )
        ++s_stats.terminated;

    return ctx.count;
}

IpsOption::EvalStatus SdPatternOption::eval(Cursor& c, Packet* p)
{
    // cppcheck-suppress unreadVariable
    RuleProfile profile(sd_pattern_perf_stats);

    unsigned matches = SdSearch(c, p);

    if ( matches >= config.threshold )
        return MATCH;

    else if ( matches == 0 )
        ++s_stats.nomatch_notfound;

    else
        ++s_stats.nomatch_threshold;

    return NO_MATCH;
}

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "~pattern", Parameter::PT_STRING, nullptr, nullptr,
      "The pattern to search for" },

    { "threshold", Parameter::PT_INT, "1:max32", "1",
      "number of matches before alerting" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class SdPatternModule : public Module
{
public:
    SdPatternModule() : Module(s_name, s_help, s_params)
    { scratcher = new HyperScratchAllocator; }

    ~SdPatternModule() override
    { delete scratcher; }

    bool begin(const char*, int, SnortConfig*) override;
    bool set(const char*, Value& v, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

    const PegInfo* get_pegs() const override
    { return sd_pegs; }

    PegCount* get_counts() const override
    { return (PegCount*)&s_stats; }

    ProfileStats* get_profile() const override
    { return &sd_pattern_perf_stats; }

    void get_data(SdPatternConfig& c)
    { c = config; }

    Usage get_usage() const override
    { return DETECT; }

private:
    SdPatternConfig config;
};

bool SdPatternModule::begin(const char*, int, SnortConfig*)
{
    if ( hs_valid_platform() != HS_SUCCESS )
    {
        ParseError("This host does not support Hyperscan.");
        return false;
    }

    config.reset();
    return true;
}

bool SdPatternModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("~pattern") )
        config.pii = v.get_unquoted_string();
    else if ( v.is("threshold") )
        config.threshold = v.get_uint32();

    return true;
}

bool SdPatternModule::end(const char*, int, SnortConfig*)
{
    if (config.pii == "credit_card")
    {
        config.pii = SD_CREDIT_PATTERN_ALL;
        config.validate = SdLuhnAlgorithm;
        config.can_be_obfuscated = true;
        config.forced_boundary = true;
    }
    else if (config.pii == "us_social")
    {
        config.pii = SD_SOCIAL_PATTERN;
        config.validate = validate_us_ssn;
        config.can_be_obfuscated = true;
        config.forced_boundary = true;
    }
    else if (config.pii == "us_social_nodashes")
    {
        config.pii = SD_SOCIAL_NODASHES_PATTERN;
        config.validate = validate_us_ssn_nodashes;
        config.can_be_obfuscated = true;
        config.forced_boundary = true;
    }
    else if (config.pii == "email")
    {
        config.pii = SD_EMAIL_PATTERN;
        config.can_be_obfuscated = true;
    }
    else if (config.pii == "us_phone")
    {
        config.pii = SD_US_PHONE_PATTERN;
        config.can_be_obfuscated = true;
        config.forced_boundary = true;
    }

    hs_compile_error_t* err = nullptr;

    if ( hs_compile(config.pii.c_str(), HS_FLAG_DOTALL|HS_FLAG_SOM_LEFTMOST, HS_MODE_BLOCK,
        nullptr, &config.db, &err)
        or !config.db )
    {
        ParseError("can't compile regex '%s'", config.pii.c_str());
        hs_free_compile_error(err);
        return false;
    }
    return true;
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new SdPatternModule;
}

static void mod_dtor(Module* p)
{
    delete p;
}

static IpsOption* sd_pattern_ctor(Module* m, IpsInfo&)
{
    SdPatternModule* mod = (SdPatternModule*)m;
    SdPatternConfig c;
    mod->get_data(c);
    return new SdPatternOption(c);
}

static void sd_pattern_dtor(IpsOption* p)
{
    delete p;
}

static const IpsApi sd_pattern_api =
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
    OPT_TYPE_DETECTION,
    0, 0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    sd_pattern_ctor,
    sd_pattern_dtor,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
#else
const BaseApi* ips_sd_pattern[] =
#endif
{
    &sd_pattern_api.base,
    nullptr
};
