/*
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "OutdoorPvPMgr.h"
#include "ScriptSystem.h"
#include "Transport.h"
#include "Vehicle.h"
#include "SmartAI.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "GossipDef.h"
#include "CreatureAIImpl.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Chat.h"

// namespace
// {
    UnusedScriptNamesContainer UnusedScriptNames;
// }

// Trait which indicates whether this script type
// must be assigned in the database.
template<typename>
struct is_script_database_bound
    : std::false_type { };

template<>
struct is_script_database_bound<SpellScriptLoader>
    : std::true_type { };

template<>
struct is_script_database_bound<InstanceMapScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ItemScript>
    : std::true_type { };

template<>
struct is_script_database_bound<CreatureScript>
    : std::true_type { };

template<>
struct is_script_database_bound<GameObjectScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AreaTriggerScript>
    : std::true_type { };

template<>
struct is_script_database_bound<BattlegroundScript>
    : std::true_type { };

template<>
struct is_script_database_bound<OutdoorPvPScript>
    : std::true_type { };

template<>
struct is_script_database_bound<WeatherScript>
    : std::true_type { };

template<>
struct is_script_database_bound<ConditionScript>
    : std::true_type { };

template<>
struct is_script_database_bound<TransportScript>
    : std::true_type { };

template<>
struct is_script_database_bound<AchievementCriteriaScript>
    : std::true_type { };

// This is the global static registry of scripts.
template<class TScript>
class ScriptRegistry
{
    public:

        typedef std::map<uint32, TScript*> ScriptMap;
        typedef typename ScriptMap::iterator ScriptMapIterator;

        // The actual list of scripts. This will be accessed concurrently, so it must not be modified
        // after server startup.
        static ScriptMap ScriptPointerList;
        static std::vector<TScript*> Scripts;

        static void AddScript(TScript* const script, bool addToDeleteContainer = true)
        {
            ASSERT(script);

            // See if the script is using the same memory as another script. If this happens, it means that
            // someone forgot to allocate new memory for a script.
            for (ScriptMapIterator it = ScriptPointerList.begin(); it != ScriptPointerList.end(); ++it)
            {
                if (it->second == script)
                {
                    TC_LOG_ERROR("scripts", "Script '%s' has same memory pointer as '%s'.",
                        script->GetName().c_str(), it->second->GetName().c_str());

                    return;
                }
            }

            AddScript(is_script_database_bound<TScript>{}, script);
            if (addToDeleteContainer)
                Scripts.push_back(script);
        }

        // Gets a script by its ID (assigned by ObjectMgr).
        static TScript* GetScriptById(uint32 id)
        {
            ScriptMapIterator it = ScriptPointerList.find(id);
            if (it != ScriptPointerList.end())
                return it->second;

            return NULL;
        }

    private:

        // Adds a database bound script
        static void AddScript(std::true_type, TScript* const script)
        {
            // Get an ID for the script. An ID only exists if it's a script that is assigned in the database
            // through a script name (or similar).
            uint32 id = sObjectMgr->GetScriptId(script->GetName());
            if (id)
            {
                // Try to find an existing script.
                bool existing = false;
                for (ScriptMapIterator it = ScriptPointerList.begin(); it != ScriptPointerList.end(); ++it)
                {
                    // If the script names match...
                    if (it->second->GetName() == script->GetName())
                    {
                        // ... It exists.
                        existing = true;
                        break;
                    }
                }

                // If the script isn't assigned -> assign it!
                if (!existing)
                {
                    ScriptPointerList[id] = script;
                    sScriptMgr->IncrementScriptCount();

                #ifdef SCRIPTS
                    UnusedScriptNamesContainer::iterator itr = std::lower_bound(UnusedScriptNames.begin(), UnusedScriptNames.end(), script->GetName());
                    if (itr != UnusedScriptNames.end() && *itr == script->GetName())
                        UnusedScriptNames.erase(itr);
                #endif
                }
                else
                {
                    // If the script is already assigned -> delete it!
                    TC_LOG_ERROR("scripts", "Script '%s' already assigned with the same script name, so the script can't work.",
                        script->GetName().c_str());

                    ABORT(); // Error that should be fixed ASAP.
                }
            }
            else
            {
                // The script uses a script name from database, but isn't assigned to anything.
                TC_LOG_ERROR("sql.sql", "Script named '%s' does not have a script name assigned in database.", script->GetName().c_str());
            }
        }

        // Adds a non database bound script
        static void AddScript(std::false_type, TScript* const script)
        {
            // We're dealing with a code-only script; just add it.
            ScriptPointerList[_scriptIdCounter++] = script;
            sScriptMgr->IncrementScriptCount();
        }

        // Counter used for code-only scripts.
        static uint32 _scriptIdCounter;
};

// Utility macros to refer to the script registry.
#define SCR_REG_MAP(T) ScriptRegistry<T>::ScriptMap
#define SCR_REG_ITR(T) ScriptRegistry<T>::ScriptMapIterator
#define SCR_REG_LST(T) ScriptRegistry<T>::ScriptPointerList
#define SCR_REG_VEC(T) ScriptRegistry<T>::Scripts

// Utility macros for looping over scripts.
#define FOR_SCRIPTS(T, C, E) \
    if (SCR_REG_LST(T).empty()) \
        return; \
    for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
        C != SCR_REG_LST(T).end(); ++C)
#define FOR_SCRIPTS_RET(T, C, E, R) \
    if (SCR_REG_LST(T).empty()) \
        return R; \
    for (SCR_REG_ITR(T) C = SCR_REG_LST(T).begin(); \
        C != SCR_REG_LST(T).end(); ++C)
#define FOREACH_SCRIPT(T) \
    FOR_SCRIPTS(T, itr, end) \
    itr->second

// Utility macros for finding specific scripts.
#define GET_SCRIPT(T, I, V) \
    T* V = ScriptRegistry<T>::GetScriptById(I); \
    if (!V) \
        return;
#define GET_SCRIPT_RET(T, I, V, R) \
    T* V = ScriptRegistry<T>::GetScriptById(I); \
    if (!V) \
        return R;

struct TSpellSummary
{
    uint8 Targets;                                          // set of enum SelectTarget
    uint8 Effects;                                          // set of enum SelectEffect
} *SpellSummary;

ScriptMgr::ScriptMgr()
  : _scriptCount(0), _scheduledScripts(0), _script_loader_callback(nullptr)
{
}

ScriptMgr::~ScriptMgr() { }

ScriptMgr* ScriptMgr::instance()
{
    static ScriptMgr instance;
    return &instance;
}

void ScriptMgr::Initialize()
{
    uint32 oldMSTime = getMSTime();

    LoadDatabase();

    TC_LOG_INFO("server.loading", "Loading C++ scripts");

    FillSpellSummary();

    AddSC_SmartScripts();

    ASSERT(_script_loader_callback,
           "Script loader callback wasn't registered!");

    _script_loader_callback();

#ifdef SCRIPTS
    for (std::string const& scriptName : UnusedScriptNames)
    {
        TC_LOG_ERROR("sql.sql", "ScriptName '%s' exists in database, but no core script found!", scriptName.c_str());
    }
#endif

    TC_LOG_INFO("server.loading", ">> Loaded %u C++ scripts in %u ms", GetScriptCount(), GetMSTimeDiffToNow(oldMSTime));
}

void ScriptMgr::Unload()
{
    #define SCR_CLEAR(T) \
        for (T* scr : SCR_REG_VEC(T)) \
            delete scr; \
        SCR_REG_VEC(T).clear();

    // Clear scripts for every script type.
    SCR_CLEAR(SpellScriptLoader);
    SCR_CLEAR(ServerScript);
    SCR_CLEAR(WorldScript);
    SCR_CLEAR(FormulaScript);
    SCR_CLEAR(WorldMapScript);
    SCR_CLEAR(InstanceMapScript);
    SCR_CLEAR(BattlegroundMapScript);
    SCR_CLEAR(ItemScript);
    SCR_CLEAR(CreatureScript);
    SCR_CLEAR(GameObjectScript);
    SCR_CLEAR(AreaTriggerScript);
    SCR_CLEAR(BattlegroundScript);
    SCR_CLEAR(OutdoorPvPScript);
    SCR_CLEAR(CommandScript);
    SCR_CLEAR(WeatherScript);
    SCR_CLEAR(AuctionHouseScript);
    SCR_CLEAR(ConditionScript);
    SCR_CLEAR(VehicleScript);
    SCR_CLEAR(DynamicObjectScript);
    SCR_CLEAR(TransportScript);
    SCR_CLEAR(AchievementCriteriaScript);
    SCR_CLEAR(PlayerScript);
    SCR_CLEAR(AccountScript);
    SCR_CLEAR(GuildScript);
    SCR_CLEAR(GroupScript);
    SCR_CLEAR(UnitScript);

    #undef SCR_CLEAR

    delete[] SpellSummary;
    delete[] UnitAI::AISpellInfo;
}

void ScriptMgr::LoadDatabase()
{
    sScriptSystemMgr->LoadScriptWaypoints();
}

void ScriptMgr::FillSpellSummary()
{
    UnitAI::FillAISpellInfo();

    SpellSummary = new TSpellSummary[sSpellMgr->GetSpellInfoStoreSize()];

    SpellInfo const* pTempSpell;

    for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
    {
        SpellSummary[i].Effects = 0;
        SpellSummary[i].Targets = 0;

        pTempSpell = sSpellMgr->GetSpellInfo(i);
        // This spell doesn't exist.
        if (!pTempSpell)
            continue;

        for (uint32 j = 0; j < MAX_SPELL_EFFECTS; ++j)
        {
            // Spell targets self.
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SELF-1);

            // Spell targets a single enemy.
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_ENEMY-1);

            // Spell targets AoE at enemy.
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_ENEMY-1);

            // Spell targets an enemy.
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_TARGET_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_DEST_AREA_ENEMY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_DEST_DYNOBJ_ENEMY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_ENEMY-1);

            // Spell targets a single friend (or self).
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_SINGLE_FRIEND-1);

            // Spell targets AoE friends.
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_AOE_FRIEND-1);

            // Spell targets any friend (or self).
            if (pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_TARGET_PARTY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_CASTER_AREA_PARTY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_UNIT_LASTTARGET_AREA_PARTY ||
                pTempSpell->Effects[j].TargetA.GetTarget() == TARGET_SRC_CASTER)
                SpellSummary[i].Targets |= 1 << (SELECT_TARGET_ANY_FRIEND-1);

            // Make sure that this spell includes a damage effect.
            if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
                pTempSpell->Effects[j].Effect == SPELL_EFFECT_INSTAKILL ||
                pTempSpell->Effects[j].Effect == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE ||
                pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEALTH_LEECH)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_DAMAGE-1);

            // Make sure that this spell includes a healing effect (or an apply aura with a periodic heal).
            if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL ||
                pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                pTempSpell->Effects[j].Effect == SPELL_EFFECT_HEAL_MECHANICAL ||
                (pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA  && pTempSpell->Effects[j].ApplyAuraName == 8))
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_HEALING-1);

            // Make sure that this spell applies an aura.
            if (pTempSpell->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA)
                SpellSummary[i].Effects |= 1 << (SELECT_EFFECT_AURA-1);
        }
    }
}

void ScriptMgr::CreateSpellScripts(uint32 spellId, std::list<SpellScript*>& scriptVector)
{
    SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);

    for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        SpellScriptLoader* tmpscript = ScriptRegistry<SpellScriptLoader>::GetScriptById(itr->second);
        if (!tmpscript)
            continue;

        SpellScript* script = tmpscript->GetSpellScript();

        if (!script)
            continue;

        script->_Init(&tmpscript->GetName(), spellId);

        scriptVector.push_back(script);
    }
}

void ScriptMgr::CreateAuraScripts(uint32 spellId, std::list<AuraScript*>& scriptVector)
{
    SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);

    for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        SpellScriptLoader* tmpscript = ScriptRegistry<SpellScriptLoader>::GetScriptById(itr->second);
        if (!tmpscript)
            continue;

        AuraScript* script = tmpscript->GetAuraScript();

        if (!script)
            continue;

        script->_Init(&tmpscript->GetName(), spellId);

        scriptVector.push_back(script);
    }
}

void ScriptMgr::CreateSpellScriptLoaders(uint32 spellId, std::vector<std::pair<SpellScriptLoader*, SpellScriptsContainer::iterator> >& scriptVector)
{
    SpellScriptsBounds bounds = sObjectMgr->GetSpellScriptsBounds(spellId);
    scriptVector.reserve(std::distance(bounds.first, bounds.second));

    for (SpellScriptsContainer::iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        SpellScriptLoader* tmpscript = ScriptRegistry<SpellScriptLoader>::GetScriptById(itr->second);
        if (!tmpscript)
            continue;

        scriptVector.push_back(std::make_pair(tmpscript, itr));
    }
}

void ScriptMgr::OnNetworkStart()
{
    FOREACH_SCRIPT(ServerScript)->OnNetworkStart();
}

void ScriptMgr::OnNetworkStop()
{
    FOREACH_SCRIPT(ServerScript)->OnNetworkStop();
}

void ScriptMgr::OnSocketOpen(std::shared_ptr<WorldSocket> socket)
{
    ASSERT(socket);

    FOREACH_SCRIPT(ServerScript)->OnSocketOpen(socket);
}

void ScriptMgr::OnSocketClose(std::shared_ptr<WorldSocket> socket)
{
    ASSERT(socket);

    FOREACH_SCRIPT(ServerScript)->OnSocketClose(socket);
}

void ScriptMgr::OnPacketReceive(WorldSession* session, WorldPacket const& packet)
{
    if (SCR_REG_LST(ServerScript).empty())
        return;

    WorldPacket copy(packet);
    FOREACH_SCRIPT(ServerScript)->OnPacketReceive(session, copy);
}

void ScriptMgr::OnPacketSend(WorldSession* session, WorldPacket const& packet)
{
    ASSERT(session);

    if (SCR_REG_LST(ServerScript).empty())
        return;

    WorldPacket copy(packet);
    FOREACH_SCRIPT(ServerScript)->OnPacketSend(session, copy);
}

void ScriptMgr::OnOpenStateChange(bool open)
{
    FOREACH_SCRIPT(WorldScript)->OnOpenStateChange(open);
}

void ScriptMgr::OnConfigLoad(bool reload)
{
    FOREACH_SCRIPT(WorldScript)->OnConfigLoad(reload);
}

void ScriptMgr::OnMotdChange(std::string& newMotd)
{
    FOREACH_SCRIPT(WorldScript)->OnMotdChange(newMotd);
}

void ScriptMgr::OnShutdownInitiate(ShutdownExitCode code, ShutdownMask mask)
{
    FOREACH_SCRIPT(WorldScript)->OnShutdownInitiate(code, mask);
}

void ScriptMgr::OnShutdownCancel()
{
    FOREACH_SCRIPT(WorldScript)->OnShutdownCancel();
}

void ScriptMgr::OnWorldUpdate(uint32 diff)
{
    FOREACH_SCRIPT(WorldScript)->OnUpdate(diff);
}

void ScriptMgr::OnHonorCalculation(float& honor, uint8 level, float multiplier)
{
    FOREACH_SCRIPT(FormulaScript)->OnHonorCalculation(honor, level, multiplier);
}

void ScriptMgr::OnGrayLevelCalculation(uint8& grayLevel, uint8 playerLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnGrayLevelCalculation(grayLevel, playerLevel);
}

void ScriptMgr::OnColorCodeCalculation(XPColorChar& color, uint8 playerLevel, uint8 mobLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnColorCodeCalculation(color, playerLevel, mobLevel);
}

void ScriptMgr::OnZeroDifferenceCalculation(uint8& diff, uint8 playerLevel)
{
    FOREACH_SCRIPT(FormulaScript)->OnZeroDifferenceCalculation(diff, playerLevel);
}

void ScriptMgr::OnBaseGainCalculation(uint32& gain, uint8 playerLevel, uint8 mobLevel, ContentLevels content)
{
    FOREACH_SCRIPT(FormulaScript)->OnBaseGainCalculation(gain, playerLevel, mobLevel, content);
}

void ScriptMgr::OnGainCalculation(uint32& gain, Player* player, Unit* unit)
{
    ASSERT(player);
    ASSERT(unit);

    FOREACH_SCRIPT(FormulaScript)->OnGainCalculation(gain, player, unit);
}

void ScriptMgr::OnGroupRateCalculation(float& rate, uint32 count, bool isRaid)
{
    FOREACH_SCRIPT(FormulaScript)->OnGroupRateCalculation(rate, count, isRaid);
}

#define SCR_MAP_BGN(M, V, I, E, C, T) \
    if (V->GetEntry() && V->GetEntry()->T()) \
    { \
        FOR_SCRIPTS(M, I, E) \
        { \
            MapEntry const* C = I->second->GetEntry(); \
            if (!C) \
                continue; \
            if (C->MapID == V->GetId()) \
            {

#define SCR_MAP_END \
                return; \
            } \
        } \
    }

void ScriptMgr::OnCreateMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnCreate(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnCreate((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnCreate((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnDestroyMap(Map* map)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnDestroy(map);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnDestroy((InstanceMap*)map);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnDestroy((BattlegroundMap*)map);
    SCR_MAP_END;
}

void ScriptMgr::OnLoadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnLoadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnLoadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnLoadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnUnloadGridMap(Map* map, GridMap* gmap, uint32 gx, uint32 gy)
{
    ASSERT(map);
    ASSERT(gmap);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnUnloadGridMap(map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnUnloadGridMap((InstanceMap*)map, gmap, gx, gy);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnUnloadGridMap((BattlegroundMap*)map, gmap, gx, gy);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerEnterMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    FOREACH_SCRIPT(PlayerScript)->OnMapChanged(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnPlayerEnter(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnPlayerEnter((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnPlayerEnter((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnPlayerLeaveMap(Map* map, Player* player)
{
    ASSERT(map);
    ASSERT(player);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnPlayerLeave(map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnPlayerLeave((InstanceMap*)map, player);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnPlayerLeave((BattlegroundMap*)map, player);
    SCR_MAP_END;
}

void ScriptMgr::OnMapUpdate(Map* map, uint32 diff)
{
    ASSERT(map);

    SCR_MAP_BGN(WorldMapScript, map, itr, end, entry, IsWorldMap);
        itr->second->OnUpdate(map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(InstanceMapScript, map, itr, end, entry, IsDungeon);
        itr->second->OnUpdate((InstanceMap*)map, diff);
    SCR_MAP_END;

    SCR_MAP_BGN(BattlegroundMapScript, map, itr, end, entry, IsBattleground);
        itr->second->OnUpdate((BattlegroundMap*)map, diff);
    SCR_MAP_END;
}

#undef SCR_MAP_BGN
#undef SCR_MAP_END

InstanceScript* ScriptMgr::CreateInstanceData(InstanceMap* map)
{
    ASSERT(map);

    GET_SCRIPT_RET(InstanceMapScript, map->GetScriptId(), tmpscript, NULL);
    return tmpscript->GetInstanceScript(map);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Item* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(ItemScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnQuestAccept(Player* player, Item* item, Quest const* quest)
{
    ASSERT(player);
    ASSERT(item);
    ASSERT(quest);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, item, quest);
}

bool ScriptMgr::OnItemUse(Player* player, Item* item, SpellCastTargets const& targets)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnUse(player, item, targets);
}

bool ScriptMgr::OnItemExpire(Player* player, ItemTemplate const* proto)
{
    ASSERT(player);
    ASSERT(proto);

    GET_SCRIPT_RET(ItemScript, proto->ScriptId, tmpscript, false);
    return tmpscript->OnExpire(player, proto);
}

bool ScriptMgr::OnItemRemove(Player* player, Item* item)
{
    ASSERT(player);
    ASSERT(item);

    GET_SCRIPT_RET(ItemScript, item->GetScriptId(), tmpscript, false);
    return tmpscript->OnRemove(player, item);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, Creature* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(CreatureScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnGossipHello(Player* player, Creature* creature)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, creature);
}

bool ScriptMgr::OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelect(player, creature, sender, action);
}

bool ScriptMgr::OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(code);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelectCode(player, creature, sender, action, code);
}

bool ScriptMgr::OnQuestAccept(Player* player, Creature* creature, Quest const* quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, creature, quest);
}

bool ScriptMgr::OnQuestSelect(Player* player, Creature* creature, Quest const* quest)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestSelect(player, creature, quest);
}

bool ScriptMgr::OnQuestReward(Player* player, Creature* creature, Quest const* quest, uint32 opt)
{
    ASSERT(player);
    ASSERT(creature);
    ASSERT(quest);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, creature, quest, opt);
}

uint32 ScriptMgr::GetDialogStatus(Player* player, Creature* creature)
{
    ASSERT(player);
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, creature);
}

CreatureAI* ScriptMgr::GetCreatureAI(Creature* creature)
{
    ASSERT(creature);

    GET_SCRIPT_RET(CreatureScript, creature->GetScriptId(), tmpscript, NULL);
    return tmpscript->GetAI(creature);
}

GameObjectAI* ScriptMgr::GetGameObjectAI(GameObject* gameobject)
{
    ASSERT(gameobject);

    GET_SCRIPT_RET(GameObjectScript, gameobject->GetScriptId(), tmpscript, NULL);
    return tmpscript->GetAI(gameobject);
}

void ScriptMgr::OnCreatureUpdate(Creature* creature, uint32 diff)
{
    ASSERT(creature);

    GET_SCRIPT(CreatureScript, creature->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(creature, diff);
}

bool ScriptMgr::OnGossipHello(Player* player, GameObject* go)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnGossipHello(player, go);
}

bool ScriptMgr::OnGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelect(player, go, sender, action);
}

bool ScriptMgr::OnGossipSelectCode(Player* player, GameObject* go, uint32 sender, uint32 action, const char* code)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(code);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    return tmpscript->OnGossipSelectCode(player, go, sender, action, code);
}

bool ScriptMgr::OnQuestAccept(Player* player, GameObject* go, Quest const* quest)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestAccept(player, go, quest);
}

bool ScriptMgr::OnQuestReward(Player* player, GameObject* go, Quest const* quest, uint32 opt)
{
    ASSERT(player);
    ASSERT(go);
    ASSERT(quest);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, false);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->OnQuestReward(player, go, quest, opt);
}

uint32 ScriptMgr::GetDialogStatus(Player* player, GameObject* go)
{
    ASSERT(player);
    ASSERT(go);

    GET_SCRIPT_RET(GameObjectScript, go->GetScriptId(), tmpscript, DIALOG_STATUS_SCRIPTED_NO_STATUS);
    player->PlayerTalkClass->ClearMenus();
    return tmpscript->GetDialogStatus(player, go);
}

void ScriptMgr::OnGameObjectDestroyed(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDestroyed(go, player);
}

void ScriptMgr::OnGameObjectDamaged(GameObject* go, Player* player)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnDamaged(go, player);
}

void ScriptMgr::OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnLootStateChanged(go, state, unit);
}

void ScriptMgr::OnGameObjectStateChanged(GameObject* go, uint32 state)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnGameObjectStateChanged(go, state);
}

void ScriptMgr::OnGameObjectUpdate(GameObject* go, uint32 diff)
{
    ASSERT(go);

    GET_SCRIPT(GameObjectScript, go->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(go, diff);
}

bool ScriptMgr::OnDummyEffect(Unit* caster, uint32 spellId, SpellEffIndex effIndex, GameObject* target)
{
    ASSERT(caster);
    ASSERT(target);

    GET_SCRIPT_RET(GameObjectScript, target->GetScriptId(), tmpscript, false);
    return tmpscript->OnDummyEffect(caster, spellId, effIndex, target);
}

bool ScriptMgr::OnAreaTrigger(Player* player, AreaTriggerEntry const* trigger)
{
    ASSERT(player);
    ASSERT(trigger);

    GET_SCRIPT_RET(AreaTriggerScript, sObjectMgr->GetAreaTriggerScriptId(trigger->id), tmpscript, false);
    return tmpscript->OnTrigger(player, trigger);
}

Battleground* ScriptMgr::CreateBattleground(BattlegroundTypeId /*typeId*/)
{
    /// @todo Implement script-side battlegrounds.
    ABORT();
    return NULL;
}

OutdoorPvP* ScriptMgr::CreateOutdoorPvP(OutdoorPvPData const* data)
{
    ASSERT(data);

    GET_SCRIPT_RET(OutdoorPvPScript, data->ScriptId, tmpscript, NULL);
    return tmpscript->GetOutdoorPvP();
}

std::vector<ChatCommand> ScriptMgr::GetChatCommands()
{
    std::vector<ChatCommand> table;

    FOR_SCRIPTS_RET(CommandScript, itr, end, table)
    {
        std::vector<ChatCommand> cmds = itr->second->GetCommands();
        table.insert(table.end(), cmds.begin(), cmds.end());
    }

    // Sort commands in alphabetical order
    std::sort(table.begin(), table.end(), [](const ChatCommand& a, const ChatCommand&b)
    {
        return strcmp(a.Name, b.Name) < 0;
    });

    return table;
}

void ScriptMgr::OnWeatherChange(Weather* weather, WeatherState state, float grade)
{
    ASSERT(weather);

    GET_SCRIPT(WeatherScript, weather->GetScriptId(), tmpscript);
    tmpscript->OnChange(weather, state, grade);
}

void ScriptMgr::OnWeatherUpdate(Weather* weather, uint32 diff)
{
    ASSERT(weather);

    GET_SCRIPT(WeatherScript, weather->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(weather, diff);
}

void ScriptMgr::OnAuctionAdd(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionAdd(ah, entry);
}

void ScriptMgr::OnAuctionRemove(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionRemove(ah, entry);
}

void ScriptMgr::OnAuctionSuccessful(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionSuccessful(ah, entry);
}

void ScriptMgr::OnAuctionExpire(AuctionHouseObject* ah, AuctionEntry* entry)
{
    ASSERT(ah);
    ASSERT(entry);

    FOREACH_SCRIPT(AuctionHouseScript)->OnAuctionExpire(ah, entry);
}

bool ScriptMgr::OnConditionCheck(Condition const* condition, ConditionSourceInfo& sourceInfo)
{
    ASSERT(condition);

    GET_SCRIPT_RET(ConditionScript, condition->ScriptId, tmpscript, true);
    return tmpscript->OnConditionCheck(condition, sourceInfo);
}

void ScriptMgr::OnInstall(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnInstall(veh);
}

void ScriptMgr::OnUninstall(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnUninstall(veh);
}

void ScriptMgr::OnReset(Vehicle* veh)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnReset(veh);
}

void ScriptMgr::OnInstallAccessory(Vehicle* veh, Creature* accessory)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(accessory);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnInstallAccessory(veh, accessory);
}

void ScriptMgr::OnAddPassenger(Vehicle* veh, Unit* passenger, int8 seatId)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(passenger);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnAddPassenger(veh, passenger, seatId);
}

void ScriptMgr::OnRemovePassenger(Vehicle* veh, Unit* passenger)
{
    ASSERT(veh);
    ASSERT(veh->GetBase()->GetTypeId() == TYPEID_UNIT);
    ASSERT(passenger);

    GET_SCRIPT(VehicleScript, veh->GetBase()->ToCreature()->GetScriptId(), tmpscript);
    tmpscript->OnRemovePassenger(veh, passenger);
}

void ScriptMgr::OnDynamicObjectUpdate(DynamicObject* dynobj, uint32 diff)
{
    ASSERT(dynobj);

    FOR_SCRIPTS(DynamicObjectScript, itr, end)
        itr->second->OnUpdate(dynobj, diff);
}

void ScriptMgr::OnAddPassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddPassenger(transport, player);
}

void ScriptMgr::OnAddCreaturePassenger(Transport* transport, Creature* creature)
{
    ASSERT(transport);
    ASSERT(creature);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnAddCreaturePassenger(transport, creature);
}

void ScriptMgr::OnRemovePassenger(Transport* transport, Player* player)
{
    ASSERT(transport);
    ASSERT(player);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRemovePassenger(transport, player);
}

void ScriptMgr::OnTransportUpdate(Transport* transport, uint32 diff)
{
    ASSERT(transport);

    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnUpdate(transport, diff);
}

void ScriptMgr::OnRelocate(Transport* transport, uint32 waypointId, uint32 mapId, float x, float y, float z)
{
    GET_SCRIPT(TransportScript, transport->GetScriptId(), tmpscript);
    tmpscript->OnRelocate(transport, waypointId, mapId, x, y, z);
}

void ScriptMgr::OnStartup()
{
    FOREACH_SCRIPT(WorldScript)->OnStartup();
}

void ScriptMgr::OnShutdown()
{
    FOREACH_SCRIPT(WorldScript)->OnShutdown();
}

bool ScriptMgr::OnCriteriaCheck(uint32 scriptId, Player* source, Unit* target)
{
    ASSERT(source);
    // target can be NULL.

    GET_SCRIPT_RET(AchievementCriteriaScript, scriptId, tmpscript, false);
    return tmpscript->OnCheck(source, target);
}

// Player
void ScriptMgr::OnPVPKill(Player* killer, Player* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnPVPKill(killer, killed);
}

void ScriptMgr::OnCreatureKill(Player* killer, Creature* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnCreatureKill(killer, killed);
}

void ScriptMgr::OnPlayerKilledByCreature(Creature* killer, Player* killed)
{
    FOREACH_SCRIPT(PlayerScript)->OnPlayerKilledByCreature(killer, killed);
}

void ScriptMgr::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    FOREACH_SCRIPT(PlayerScript)->OnLevelChanged(player, oldLevel);
}

void ScriptMgr::OnPlayerFreeTalentPointsChanged(Player* player, uint32 points)
{
    FOREACH_SCRIPT(PlayerScript)->OnFreeTalentPointsChanged(player, points);
}

void ScriptMgr::OnPlayerTalentsReset(Player* player, bool noCost)
{
    FOREACH_SCRIPT(PlayerScript)->OnTalentsReset(player, noCost);
}

void ScriptMgr::OnPlayerMoneyChanged(Player* player, int32& amount)
{
    FOREACH_SCRIPT(PlayerScript)->OnMoneyChanged(player, amount);
}

void ScriptMgr::OnPlayerMoneyLimit(Player* player, int32 amount)
{
    FOREACH_SCRIPT(PlayerScript)->OnMoneyLimit(player, amount);
}

void ScriptMgr::OnGivePlayerXP(Player* player, uint32& amount, Unit* victim)
{
    FOREACH_SCRIPT(PlayerScript)->OnGiveXP(player, amount, victim);
}

void ScriptMgr::OnPlayerReputationChange(Player* player, uint32 factionID, int32& standing, bool incremental)
{
    FOREACH_SCRIPT(PlayerScript)->OnReputationChange(player, factionID, standing, incremental);
}

void ScriptMgr::OnPlayerDuelRequest(Player* target, Player* challenger)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelRequest(target, challenger);
}

void ScriptMgr::OnPlayerDuelStart(Player* player1, Player* player2)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelStart(player1, player2);
}

void ScriptMgr::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
{
    FOREACH_SCRIPT(PlayerScript)->OnDuelEnd(winner, loser, type);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, receiver);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, group);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* guild)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, guild);
}

void ScriptMgr::OnPlayerChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel)
{
    FOREACH_SCRIPT(PlayerScript)->OnChat(player, type, lang, msg, channel);
}

void ScriptMgr::OnPlayerEmote(Player* player, uint32 emote)
{
    FOREACH_SCRIPT(PlayerScript)->OnEmote(player, emote);
}

void ScriptMgr::OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid)
{
    FOREACH_SCRIPT(PlayerScript)->OnTextEmote(player, textEmote, emoteNum, guid);
}

void ScriptMgr::OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck)
{
    FOREACH_SCRIPT(PlayerScript)->OnSpellCast(player, spell, skipCheck);
}

void ScriptMgr::OnPlayerLogin(Player* player, bool firstLogin)
{
    FOREACH_SCRIPT(PlayerScript)->OnLogin(player, firstLogin);
}

void ScriptMgr::OnPlayerLogout(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnLogout(player);
}

void ScriptMgr::OnPlayerCreate(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnCreate(player);
}

void ScriptMgr::OnPlayerDelete(ObjectGuid guid, uint32 accountId)
{
    FOREACH_SCRIPT(PlayerScript)->OnDelete(guid, accountId);
}

void ScriptMgr::OnPlayerFailedDelete(ObjectGuid guid, uint32 accountId)
{
    FOREACH_SCRIPT(PlayerScript)->OnFailedDelete(guid, accountId);
}

void ScriptMgr::OnPlayerSave(Player* player)
{
    FOREACH_SCRIPT(PlayerScript)->OnSave(player);
}

void ScriptMgr::OnPlayerBindToInstance(Player* player, Difficulty difficulty, uint32 mapid, bool permanent, uint8 extendState)
{
    FOREACH_SCRIPT(PlayerScript)->OnBindToInstance(player, difficulty, mapid, permanent, extendState);
}

void ScriptMgr::OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea)
{
    FOREACH_SCRIPT(PlayerScript)->OnUpdateZone(player, newZone, newArea);
}

void ScriptMgr::OnQuestStatusChange(Player* player, uint32 questId, QuestStatus status)
{
    FOREACH_SCRIPT(PlayerScript)->OnQuestStatusChange(player, questId, status);
}

// Account
void ScriptMgr::OnAccountLogin(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnAccountLogin(accountId);
}

void ScriptMgr::OnFailedAccountLogin(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedAccountLogin(accountId);
}

void ScriptMgr::OnEmailChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnEmailChange(accountId);
}

void ScriptMgr::OnFailedEmailChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedEmailChange(accountId);
}

void ScriptMgr::OnPasswordChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnPasswordChange(accountId);
}

void ScriptMgr::OnFailedPasswordChange(uint32 accountId)
{
    FOREACH_SCRIPT(AccountScript)->OnFailedPasswordChange(accountId);
}

// Guild
void ScriptMgr::OnGuildAddMember(Guild* guild, Player* player, uint8& plRank)
{
    FOREACH_SCRIPT(GuildScript)->OnAddMember(guild, player, plRank);
}

void ScriptMgr::OnGuildRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked)
{
    FOREACH_SCRIPT(GuildScript)->OnRemoveMember(guild, player, isDisbanding, isKicked);
}

void ScriptMgr::OnGuildMOTDChanged(Guild* guild, const std::string& newMotd)
{
    FOREACH_SCRIPT(GuildScript)->OnMOTDChanged(guild, newMotd);
}

void ScriptMgr::OnGuildInfoChanged(Guild* guild, const std::string& newInfo)
{
    FOREACH_SCRIPT(GuildScript)->OnInfoChanged(guild, newInfo);
}

void ScriptMgr::OnGuildCreate(Guild* guild, Player* leader, const std::string& name)
{
    FOREACH_SCRIPT(GuildScript)->OnCreate(guild, leader, name);
}

void ScriptMgr::OnGuildDisband(Guild* guild)
{
    FOREACH_SCRIPT(GuildScript)->OnDisband(guild);
}

void ScriptMgr::OnGuildMemberWitdrawMoney(Guild* guild, Player* player, uint32 &amount, bool isRepair)
{
    FOREACH_SCRIPT(GuildScript)->OnMemberWitdrawMoney(guild, player, amount, isRepair);
}

void ScriptMgr::OnGuildMemberDepositMoney(Guild* guild, Player* player, uint32 &amount)
{
    FOREACH_SCRIPT(GuildScript)->OnMemberDepositMoney(guild, player, amount);
}

void ScriptMgr::OnGuildItemMove(Guild* guild, Player* player, Item* pItem, bool isSrcBank, uint8 srcContainer, uint8 srcSlotId,
            bool isDestBank, uint8 destContainer, uint8 destSlotId)
{
    FOREACH_SCRIPT(GuildScript)->OnItemMove(guild, player, pItem, isSrcBank, srcContainer, srcSlotId, isDestBank, destContainer, destSlotId);
}

void ScriptMgr::OnGuildEvent(Guild* guild, uint8 eventType, ObjectGuid::LowType playerGuid1, ObjectGuid::LowType playerGuid2, uint8 newRank)
{
    FOREACH_SCRIPT(GuildScript)->OnEvent(guild, eventType, playerGuid1, playerGuid2, newRank);
}

void ScriptMgr::OnGuildBankEvent(Guild* guild, uint8 eventType, uint8 tabId, ObjectGuid::LowType playerGuid, uint32 itemOrMoney, uint16 itemStackCount, uint8 destTabId)
{
    FOREACH_SCRIPT(GuildScript)->OnBankEvent(guild, eventType, tabId, playerGuid, itemOrMoney, itemStackCount, destTabId);
}

// Group
void ScriptMgr::OnGroupAddMember(Group* group, ObjectGuid guid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnAddMember(group, guid);
}

void ScriptMgr::OnGroupInviteMember(Group* group, ObjectGuid guid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnInviteMember(group, guid);
}

void ScriptMgr::OnGroupRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, const char* reason)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnRemoveMember(group, guid, method, kicker, reason);
}

void ScriptMgr::OnGroupChangeLeader(Group* group, ObjectGuid newLeaderGuid, ObjectGuid oldLeaderGuid)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnChangeLeader(group, newLeaderGuid, oldLeaderGuid);
}

void ScriptMgr::OnGroupDisband(Group* group)
{
    ASSERT(group);
    FOREACH_SCRIPT(GroupScript)->OnDisband(group);
}

// Unit
void ScriptMgr::OnHeal(Unit* healer, Unit* reciever, uint32& gain)
{
    FOREACH_SCRIPT(UnitScript)->OnHeal(healer, reciever, gain);
}

void ScriptMgr::OnDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    FOREACH_SCRIPT(UnitScript)->OnDamage(attacker, victim, damage);
}

void ScriptMgr::ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage)
{
    FOREACH_SCRIPT(UnitScript)->ModifyPeriodicDamageAurasTick(target, attacker, damage);
}

void ScriptMgr::ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage)
{
    FOREACH_SCRIPT(UnitScript)->ModifyMeleeDamage(target, attacker, damage);
}

void ScriptMgr::ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage)
{
    FOREACH_SCRIPT(UnitScript)->ModifySpellDamageTaken(target, attacker, damage);
}

SpellScriptLoader::SpellScriptLoader(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<SpellScriptLoader>::AddScript(this);
}

ServerScript::ServerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ServerScript>::AddScript(this);
}

WorldScript::WorldScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<WorldScript>::AddScript(this);
}

FormulaScript::FormulaScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<FormulaScript>::AddScript(this);
}

UnitScript::UnitScript(const char* name, bool addToScripts)
    : ScriptObject(name)
{
    ScriptRegistry<UnitScript>::AddScript(this, addToScripts);
}

WorldMapScript::WorldMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<Map>(mapId)
{
    if (GetEntry() && !GetEntry()->IsWorldMap())
        TC_LOG_ERROR("scripts", "WorldMapScript for map %u is invalid.", mapId);

    ScriptRegistry<WorldMapScript>::AddScript(this);
}

InstanceMapScript::InstanceMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<InstanceMap>(mapId)
{
    if (GetEntry() && !GetEntry()->IsDungeon())
        TC_LOG_ERROR("scripts", "InstanceMapScript for map %u is invalid.", mapId);

    ScriptRegistry<InstanceMapScript>::AddScript(this);
}

BattlegroundMapScript::BattlegroundMapScript(const char* name, uint32 mapId)
    : ScriptObject(name), MapScript<BattlegroundMap>(mapId)
{
    if (GetEntry() && !GetEntry()->IsBattleground())
        TC_LOG_ERROR("scripts", "BattlegroundMapScript for map %u is invalid.", mapId);

    ScriptRegistry<BattlegroundMapScript>::AddScript(this);
}

ItemScript::ItemScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ItemScript>::AddScript(this);
}

CreatureScript::CreatureScript(const char* name)
    : UnitScript(name, false)
{
    ScriptRegistry<CreatureScript>::AddScript(this);
}

GameObjectScript::GameObjectScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GameObjectScript>::AddScript(this);
}

AreaTriggerScript::AreaTriggerScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AreaTriggerScript>::AddScript(this);
}

BattlegroundScript::BattlegroundScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<BattlegroundScript>::AddScript(this);
}

OutdoorPvPScript::OutdoorPvPScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<OutdoorPvPScript>::AddScript(this);
}

CommandScript::CommandScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<CommandScript>::AddScript(this);
}

WeatherScript::WeatherScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<WeatherScript>::AddScript(this);
}

AuctionHouseScript::AuctionHouseScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AuctionHouseScript>::AddScript(this);
}

ConditionScript::ConditionScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<ConditionScript>::AddScript(this);
}

VehicleScript::VehicleScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<VehicleScript>::AddScript(this);
}

DynamicObjectScript::DynamicObjectScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<DynamicObjectScript>::AddScript(this);
}

TransportScript::TransportScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<TransportScript>::AddScript(this);
}

AchievementCriteriaScript::AchievementCriteriaScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AchievementCriteriaScript>::AddScript(this);
}

PlayerScript::PlayerScript(const char* name)
    : UnitScript(name, false)
{
    ScriptRegistry<PlayerScript>::AddScript(this);
}

AccountScript::AccountScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<AccountScript>::AddScript(this);
}

GuildScript::GuildScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GuildScript>::AddScript(this);
}

GroupScript::GroupScript(const char* name)
    : ScriptObject(name)
{
    ScriptRegistry<GroupScript>::AddScript(this);
}

// Instantiate static members of ScriptRegistry.
template<class TScript> std::map<uint32, TScript*> ScriptRegistry<TScript>::ScriptPointerList;
template<class TScript> std::vector<TScript*> ScriptRegistry<TScript>::Scripts;
template<class TScript> uint32 ScriptRegistry<TScript>::_scriptIdCounter = 0;

// Specialize for each script type class like so:
template class TC_GAME_API ScriptRegistry<SpellScriptLoader>;
template class TC_GAME_API ScriptRegistry<ServerScript>;
template class TC_GAME_API ScriptRegistry<WorldScript>;
template class TC_GAME_API ScriptRegistry<FormulaScript>;
template class TC_GAME_API ScriptRegistry<WorldMapScript>;
template class TC_GAME_API ScriptRegistry<InstanceMapScript>;
template class TC_GAME_API ScriptRegistry<BattlegroundMapScript>;
template class TC_GAME_API ScriptRegistry<ItemScript>;
template class TC_GAME_API ScriptRegistry<CreatureScript>;
template class TC_GAME_API ScriptRegistry<GameObjectScript>;
template class TC_GAME_API ScriptRegistry<AreaTriggerScript>;
template class TC_GAME_API ScriptRegistry<BattlegroundScript>;
template class TC_GAME_API ScriptRegistry<OutdoorPvPScript>;
template class TC_GAME_API ScriptRegistry<CommandScript>;
template class TC_GAME_API ScriptRegistry<WeatherScript>;
template class TC_GAME_API ScriptRegistry<AuctionHouseScript>;
template class TC_GAME_API ScriptRegistry<ConditionScript>;
template class TC_GAME_API ScriptRegistry<VehicleScript>;
template class TC_GAME_API ScriptRegistry<DynamicObjectScript>;
template class TC_GAME_API ScriptRegistry<TransportScript>;
template class TC_GAME_API ScriptRegistry<AchievementCriteriaScript>;
template class TC_GAME_API ScriptRegistry<PlayerScript>;
template class TC_GAME_API ScriptRegistry<GuildScript>;
template class TC_GAME_API ScriptRegistry<GroupScript>;
template class TC_GAME_API ScriptRegistry<UnitScript>;
template class TC_GAME_API ScriptRegistry<AccountScript>;

// Undefine utility macros.
#undef GET_SCRIPT_RET
#undef GET_SCRIPT
#undef FOREACH_SCRIPT
#undef FOR_SCRIPTS_RET
#undef FOR_SCRIPTS
#undef SCR_REG_LST
#undef SCR_REG_ITR
#undef SCR_REG_MAP
