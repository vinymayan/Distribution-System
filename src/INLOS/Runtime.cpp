#include "INLOS/Runtime.h"

#include "DistributionCore/Domain.h"
#include "DistributionCore/FilterEvaluator.h"
#include "DistributionCore/RewardSelector.h"
#include "INLOS/NewSkillMenu.h"
#include "INLOS/Store.h"
#include "INLOS/Settings.h"
#include "WhoEditThatAPI.h"

#include <ClibUtil/editorID.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>

namespace INLOS
{
    namespace
    {
        RE::FormID ResolveFormID(
            const std::string_view a_editorID,
            const std::string_view a_reference)
        {
            if (!a_editorID.empty()) {
                if (auto* form =
                        RE::TESForm::LookupByEditorID(a_editorID)) {
                    return form->GetFormID();
                }
            }
            const auto separator = a_reference.find('|');
            if (separator == std::string_view::npos) {
                return 0;
            }
            try {
                const auto plugin = std::string(
                    a_reference.substr(0, separator));
                const auto local = static_cast<RE::FormID>(
                    std::stoul(
                        std::string(a_reference.substr(separator + 1)),
                        nullptr,
                        16));
                if (_stricmp(plugin.c_str(), "Dynamic") == 0 ||
                    _stricmp(plugin.c_str(), "Created") == 0) {
                    return local;
                }
                auto* dataHandler = RE::TESDataHandler::GetSingleton();
                return dataHandler ?
                    dataHandler->LookupFormID(local, plugin) :
                    0;
            }
            catch (...) {
                return 0;
            }
        }

        bool MatchesNumeric(
            const float a_value,
            const BlacklistFilter& a_filter)
        {
            switch (a_filter.comparison) {
            case NumericComparison::kGreaterOrEqual:
                return a_value >= a_filter.minimumValue;
            case NumericComparison::kLessOrEqual:
                return a_value <= a_filter.minimumValue;
            case NumericComparison::kEqual:
                return std::abs(a_value - a_filter.minimumValue) <= 0.001f;
            case NumericComparison::kBetween: {
                const auto [minimum, maximum] = std::minmax(
                    a_filter.minimumValue,
                    a_filter.maximumValue);
                return a_value >= minimum && a_value <= maximum;
            }
            default:
                return false;
            }
        }

        bool MatchesLevel(const RE::Actor* a_actor, const Rule& a_rule)
        {
            if (!a_actor) {
                return false;
            }
            const auto level = static_cast<float>(a_actor->GetLevel());
            BlacklistFilter comparison;
            comparison.comparison = a_rule.levelComparison;
            comparison.minimumValue = static_cast<float>(a_rule.level);
            comparison.maximumValue =
                static_cast<float>(a_rule.maximumLevel);
            return MatchesNumeric(level, comparison);
        }

        RE::ActorValue ResolveActorValue(const std::string_view a_name)
        {
            if (a_name.empty()) {
                return RE::ActorValue::kNone;
            }
            const auto name = std::string(a_name);
            return RE::ActorValueList::LookupActorValueByName(name.c_str());
        }

        bool IsVanillaSkill(const RE::ActorValue a_actorValue)
        {
            return a_actorValue >= RE::ActorValue::kOneHanded &&
                a_actorValue <= RE::ActorValue::kEnchanting;
        }

        WhoEditThat::API::IWhoEditThatAPI* GetINLOSLedger(
            WhoEditThat::API::ClientHandle& a_client)
        {
            static auto* api = WhoEditThat::API::GetAPI();
            if (!api) {
                api = WhoEditThat::API::GetAPI();
            }
            if (!api || !api->IsReady()) {
                return nullptr;
            }
            if (a_client == WhoEditThat::API::kInvalidClient) {
                WhoEditThat::API::ClientRegistration registration;
                registration.clientID = "viny.inlos";
                registration.displayName = "INLOS";
                a_client = api->RegisterClient(
                    std::addressof(registration));
            }
            return a_client == WhoEditThat::API::kInvalidClient ?
                nullptr : api;
        }

        void LogINLOSLedgerResult(
            const WhoEditThat::API::Result* a_result,
            void*)
        {
            if (!a_result) {
                return;
            }
            if (a_result->status == WhoEditThat::API::Status::kSuccess) {
                logger::debug(
                    "[INLOS] Skill bonus recorded by WhoEditThat: "
                    "actor={:08X} key='{}' value='{}' delta={:+.3f}.",
                    a_result->actorFormID,
                    a_result->mutationKey,
                    a_result->actorValue,
                    a_result->appliedDelta);
                return;
            }
            logger::warn(
                "[INLOS] WhoEditThat rejected skill bonus: "
                "actor={:08X} status={} message='{}'.",
                a_result->actorFormID,
                std::to_underlying(a_result->status),
                a_result->message);
        }

        bool QueueVanillaSkillBonus(
            RE::Actor* a_actor,
            const RE::ActorValue a_skill,
            const int a_amount)
        {
            static WhoEditThat::API::ClientHandle client =
                WhoEditThat::API::kInvalidClient;
            static std::atomic<std::uint64_t> sequence{ 0 };
            static const auto sessionID = static_cast<std::uint64_t>(
                std::chrono::system_clock::now().time_since_epoch().count());
            auto* api = GetINLOSLedger(client);
            const auto* skillName =
                RE::ActorValueList::GetActorValueName(a_skill);
            if (!a_actor || !api || !skillName || !*skillName ||
                a_amount == 0) {
                return false;
            }

            const auto mutationKey = std::format(
                "loot/skill/{:016X}/{:016X}",
                sessionID,
                sequence.fetch_add(1, std::memory_order_relaxed));
            WhoEditThat::API::ActorValueContributionRequest request;
            request.client = client;
            request.actorFormID = a_actor->GetFormID();
            request.mutationKey = mutationKey.c_str();
            request.targetActorValue = skillName;
            request.operation = WhoEditThat::API::NumericOperation::kFlat;
            request.source = WhoEditThat::API::NumericSource::kFixed;
            request.channel = WhoEditThat::API::ModifierChannel::kPermanent;
            request.fixedValue = static_cast<float>(a_amount);
            return api->QueueUpsertActorValue(
                std::addressof(request),
                LogINLOSLedgerResult,
                nullptr);
        }

        bool AddVanillaPerkPoints(RE::Actor* a_actor, const int a_amount)
        {
            if (!a_actor || !a_actor->IsPlayerRef() || a_amount == 0) {
                return false;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            const auto current = static_cast<int>(
                player->GetPlayerRuntimeData().perkCount);
            player->GetPlayerRuntimeData().perkCount =
                static_cast<std::uint8_t>(
                    std::clamp(current + a_amount, 0, 255));
            return true;
        }

        bool IsHumanoid(RE::Actor* a_actor, RE::TESNPC* a_npc)
        {
            if (a_actor) {
                return a_actor->IsHumanoid();
            }
            auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
            constexpr auto index = static_cast<std::size_t>(
                RE::DEFAULT_OBJECTS::kKeywordNPC);
            auto* keyword = defaults && defaults->objects[index] ?
                defaults->objects[index]->As<RE::BGSKeyword>() :
                nullptr;
            return !keyword || (a_npc && a_npc->HasKeyword(keyword));
        }

        RE::Actor* ResolveSummonOwner(RE::Actor* a_actor)
        {
            auto* current = a_actor;
            for (std::size_t depth = 0;
                 current && current->IsSummoned() && depth < 8;
                 ++depth) {
                const auto commander = current->GetCommandingActor();
                auto* next = commander ? commander.get() : nullptr;
                if (!next || next == current) {
                    break;
                }
                current = next;
            }
            return current;
        }

        bool IsActivePlayerFollowerForLoot(RE::Actor* a_actor)
        {
            if (!a_actor || a_actor->IsPlayerRef() ||
                a_actor->IsSummoned()) {
                return false;
            }
            static auto* currentFollowerFaction =
                []() -> RE::TESFaction* {
                    auto* dataHandler =
                        RE::TESDataHandler::GetSingleton();
                    return dataHandler ?
                        dataHandler->LookupForm<RE::TESFaction>(
                            0x0001CA7D,
                            "Skyrim.esm") :
                        nullptr;
                }();
            return (currentFollowerFaction &&
                       a_actor->IsInFaction(currentFollowerFaction)) ||
                a_actor->IsPlayerTeammate();
        }

        RE::Actor* ResolveConfiguredLootReceiver(RE::Actor* a_instigator)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return nullptr;
            }

            const auto mode =
                Settings::GetSingleton()->lootRecipientMode;
            if (mode == LootRecipientMode::kPlayerOnly) {
                return player;
            }

            auto* receiver = ResolveSummonOwner(a_instigator);
            if (!receiver) {
                return nullptr;
            }
            if (mode == LootRecipientMode::kAnyActor) {
                return receiver;
            }
            return receiver->IsPlayerRef() ||
                    IsActivePlayerFollowerForLoot(receiver) ?
                receiver :
                nullptr;
        }

        RE::Actor* ResolveVanillaLootReceiver(RE::Actor* a_instigator)
        {
            auto* receiver = ResolveSummonOwner(a_instigator);
            if (!receiver) {
                return nullptr;
            }
            const auto* settings = Settings::GetSingleton();
            if (settings->followerVanillaLootToPlayer &&
                !receiver->IsPlayerRef() &&
                IsActivePlayerFollowerForLoot(receiver)) {
                return RE::PlayerCharacter::GetSingleton();
            }
            return receiver;
        }

        void ProcessVanillaLoot(
            RE::Actor* a_subject,
            RE::Actor* a_instigator,
            const Trigger a_trigger)
        {
            if (!a_subject || a_trigger != Trigger::kDeath) {
                return;
            }
            const auto* settings = Settings::GetSingleton();
            if (settings->vanillaLootMode ==
                VanillaLootMode::kDoNothing) {
                return;
            }

            auto* receiver =
                settings->vanillaLootMode ==
                    VanillaLootMode::kAutoLoot ?
                ResolveVanillaLootReceiver(a_instigator) :
                nullptr;
            if (settings->vanillaLootMode ==
                    VanillaLootMode::kAutoLoot &&
                (!receiver || receiver == a_subject)) {
                return;
            }

            struct VanillaStack
            {
                RE::TESBoundObject* item = nullptr;
                std::int32_t count = 0;
                bool questObject = false;
            };
            std::vector<VanillaStack> stacks;
            {
                // InventoryEntryData contains shallow ExtraDataList pointers.
                // Read everything needed before the first inventory mutation,
                // then let the snapshot die. RemoveItem receives no ExtraData
                // pointer and lets the engine move/remove each complete stack.
                const auto inventory = a_subject->GetInventory();
                stacks.reserve(inventory.size());
                for (const auto& [item, entry] : inventory) {
                    if (!item || entry.first <= 0) {
                        continue;
                    }
                    stacks.push_back({
                        item,
                        entry.first,
                        entry.second &&
                            entry.second->IsQuestObject()
                    });
                }
            }

            std::uint32_t affectedStacks = 0;
            for (const auto& stack : stacks) {
                if (settings->vanillaLootMode ==
                        VanillaLootMode::kDiscard &&
                    settings->preserveQuestItemsWhenDiscarding &&
                    stack.questObject) {
                    continue;
                }

                a_subject->RemoveItem(
                    stack.item,
                    stack.count,
                    settings->vanillaLootMode ==
                            VanillaLootMode::kAutoLoot ?
                        RE::ITEM_REMOVE_REASON::kStoreInContainer :
                        RE::ITEM_REMOVE_REASON::kRemove,
                    nullptr,
                    receiver);
                ++affectedStacks;
            }
            logger::debug(
                "[INLOS] Vanilla loot mode {} processed {} stacks on {:08X}.",
                static_cast<int>(settings->vanillaLootMode),
                affectedStacks,
                a_subject->GetFormID());
        }

        bool MatchesActorValue(
            RE::Actor* a_actor,
            const BlacklistFilter& a_filter)
        {
            if (!a_actor) {
                return false;
            }
            const auto value = ResolveActorValue(
                a_filter.actorValueName);
            auto* owner = a_actor->AsActorValueOwner();
            if (value == RE::ActorValue::kNone || !owner) {
                return false;
            }
            float current = 0.0f;
            switch (a_filter.actorValueMode) {
            case ActorValueMode::kCurrent:
                current = owner->GetActorValue(value);
                break;
            case ActorValueMode::kPermanent:
                current = owner->GetPermanentActorValue(value);
                break;
            case ActorValueMode::kMaximum:
                current = a_actor->GetActorValueMax(value);
                break;
            default:
                return false;
            }
            return std::isfinite(current) &&
                MatchesNumeric(current, a_filter);
        }

        bool IsEquipped(
            RE::Actor* a_actor,
            RE::TESBoundObject* a_item)
        {
            if (!a_actor || !a_item) {
                return false;
            }
            if (a_actor->GetEquippedObject(false) == a_item ||
                a_actor->GetEquippedObject(true) == a_item) {
                return true;
            }
            const auto inventory = a_actor->GetInventory();
            const auto found = inventory.find(a_item);
            return found != inventory.end() &&
                found->second.second &&
                found->second.second->IsWorn();
        }

        bool MatchesFilter(
            RE::Actor* a_subject,
            RE::TESNPC* a_npc,
            const BlacklistFilter& a_filter)
        {
            DistributionCore::FilterEvaluationServices services;
            services.resolveFormID =
                [](const std::string_view,
                   const std::string_view a_editorID,
                   const std::string_view a_formID) {
                    return ResolveFormID(a_editorID, a_formID);
                };
            const auto shared =
                DistributionCore::EvaluateFilter(
                    a_subject,
                    a_npc,
                    a_filter,
                    services);
            if (shared !=
                DistributionCore::FilterEvaluation::kNotHandled) {
                return shared ==
                    DistributionCore::FilterEvaluation::kMatch;
            }
            if (a_filter.type == "Actor Value") {
                return MatchesActorValue(a_subject, a_filter);
            }
            if (a_filter.type == "Source Plugin") {
                const auto* file = a_npc ? a_npc->GetFile(0) : nullptr;
                const auto source = file ?
                    std::string(file->GetFilename()) :
                    std::string("Dynamic");
                const auto expected = a_filter.optionText.empty() ?
                    a_filter.editorID :
                    a_filter.optionText;
                return _stricmp(source.c_str(), expected.c_str()) == 0;
            }
            if (a_filter.type == "NPC Trait") {
                if (!a_npc) {
                    return false;
                }
                switch (static_cast<NPCTraitFilter>(
                    a_filter.optionMode)) {
                case NPCTraitFilter::kUnique:
                    return a_npc->IsUnique();
                case NPCTraitFilter::kEssential:
                    return a_subject ?
                        a_subject->IsEssential() :
                        a_npc->IsEssential();
                case NPCTraitFilter::kProtected:
                    return a_subject ?
                        a_subject->IsProtected() :
                        a_npc->IsProtected();
                default:
                    return false;
                }
            }
            if (a_filter.type == "Cell Type") {
                const auto* cell =
                    a_subject ? a_subject->GetParentCell() : nullptr;
                return cell &&
                    (static_cast<CellTypeFilter>(a_filter.optionMode) ==
                            CellTypeFilter::kInterior ?
                        cell->IsInteriorCell() :
                        !cell->IsInteriorCell());
            }
            if (a_filter.type == "Relationship Rank") {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* playerBase =
                    player ? player->GetActorBase() : nullptr;
                const auto* relationship =
                    a_npc && playerBase ?
                    RE::BGSRelationship::GetRelationship(
                        a_npc, playerBase) :
                    nullptr;
                const auto rank = relationship ?
                    4 - static_cast<int>(relationship->level.get()) :
                    -5;
                return MatchesNumeric(
                    static_cast<float>(rank), a_filter);
            }

            const auto formID = ResolveFormID(
                a_filter.editorID,
                a_filter.formIDStr);
            if (a_filter.type != "Gold" && formID == 0) {
                return false;
            }
            if (a_filter.type == "NPC") {
                return a_npc && (
                    a_npc->GetFormID() == formID ||
                    (a_npc->baseTemplateForm &&
                        a_npc->baseTemplateForm->GetFormID() == formID));
            }
            if (a_filter.type == "Keyword") {
                auto* keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(formID);
                return keyword && a_npc && (
                    a_npc->HasKeyword(keyword) ||
                    (a_npc->race && a_npc->race->HasKeyword(keyword)));
            }
            if (a_filter.type == "Faction" ||
                a_filter.type == "Faction Rank") {
                auto* faction =
                    RE::TESForm::LookupByID<RE::TESFaction>(formID);
                if (!a_subject || !faction ||
                    !a_subject->IsInFaction(faction)) {
                    return false;
                }
                return a_filter.type == "Faction" ||
                    MatchesNumeric(
                        static_cast<float>(
                            a_subject->GetFactionRank(
                                faction,
                                a_subject->IsPlayerRef())),
                        a_filter);
            }
            if (a_filter.type == "Perk") {
                auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(formID);
                return a_subject && perk && a_subject->HasPerk(perk);
            }
            if (a_filter.type == "Spell") {
                auto* spell =
                    RE::TESForm::LookupByID<RE::SpellItem>(formID);
                return a_subject && spell && a_subject->HasSpell(spell);
            }
            if (a_filter.type == "Shout") {
                auto* shout =
                    RE::TESForm::LookupByID<RE::TESShout>(formID);
                return a_subject && shout && a_subject->HasShout(shout);
            }
            if (a_filter.type == "Race") {
                return a_npc && a_npc->race &&
                    a_npc->race->GetFormID() == formID;
            }
            if (a_filter.type == "Inventory Item" ||
                a_filter.type == "Inventory Count") {
                auto* item =
                    RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
                if (!a_subject || !item) {
                    return false;
                }
                const auto count = std::max(
                    0, a_subject->GetInventoryCount(item));
                return a_filter.type == "Inventory Item" ?
                    count > 0 :
                    MatchesNumeric(static_cast<float>(count), a_filter);
            }
            if (a_filter.type == "Gold") {
                return a_subject &&
                    MatchesNumeric(
                        static_cast<float>(
                            std::max(0, a_subject->GetGoldAmount())),
                        a_filter);
            }
            if (a_filter.type == "Equipped Item") {
                return IsEquipped(
                    a_subject,
                    RE::TESForm::LookupByID<RE::TESBoundObject>(formID));
            }
            if (a_filter.type == "Combat Style") {
                return a_npc && a_npc->combatStyle &&
                    a_npc->combatStyle->GetFormID() == formID;
            }
            if (a_filter.type == "Voice Type") {
                return a_npc && a_npc->voiceType &&
                    a_npc->voiceType->GetFormID() == formID;
            }
            if (a_filter.type == "Class") {
                return a_npc && a_npc->npcClass &&
                    a_npc->npcClass->GetFormID() == formID;
            }
            if (a_filter.type == "Skin") {
                return a_npc && a_npc->skin &&
                    a_npc->skin->GetFormID() == formID;
            }
            if (a_filter.type == "Location") {
                auto* location =
                    RE::TESForm::LookupByID<RE::BGSLocation>(formID);
                for (auto* current =
                         a_subject ? a_subject->GetCurrentLocation() : nullptr;
                     current;
                     current = current->parentLoc) {
                    if (current == location) {
                        return true;
                    }
                }
                return false;
            }
            if (a_filter.type == "Cell") {
                const auto* cell =
                    a_subject ? a_subject->GetParentCell() : nullptr;
                return cell && cell->GetFormID() == formID;
            }
            if (a_filter.type == "Worldspace") {
                const auto* worldspace =
                    a_subject ? a_subject->GetWorldspace() : nullptr;
                return worldspace &&
                    worldspace->GetFormID() == formID;
            }
            if (a_filter.type == "Location Keyword") {
                auto* keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(formID);
                for (auto* location =
                         a_subject ? a_subject->GetCurrentLocation() : nullptr;
                     keyword && location;
                     location = location->parentLoc) {
                    if (location->HasKeyword(keyword)) {
                        return true;
                    }
                }
                return false;
            }
            if (a_filter.type == "Package") {
                return a_npc &&
                    std::ranges::any_of(
                        a_npc->aiPackages.packages,
                        [formID](const RE::TESPackage* a_package) {
                            return a_package &&
                                a_package->GetFormID() == formID;
                        });
            }
            if (a_filter.type == "Hair" ||
                a_filter.type == "Facial Hair" ||
                a_filter.type.starts_with("HeadPart ")) {
                if (!a_npc || !a_npc->headParts) {
                    return false;
                }
                for (std::int8_t index = 0;
                     index < a_npc->numHeadParts;
                     ++index) {
                    if (a_npc->headParts[index] &&
                        a_npc->headParts[index]->GetFormID() == formID) {
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        bool MatchesFilterSet(
            RE::Actor* a_subject,
            RE::TESNPC* a_npc,
            const std::vector<BlacklistFilter>& a_filters,
            const bool a_requiresAll)
        {
            if (a_filters.empty()) {
                return true;
            }
            const auto predicate = [&](const BlacklistFilter& a_filter) {
                return MatchesFilter(a_subject, a_npc, a_filter);
            };
            return a_requiresAll ?
                std::ranges::all_of(a_filters, predicate) :
                std::ranges::any_of(a_filters, predicate);
        }

        bool MatchesRule(
            RE::Actor* a_subject,
            RE::Actor* a_instigator,
            const LootRule& a_lootRule,
            const Trigger a_trigger)
        {
            const auto& rule = a_lootRule.criteria;
            if (!rule.isEnabled ||
                (a_lootRule.trigger != Trigger::kBoth &&
                    a_lootRule.trigger != a_trigger)) {
                return false;
            }
            if (!a_subject || a_subject->IsPlayerRef()) {
                return false;
            }
            auto* npc = a_subject->GetActorBase();
            if (!npc || !MatchesLevel(a_subject, rule)) {
                return false;
            }
            if (a_lootRule.requirePlayerKiller) {
                auto* killer = ResolveSummonOwner(a_instigator);
                if (!killer || !killer->IsPlayerRef()) {
                    return false;
                }
            }
            if (a_lootRule.destination == Destination::kPlayer &&
                !ResolveConfiguredLootReceiver(a_instigator)) {
                return false;
            }
            if (rule.targetGender != 0) {
                const auto female = npc->IsFemale();
                if ((rule.targetGender == 1 && female) ||
                    (rule.targetGender == 2 && !female)) {
                    return false;
                }
            }
            if (rule.targetHumanoid != 0) {
                const auto humanoid = IsHumanoid(a_subject, npc);
                if ((rule.targetHumanoid == 1 && !humanoid) ||
                    (rule.targetHumanoid == 2 && humanoid)) {
                    return false;
                }
            }
            if (rule.targetChild != 0) {
                const auto child = a_subject->IsChild();
                if ((rule.targetChild == 1 && !child) ||
                    (rule.targetChild == 2 && child)) {
                    return false;
                }
            }
            if (rule.summonedState != RuleSummonedState::kAny) {
                const auto summoned = a_subject->IsSummoned();
                if ((rule.summonedState ==
                        RuleSummonedState::kSummonedOnly &&
                        !summoned) ||
                    (rule.summonedState ==
                        RuleSummonedState::kExcludeSummoned &&
                        summoned)) {
                    return false;
                }
            }
            if (rule.followerState != RuleFollowerState::kAny) {
                const auto follower =
                    !a_subject->IsPlayerRef() &&
                    IsActivePlayerFollowerForLoot(a_subject);
                if ((rule.followerState ==
                        RuleFollowerState::kActiveOnly &&
                        !follower) ||
                    (rule.followerState ==
                        RuleFollowerState::kExcludeActive &&
                        follower)) {
                    return false;
                }
            }
            if (rule.hostilityState != RuleHostilityState::kAny) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                const auto hostile =
                    player && a_subject != player &&
                    a_subject->IsHostileToActor(player);
                if ((rule.hostilityState ==
                        RuleHostilityState::kHostileToPlayer &&
                        !hostile) ||
                    (rule.hostilityState ==
                        RuleHostilityState::kFriendlyOrAlly &&
                        hostile)) {
                    return false;
                }
            }

            for (const auto& filter : rule.targetFilters) {
                if (!DistributionCore::FilterRegistry().Supports(
                        filter.type, DistributionCore::Domain::kINLOS)) {
                    logger::error(
                        "[INLOS] Rule '{}' uses unsupported filter '{}'.",
                        rule.name,
                        filter.type);
                    return false;
                }
                if (filter.type == "City Status" &&
                    (filter.optionMode < 0 || filter.optionMode > 1)) {
                    return false;
                }
            }
            for (const auto& filter : rule.blacklistFilters) {
                if (!DistributionCore::FilterRegistry().Supports(
                        filter.type, DistributionCore::Domain::kINLOS)) {
                    logger::error(
                        "[INLOS] Rule '{}' uses unsupported blacklist filter '{}'.",
                        rule.name,
                        filter.type);
                    return false;
                }
                if (filter.type == "City Status" &&
                    (filter.optionMode < 0 || filter.optionMode > 1)) {
                    return false;
                }
            }
            for (const auto& group : rule.rewardGroups) {
                for (const auto& reward : group.rewards) {
                    if (!DistributionCore::RewardRegistry().Supports(
                            reward.typeReward,
                            DistributionCore::Domain::kINLOS)) {
                        logger::error(
                            "[INLOS] Rule '{}' uses unsupported reward '{}'.",
                            rule.name,
                            reward.typeReward);
                        return false;
                    }
                }
            }
            if (!MatchesFilterSet(
                    a_subject,
                    npc,
                    rule.targetFilters,
                    rule.targetRequiresAll)) {
                return false;
            }

            const auto blacklistScalar =
                (rule.blacklistedGender == 1 && !npc->IsFemale()) ||
                (rule.blacklistedGender == 2 && npc->IsFemale()) ||
                (rule.blacklistedHumanoid == 1 &&
                    IsHumanoid(a_subject, npc)) ||
                (rule.blacklistedHumanoid == 2 &&
                    !IsHumanoid(a_subject, npc)) ||
                (rule.blacklistedChild == 1 && a_subject->IsChild()) ||
                (rule.blacklistedChild == 2 && !a_subject->IsChild());
            if (blacklistScalar) {
                return false;
            }
            if (!rule.blacklistFilters.empty() &&
                MatchesFilterSet(
                    a_subject,
                    npc,
                    rule.blacklistFilters,
                    rule.blacklistRequiresAll)) {
                return false;
            }
            return true;
        }

        float RandomPercent()
        {
            static thread_local std::mt19937 generator{
                std::random_device{}()
            };
            return std::uniform_real_distribution<float>(
                0.0f, 100.0f)(generator);
        }

        std::vector<const Reward*> SelectRewards(const LootRule& a_rule)
        {
            std::vector<const Reward*> result;
            for (const auto& selection :
                 DistributionCore::RollRuleRewards(
                     a_rule.criteria,
                     [] { return RandomPercent(); })) {
                const auto& group =
                    a_rule.criteria.rewardGroups[
                        selection.groupIndex];
                for (const auto rewardIndex :
                     selection.rewardIndices) {
                    result.push_back(std::addressof(
                        group.rewards[rewardIndex]));
                }
            }
            return result;
        }

        void DispatchExperience(
            const float a_amount,
            const float a_total)
        {
            auto* source = SKSE::GetModCallbackEventSource();
            if (!source) {
                return;
            }
            const auto total = std::format("{:.2f}", a_total);
            SKSE::ModCallbackEvent event{
                RE::BSFixedString("INLOSExperienceGained"),
                RE::BSFixedString(total.c_str()),
                a_amount,
                RE::PlayerCharacter::GetSingleton()
            };
            source->SendEvent(std::addressof(event));
        }

        RE::TESObjectBOOK* FindSpellTome(RE::SpellItem* a_spell)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!a_spell || !dataHandler) {
                return nullptr;
            }
            for (auto* book :
                 dataHandler->GetFormArray<RE::TESObjectBOOK>()) {
                if (book && book->TeachesSpell() &&
                    book->GetSpell() == a_spell) {
                    return book;
                }
            }
            return nullptr;
        }

        std::string SanitizeBookEditorIDPart(
            const std::string_view a_value)
        {
            std::string result;
            result.reserve(a_value.size());
            for (const auto character : a_value) {
                result.push_back(
                    std::isalnum(
                        static_cast<unsigned char>(character)) ?
                    character :
                    '_');
            }
            return result;
        }

        std::string BuildBookOfPerksEditorID(
            const std::string_view a_pluginName,
            const std::string_view a_perkIdentifier)
        {
            auto pluginPart =
                SanitizeBookEditorIDPart(a_pluginName);
            auto perkPart =
                SanitizeBookEditorIDPart(a_perkIdentifier);
            if (pluginPart.empty()) {
                pluginPart = "UnknownPlugin";
            }
            if (perkPart.empty()) {
                perkPart = "0";
            }

            auto editorID = std::format(
                "BoP_Learn_{}_{}",
                pluginPart,
                perkPart);
            constexpr std::size_t kMaximumEditorIDLength = 127;
            if (editorID.size() <= kMaximumEditorIDLength) {
                return editorID;
            }

            std::uint32_t hash = 2166136261u;
            for (const auto character : editorID) {
                hash ^= static_cast<unsigned char>(character);
                hash *= 16777619u;
            }
            const auto hashSuffix =
                std::format("_{:08X}", hash);
            editorID.resize(
                kMaximumEditorIDLength - hashSuffix.size());
            editorID += hashSuffix;
            return editorID;
        }

        std::string MakeBookOfPerksEditorID(RE::BGSPerk* a_perk)
        {
            if (!a_perk) {
                return {};
            }
            const auto* file = a_perk->GetFile(0);
            if (!file || file->GetFilename().empty()) {
                return {};
            }
            const std::string_view pluginName = file->GetFilename();
            try {
                const auto perkEditorID =
                    clib_util::editorID::get_editorID(a_perk);
                if (!perkEditorID.empty()) {
                    return BuildBookOfPerksEditorID(
                        pluginName,
                        perkEditorID);
                }
            }
            catch (...) {
                // Book of Perks falls back to the local FormID when the perk
                // has no usable EditorID. Mirror that behavior below.
            }

            const auto formID = a_perk->GetFormID();
            const auto localID =
                (formID & 0xFF000000) == 0xFE000000 ?
                (formID & 0x00000FFF) :
                (formID & 0x00FFFFFF);
            return BuildBookOfPerksEditorID(
                pluginName,
                std::format("{:X}", localID));
        }

        bool GiveBook(
            RE::Actor* a_destination,
            RE::TESObjectBOOK* a_book,
            const std::uint32_t a_amount)
        {
            if (!a_destination || !a_book) {
                return false;
            }
            a_destination->AddObjectToContainer(
                a_book,
                nullptr,
                static_cast<std::int32_t>(
                    std::max<std::uint32_t>(1, a_amount)),
                nullptr);
            return true;
        }

        void ApplyReward(
            RE::Actor* a_subject,
            RE::Actor* a_instigator,
            const LootRule& a_rule,
            const Reward& a_reward)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!a_subject || !player) {
                return;
            }
            if (!DistributionCore::RewardRegistry().Supports(
                    a_reward.typeReward,
                    DistributionCore::Domain::kINLOS)) {
                logger::error(
                    "[INLOS] Rule '{}' uses unsupported reward '{}'.",
                    a_rule.criteria.name,
                    a_reward.typeReward);
                return;
            }
            const auto nonPhysicalReward =
                a_reward.typeReward == "Experience" ||
                a_reward.typeReward == "Skill XP" ||
                a_reward.typeReward == "Skill Level" ||
                a_reward.typeReward == "Perk Points" ||
                a_reward.typeReward == "NSM Resource";
            auto* progressionReceiver =
                nonPhysicalReward ?
                    ResolveConfiguredLootReceiver(a_instigator) :
                    nullptr;
            if (nonPhysicalReward && !progressionReceiver) {
                logger::debug(
                    "[INLOS] Non-physical reward '{}' from rule '{}' "
                    "has no receiver allowed by the current settings.",
                    a_reward.typeReward,
                    a_rule.criteria.name);
                return;
            }
            if (a_reward.typeReward == "Experience") {
                if (!progressionReceiver->IsPlayerRef()) {
                    logger::debug(
                        "[INLOS] General experience is player-only; "
                        "receiver {:08X} was ignored.",
                        progressionReceiver->GetFormID());
                    return;
                }
                const auto amount =
                    static_cast<float>(a_reward.amount) *
                    Settings::GetSingleton()->experienceMultiplier;
                State::GetSingleton()->AddExperience(amount);
                DispatchExperience(
                    amount,
                    State::GetSingleton()->GetExperience());
                return;
            }
            if (a_reward.typeReward == "Skill XP") {
                const auto source = GetSkillSource(a_reward.editorID);
                const auto skillID = GetSkillID(a_reward.editorID);
                const auto amount =
                    static_cast<float>(a_reward.amount) *
                    Settings::GetSingleton()->
                        experienceMultiplier;
                if (source == SkillSource::kVanilla) {
                    const auto actorValue = ResolveActorValue(skillID);
                    if (!IsVanillaSkill(actorValue)) {
                        logger::warn(
                            "[INLOS] Vanilla skill '{}' is invalid.",
                            skillID);
                        return;
                    }

                    const auto nsmSkillID = GetNSMSkillID(skillID);
                    const auto appliedByNSM =
                        NewSkillMenu::HasSkill(nsmSkillID) &&
                        NewSkillMenu::AddSkillExperience(
                            progressionReceiver->GetFormID(),
                            nsmSkillID,
                            amount);
                    if (!appliedByNSM && progressionReceiver->IsPlayerRef()) {
                        player->AddSkillExperience(actorValue, amount);
                    } else if (!appliedByNSM) {
                        logger::warn(
                            "[INLOS] Vanilla skill XP for actor {:08X} "
                            "requires skill '{}' to be registered by NSM.",
                            progressionReceiver->GetFormID(),
                            nsmSkillID);
                    }
                } else if (source != SkillSource::kNSM ||
                    !NewSkillMenu::AddSkillExperience(
                        progressionReceiver->GetFormID(), skillID, amount)) {
                    logger::warn(
                        "[INLOS] Could not give {} XP to skill '{}' "
                        "for actor {:08X}.",
                        amount,
                        a_reward.editorID,
                        progressionReceiver->GetFormID());
                }
                return;
            }
            if (a_reward.typeReward == "Skill Level") {
                const auto source = GetSkillSource(a_reward.editorID);
                const auto skillID = GetSkillID(a_reward.editorID);
                const auto amount = static_cast<int>(std::min<std::uint32_t>(
                    a_reward.amount,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<int>::max())));
                const auto mode = static_cast<SkillLevelMode>(
                    a_reward.functionOnType);
                bool applied = false;
                if (source == SkillSource::kVanilla) {
                    const auto actorValue = ResolveActorValue(skillID);
                    if (IsVanillaSkill(actorValue)) {
                        if (mode == SkillLevelMode::kBonus) {
                            applied = QueueVanillaSkillBonus(
                                progressionReceiver, actorValue, amount);
                        } else if (auto* owner =
                                       progressionReceiver->AsActorValueOwner()) {
                            owner->SetBaseActorValue(
                                actorValue,
                                owner->GetBaseActorValue(actorValue) + amount);
                            applied = true;
                        }
                    }
                } else if (source == SkillSource::kNSM) {
                    applied = mode == SkillLevelMode::kBonus ?
                        NewSkillMenu::AddSkillBonus(
                            progressionReceiver->GetFormID(), skillID, amount) :
                        NewSkillMenu::AddSkillLevel(
                            progressionReceiver->GetFormID(), skillID, amount);
                }
                if (!applied) {
                    logger::warn(
                        "[INLOS] Could not add {} {} levels to skill '{}' "
                        "for actor {:08X}.",
                        amount,
                        mode == SkillLevelMode::kBonus ? "bonus" : "base",
                        a_reward.editorID,
                        progressionReceiver->GetFormID());
                }
                return;
            }
            if (a_reward.typeReward == "Perk Points") {
                const auto amount = static_cast<int>(std::min<std::uint32_t>(
                    a_reward.amount,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<int>::max())));
                if (!NewSkillMenu::AddPerkPoints(
                        progressionReceiver->GetFormID(), amount) &&
                    !AddVanillaPerkPoints(progressionReceiver, amount)) {
                    logger::warn(
                        "[INLOS] Could not add {} perk points "
                        "for actor {:08X}.",
                        amount,
                        progressionReceiver->GetFormID());
                }
                return;
            }
            if (a_reward.typeReward == "NSM Resource") {
                if (!NewSkillMenu::AddResource(
                        progressionReceiver->GetFormID(),
                        a_reward.editorID,
                        static_cast<float>(a_reward.amount))) {
                    logger::warn(
                        "[INLOS] Could not add {} to NSM resource '{}' "
                        "for actor {:08X}.",
                        a_reward.amount,
                        a_reward.editorID,
                        progressionReceiver->GetFormID());
                }
                return;
            }

            auto* destination =
                a_rule.destination == Destination::kPlayer ?
                ResolveConfiguredLootReceiver(a_instigator) :
                a_subject;
            if (!destination) {
                return;
            }
            auto formID = ResolveFormID(
                a_reward.editorID,
                a_reward.formIDStr);
            if (a_reward.typeReward == "Gold" && formID == 0) {
                formID = 0x0000000F;
            }
            if (a_reward.typeReward == "Leveled Item") {
                auto* list =
                    RE::TESForm::LookupByID<RE::TESLevItem>(formID);
                for (const auto& resolved :
                     DistributionCore::ResolveLeveledItems(
                         a_subject,
                         list,
                         std::max<std::uint32_t>(1, a_reward.amount))) {
                    destination->AddObjectToContainer(
                        resolved.item,
                        nullptr,
                        static_cast<std::int32_t>(resolved.count),
                        nullptr);
                }
                return;
            }
            if (a_reward.typeReward == "Spell") {
                auto* spell =
                    RE::TESForm::LookupByID<RE::SpellItem>(formID);
                if (!spell) {
                    return;
                }
                const auto tangibleLoot =
                    destination == a_subject &&
                    a_subject->IsDead();
                if (!tangibleLoot &&
                    !destination->HasSpell(spell)) {
                    destination->AddSpell(spell);
                    return;
                }
                if (!tangibleLoot &&
                    !Settings::GetSingleton()->
                        giveSpellTomeWhenKnown) {
                    return;
                }
                if (!GiveBook(
                        destination,
                        FindSpellTome(spell),
                        a_reward.amount)) {
                    logger::warn(
                        "[INLOS] No spell tome found for reward {:08X}.",
                        formID);
                }
                return;
            }
            if (a_reward.typeReward == "Perk") {
                auto* perk =
                    RE::TESForm::LookupByID<RE::BGSPerk>(formID);
                if (!perk) {
                    return;
                }
                const auto tangibleLoot =
                    destination == a_subject &&
                    a_subject->IsDead();
                if (!tangibleLoot &&
                    !destination->HasPerk(perk)) {
                    destination->AddPerk(perk);
                    return;
                }
                if (!tangibleLoot &&
                    !Settings::GetSingleton()->
                        givePerkBookWhenOwned) {
                    return;
                }
                const auto editorID =
                    MakeBookOfPerksEditorID(perk);
                auto* book = editorID.empty() ?
                    nullptr :
                    RE::TESForm::LookupByEditorID<
                        RE::TESObjectBOOK>(editorID);
                if (!GiveBook(
                        destination,
                        book,
                        a_reward.amount)) {
                    logger::warn(
                        "[INLOS] Book of Perks form '{}' is unavailable "
                        "for perk {:08X}.",
                        editorID,
                        formID);
                }
                return;
            }
            auto* item =
                RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
            if (item) {
                destination->AddObjectToContainer(
                    item,
                    nullptr,
                    static_cast<std::int32_t>(
                        std::max<std::uint32_t>(1, a_reward.amount)),
                    nullptr);
            }
        }

        void Evaluate(
            RE::Actor* a_subject,
            RE::Actor* a_instigator,
            const Trigger a_trigger)
        {
            if (!a_subject) {
                return;
            }
            std::vector<std::string> applied;
            if (!State::GetSingleton()->BeginEvent(
                    a_subject->GetFormID(),
                    a_trigger,
                    applied)) {
                return;
            }
            const std::set<std::string> previouslyApplied(
                applied.begin(), applied.end());
            ProcessVanillaLoot(
                a_subject,
                a_instigator,
                a_trigger);
            for (const auto& rule : Store::GetSingleton()->Rules()) {
                if (previouslyApplied.contains(rule.criteria.id) ||
                    !MatchesRule(
                        a_subject,
                        a_instigator,
                        rule,
                        a_trigger)) {
                    continue;
                }
                for (const auto* reward : SelectRewards(rule)) {
                    if (reward) {
                        ApplyReward(
                            a_subject,
                            a_instigator,
                            rule,
                            *reward);
                    }
                }
                State::GetSingleton()->CompleteRule(
                    a_subject->GetFormID(),
                    rule.criteria.id);
                logger::info(
                    "[INLOS] Applied '{}' to '{}'.",
                    rule.criteria.name,
                    a_subject->GetName());
            }
        }
    }

    State* State::GetSingleton()
    {
        static State singleton;
        return std::addressof(singleton);
    }

    bool State::BeginEvent(
        const RE::FormID a_actorID,
        const Trigger a_trigger,
        std::vector<std::string>& a_previouslyApplied)
    {
        std::scoped_lock lock(_lock);
        auto& state = _actors[a_actorID];
        if (a_trigger == Trigger::kDeath) {
            if (state.deathProcessed) {
                return false;
            }
            state.dead = true;
            state.deathProcessed = true;
        }
        else {
            if (state.defeatProcessed) {
                return false;
            }
            state.defeatProcessed = true;
        }
        a_previouslyApplied.assign(
            state.appliedRuleIDs.begin(),
            state.appliedRuleIDs.end());
        return true;
    }

    void State::CompleteRule(
        const RE::FormID a_actorID,
        const std::string_view a_ruleID)
    {
        std::scoped_lock lock(_lock);
        _actors[a_actorID].appliedRuleIDs.emplace(a_ruleID);
    }

    void State::MarkAlive(const RE::FormID a_actorID)
    {
        std::scoped_lock lock(_lock);
        const auto found = _actors.find(a_actorID);
        if (found == _actors.end()) {
            return;
        }
        auto& state = found->second;
        if (state.dead || state.defeatProcessed) {
            ++state.generation;
            state.dead = false;
            state.deathProcessed = false;
            state.defeatProcessed = false;
            state.appliedRuleIDs.clear();
        }
    }

    void State::MarkLoadedAlive(const RE::FormID a_actorID)
    {
        std::scoped_lock lock(_lock);
        const auto found = _actors.find(a_actorID);
        if (found == _actors.end() || !found->second.dead) {
            return;
        }
        auto& state = found->second;
        ++state.generation;
        state.dead = false;
        state.deathProcessed = false;
        state.defeatProcessed = false;
        state.appliedRuleIDs.clear();
    }

    void State::Revert()
    {
        std::scoped_lock lock(_lock);
        _actors.clear();
        _experience = 0.0f;
    }

    void State::AddExperience(const float a_amount)
    {
        if (!std::isfinite(a_amount) || a_amount <= 0.0f) {
            return;
        }
        std::scoped_lock lock(_lock);
        _experience += a_amount;
    }

    float State::GetExperience() const
    {
        std::scoped_lock lock(_lock);
        return _experience;
    }

    DeathEventHandler* DeathEventHandler::GetSingleton()
    {
        static DeathEventHandler singleton;
        return std::addressof(singleton);
    }

    void DeathEventHandler::Register()
    {
        if (auto* source =
                RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<
                RE::TESDeathEvent>(GetSingleton());
            source->AddEventSink<
                RE::TESObjectLoadedEvent>(GetSingleton());
        }
    }

    RE::BSEventNotifyControl DeathEventHandler::ProcessEvent(
        const RE::TESDeathEvent* a_event,
        RE::BSTEventSource<RE::TESDeathEvent>*)
    {
        if (!a_event || !a_event->dead) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (!Settings::GetSingleton()->enableDeath) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* subjectRef = a_event->actorDying.get();
        auto* killerRef = a_event->actorKiller.get();
        auto* subject = subjectRef ?
            subjectRef->As<RE::Actor>() :
            nullptr;
        auto* killer = killerRef ?
            killerRef->As<RE::Actor>() :
            nullptr;
        if (subject) {
            QueueEvaluation(
                subject->GetHandle(),
                killer ? killer->GetHandle() : RE::ActorHandle{},
                Trigger::kDeath);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl DeathEventHandler::ProcessEvent(
        const RE::TESObjectLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESObjectLoadedEvent>*)
    {
        if (!a_event || !a_event->loaded) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(
            a_event->formID);
        if (actor && !actor->IsDead()) {
            State::GetSingleton()->MarkLoadedAlive(a_event->formID);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    DefeatEventHandler* DefeatEventHandler::GetSingleton()
    {
        static DefeatEventHandler singleton;
        return std::addressof(singleton);
    }

    void DefeatEventHandler::Register()
    {
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            source->AddEventSink(GetSingleton());
        }
    }

    RE::BSEventNotifyControl DefeatEventHandler::ProcessEvent(
        const SKSE::ModCallbackEvent* a_event,
        RE::BSTEventSource<SKSE::ModCallbackEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (_stricmp(
                a_event->eventName.c_str(),
                "INLOSActorRecovered") == 0) {
            auto* actor = a_event->sender ?
                a_event->sender->As<RE::Actor>() :
                nullptr;
            if (actor) {
                State::GetSingleton()->MarkAlive(actor->GetFormID());
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        if (_stricmp(
                a_event->eventName.c_str(),
                "INLOSActorDefeated") != 0) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (!Settings::GetSingleton()->enableDefeat) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* subject = a_event->sender ?
            a_event->sender->As<RE::Actor>() :
            nullptr;
        RE::Actor* instigator = nullptr;
        const auto arguments =
            std::string(a_event->strArg.c_str());
        const auto separator = arguments.find('|');
        const auto victimArgument =
            arguments.substr(0, separator);
        if (!subject && !victimArgument.empty()) {
            try {
                subject = RE::TESForm::LookupByID<RE::Actor>(
                    static_cast<RE::FormID>(
                        std::stoul(
                            victimArgument,
                            nullptr,
                            16)));
            }
            catch (...) {
                subject = nullptr;
            }
        }
        if (separator != std::string::npos &&
            separator + 1 < arguments.size()) {
            try {
                instigator = RE::TESForm::LookupByID<RE::Actor>(
                    static_cast<RE::FormID>(
                        std::stoul(
                            arguments.substr(separator + 1),
                            nullptr,
                            16)));
            }
            catch (...) {
                instigator = nullptr;
            }
        }
        if (subject) {
            QueueEvaluation(
                subject->GetHandle(),
                instigator ?
                    instigator->GetHandle() :
                    RE::ActorHandle{},
                Trigger::kDefeat);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    void QueueEvaluation(
        const RE::ActorHandle a_subject,
        const RE::ActorHandle a_instigator,
        const Trigger a_trigger)
    {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks || !a_subject) {
            return;
        }
        tasks->AddTask(
            [a_subject, a_instigator, a_trigger] {
                const auto subject = a_subject.get();
                const auto instigator = a_instigator.get();
                auto* subjectActor = subject ?
                    subject.get()->As<RE::Actor>() :
                    nullptr;
                auto* instigatorActor = instigator ?
                    instigator.get()->As<RE::Actor>() :
                    nullptr;
                if (!subjectActor) {
                    return;
                }
                if (a_trigger == Trigger::kDeath &&
                    !subjectActor->IsDead()) {
                    return;
                }
                Evaluate(subjectActor, instigatorActor, a_trigger);
            });
    }
}
