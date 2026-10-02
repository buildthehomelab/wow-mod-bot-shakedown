/*
 * mod-bot-shakedown
 *
 * Say "cloth" in party or raid chat and every playerbot in your group hands you the cloth in its
 * bags. The items move straight into your bags the way a finished trade moves them, so there is
 * no trade window to open, fill and accept for each bot.
 *
 * Other trade goods have their own keyword (leather, ore, herb, ...), turned on in the config.
 * A keyword only counts when it is the whole message, so "anyone got cloth?" does nothing.
 *
 * Only playerbots give, and only to a real player in the same group on the same map. Soulbound
 * items, gray junk and anything a trade window would refuse stay where they are. On stock
 * AzerothCore, which has no bots, the module does nothing.
 *
 * Released under the MIT License.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // Keyword -> trade goods subclass. The names match the playerbots "t <keyword>" command.
    std::unordered_map<std::string, uint32> const KEYWORDS =
    {
        { "cloth",      ITEM_SUBCLASS_CLOTH },
        { "leather",    ITEM_SUBCLASS_LEATHER },
        { "ore",        ITEM_SUBCLASS_METAL_STONE },
        { "meat",       ITEM_SUBCLASS_MEAT },
        { "herb",       ITEM_SUBCLASS_HERB },
        { "elemental",  ITEM_SUBCLASS_ELEMENTAL },
        { "enchanting", ITEM_SUBCLASS_ENCHANTING },
    };

    constexpr std::array<uint32, 8> QUALITY_COLORS =
    {
        0xff9d9d9d, 0xffffffff, 0xff1eff00, 0xff0070dd, 0xffa335ee, 0xffff8000, 0xffe6cc80, 0xffe6cc80
    };

    struct Config
    {
        bool enabled = true;
        std::unordered_map<std::string, uint32> keywords;
    };

    Config config;

    // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
    // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
    // neither. Looking for both at compile time lets the module build on all of them.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsBot(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && IsBotSession(session);
    }

    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !IsBotSession(session);
    }

    bool IsGroupChat(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
                return true;
            default:
                return false;
        }
    }

    std::string Normalize(std::string text)
    {
        auto isSpace = [](unsigned char c) { return std::isspace(c); };
        text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
        text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    bool ShouldGive(Item* item, uint32 subClass)
    {
        if (!item)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS || proto->SubClass != subClass)
            return false;

        // Some gray junk is flagged as trade goods in Blizzard's data.
        if (proto->Quality == ITEM_QUALITY_POOR)
            return false;

        // The same test a trade window applies: not soulbound, not being looted, and so on.
        return !item->IsSoulBound() && item->CanBeTraded();
    }

    std::vector<Item*> FindItems(Player* bot, uint32 subClass)
    {
        std::vector<Item*> items;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (ShouldGive(item, subClass))
                items.push_back(item);
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                Item* item = bag->GetItemByPos(uint8(slot));
                if (ShouldGive(item, subClass))
                    items.push_back(item);
            }
        }

        return items;
    }

    std::string DescribeItem(uint32 entry, uint32 count)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return Acore::StringFormat("{}x item {}", count, entry);

        return Acore::StringFormat("{}x |c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", count,
            QUALITY_COLORS[std::min<uint32>(proto->Quality, QUALITY_COLORS.size() - 1)], entry, proto->Name1);
    }

    struct GiveResult
    {
        std::map<uint32, uint32> given; // entry -> count
        bool bagsFull = false;
    };

    // Moves the items the same way a finished trade does: out of the bot's inventory, into the
    // receiver's, merging into existing stacks where it can.
    GiveResult Give(Player* bot, Player* receiver, std::vector<Item*> const& items)
    {
        GiveResult result;

        for (Item* item : items)
        {
            ItemPosCountVec dest;
            if (receiver->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            {
                result.bagsFull = true;
                break;
            }

            uint32 const entry = item->GetEntry();
            uint32 const count = item->GetCount();

            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            // The item may be merged into an existing stack and deleted here; don't touch it after.
            receiver->MoveItemToInventory(dest, item, true, true);

            result.given[entry] += count;
        }

        if (!result.given.empty())
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bot->SaveInventoryAndGoldToDB(trans);
            receiver->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }

        return result;
    }

    // Returns false when the group has no bot on the receiver's map, so there was nobody to ask.
    bool HandleRequest(Player* receiver, Group* group, std::string const& keyword, uint32 subClass)
    {
        ChatHandler chat(receiver->GetSession());
        bool anyBot = false;
        bool anyGiven = false;
        bool bagsFull = false;

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* bot = itr->GetSource();
            if (!bot || bot == receiver || !IsBot(bot) || !bot->IsInWorld() || bot->GetMap() != receiver->GetMap())
                continue;

            anyBot = true;

            std::vector<Item*> items = FindItems(bot, subClass);
            if (items.empty())
                continue;

            if (bagsFull)
            {
                chat.PSendSysMessage("{} still has {} for you.", bot->GetName(), keyword);
                continue;
            }

            GiveResult result = Give(bot, receiver, items);

            if (!result.given.empty())
            {
                std::ostringstream list;
                for (auto const& [entry, count] : result.given)
                {
                    if (list.tellp() > 0)
                        list << ", ";

                    list << DescribeItem(entry, count);
                }

                chat.PSendSysMessage("{} gives you {}.", bot->GetName(), list.str());
                anyGiven = true;
            }

            if (result.bagsFull)
            {
                bagsFull = true;
                chat.PSendSysMessage("Your bags are full: {} still has {} for you.", bot->GetName(), keyword);
            }
        }

        if (!anyBot)
        {
            chat.PSendSysMessage("There are no bots in your group here to hand over their {}.", keyword);
            return false;
        }

        if (!anyGiven && !bagsFull)
            chat.PSendSysMessage("None of your bots have any {}.", keyword);

        return true;
    }
}

class BotShakedownWorldScript : public WorldScript
{
public:
    BotShakedownWorldScript() : WorldScript("BotShakedownWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("BotShakedown.Enable", true);

        config.keywords.clear();
        std::istringstream keywords(Normalize(sConfigMgr->GetOption<std::string>("BotShakedown.Keywords", "cloth")));
        std::string keyword;
        while (keywords >> keyword)
        {
            auto const itr = KEYWORDS.find(keyword);
            if (itr == KEYWORDS.end())
            {
                LOG_ERROR("module", "mod-bot-shakedown: unknown keyword '{}' in BotShakedown.Keywords, ignored.",
                    keyword);
                continue;
            }

            config.keywords.insert(*itr);
        }

        std::string active;
        for (auto const& [name, subClass] : config.keywords)
            active += (active.empty() ? "" : " ") + name;

        LOG_INFO("server.loading", "mod-bot-shakedown: {}, keywords: {}", config.enabled ? "enabled" : "disabled",
            active.empty() ? "none" : active);
    }
};

class BotShakedownPlayerScript : public PlayerScript
{
public:
    BotShakedownPlayerScript() : PlayerScript("BotShakedownPlayerScript") { }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*language*/, std::string& msg, Group* group) override
    {
        if (!config.enabled || !group || !IsGroupChat(type) || !IsRealPlayer(player))
            return true;

        std::string const keyword = Normalize(msg);
        auto const itr = config.keywords.find(keyword);
        if (itr == config.keywords.end())
            return true;

        // Swallow the keyword once the bots have been asked. Otherwise mod-playerbots also reads it
        // (AiPlayerbot.EnableAutoTradeOnItemMention) and every bot whispers its count and opens a
        // trade window. This module loads before mod-playerbots, so its hook runs first.
        return !HandleRequest(player, group, keyword, itr->second);
    }
};

void AddBotShakedownScripts()
{
    new BotShakedownWorldScript();
    new BotShakedownPlayerScript();
}
