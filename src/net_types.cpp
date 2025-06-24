// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_types.h>

#include <logging.h>
#include <netaddress.h>
#include <netbase.h>
#include <univalue.h>

static const char* BANMAN_JSON_VERSION_KEY{"version"};

CBanEntry::CBanEntry(const UniValue& json)
    : nVersion(json[BANMAN_JSON_VERSION_KEY].getInt<int>()),
      nCreateTime(json["ban_created"].getInt<int64_t>()),
      nBanUntil(json["banned_until"].getInt<int64_t>())
{
}

UniValue CBanEntry::ToJson() const
{
    UniValue json(UniValue::VOBJ);
    json.pushKV(BANMAN_JSON_VERSION_KEY, nVersion);
    json.pushKV("ban_created", nCreateTime);
    json.pushKV("banned_until", nBanUntil);
    return json;
}

static const char* BANMAN_JSON_ADDR_KEY = "address";

/**
 * Convert a `banmap_t` object to a JSON array.
 * @param[in] bans Bans list to convert.
 * @return a JSON array, similar to the one returned by the `listbanned` RPC. Suitable for
 * passing to `BanMapFromJson()`.
 */
UniValue BanMapToJson(const banmap_t& bans)
{
    UniValue bans_json(UniValue::VARR);
    for (const auto& it : bans) {
        const auto& address = it.first;
        const auto& ban_entry = it.second;
        UniValue j = ban_entry.ToJson();
        j.pushKV(BANMAN_JSON_ADDR_KEY, address.ToString());
        bans_json.push_back(std::move(j));
    }
    return bans_json;
}

/**
 * Convert a JSON array to a `banmap_t` object.
 * @param[in] bans_json JSON to convert, must be as returned by `BanMapToJson()`.
 * @param[out] bans Bans list to create from the JSON.
 * @throws std::runtime_error if the JSON does not have the expected fields or they contain
 * unparsable values.
 */
void BanMapFromJson(const UniValue& bans_json, banmap_t& bans)
{
    for (const auto& ban_entry_json : bans_json.getValues()) {
        const int version{ban_entry_json[BANMAN_JSON_VERSION_KEY].getInt<int>()};
        if (version != CBanEntry::CURRENT_VERSION) {
            LogPrintf("Dropping entry with unknown version (%s) from ban list\n", version);
            continue;
        }
        const auto& subnet_str = ban_entry_json[BANMAN_JSON_ADDR_KEY].get_str();
        const CSubNet subnet{LookupSubNet(subnet_str)};
        if (!subnet.IsValid()) {
            LogPrintf("Dropping entry with unparseable address or subnet (%s) from ban list\n", subnet_str);
            continue;
        }
        bans.insert_or_assign(subnet, CBanEntry{ban_entry_json});
    }
}

UniValue ASBanMapToJson(const std::map<uint32_t, CBanEntry>& bans)
{
    UniValue bans_json(UniValue::VARR);
    for (const auto& it : bans) {
        const auto& as_number = it.first;
        const auto& ban_entry = it.second;
        UniValue j = ban_entry.ToJson();
        j.pushKV(BANMAN_JSON_ADDR_KEY, "AS" + std::to_string(as_number));
        bans_json.push_back(std::move(j));
    }
    return bans_json;
}

void ASBanMapFromJson(const UniValue& bans_json, std::map<uint32_t, CBanEntry>& bans)
{
    for (const auto& ban_entry_json : bans_json.getValues()) {
        const int version{ban_entry_json[BANMAN_JSON_VERSION_KEY].getInt<int>()};
        if (version != CBanEntry::CURRENT_VERSION) {
            LogPrintf("Dropping AS entry with unknown version (%s) from ban list\n", version);
            continue;
        }
        const auto& as_str = ban_entry_json[BANMAN_JSON_ADDR_KEY].get_str();
        // Check if it's an AS number (starts with "AS")
        if (as_str.length() > 2 && as_str.substr(0, 2) == "AS") {
            try {
                uint32_t as_number = std::stoul(as_str.substr(2));
                if (as_number == 0) {
                    LogPrintf("Dropping entry with invalid AS number (AS0) from ban list\n");
                    continue;
                }
                bans.insert_or_assign(as_number, CBanEntry{ban_entry_json});
            } catch (const std::exception&) {
                LogPrintf("Dropping entry with unparseable AS number (%s) from ban list\n", as_str);
                continue;
            }
        } else {
            LogPrintf("Dropping entry with invalid AS format (%s) from ban list\n", as_str);
            continue;
        }
    }
}
