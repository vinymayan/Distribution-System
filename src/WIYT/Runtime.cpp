#include "WIYT/Runtime.h"

#include "DistributionCore/Domain.h"
#include "DistributionCore/FilterEvaluator.h"
#include "DistributionCore/RewardSelector.h"
#include "INLOS/NewSkillMenu.h"
#include "Manager.h"
#include "WIYT/DFGBridge.h"
#include "WIYT/Settings.h"
#include "WIYT/State.h"
#include "WIYT/Store.h"

#include <atomic>
#include <cmath>
#include <random>
#include <sstream>

namespace WIYT
{
    namespace
    {
        constexpr std::size_t kActivityCount =
            static_cast<std::size_t>(ActivityType::kCustom) + 1;

        struct RequirementIndexEntry
        {
            std::string titleID;
            std::string requirementID;
        };

        std::array<
            std::vector<RequirementIndexEntry>,
            kActivityCount> g_eventIndex;
        std::mutex g_runtimeLock;
        std::unordered_map<std::string, float> g_statisticCache;
        std::unordered_map<RE::FormID, std::int32_t>
            g_suppressedAcquisitions;
        struct RecentHitSource
        {
            RE::FormID formID = 0;
            std::chrono::steady_clock::time_point timestamp{};
        };
        std::unordered_map<std::uint64_t, RecentHitSource>
            g_recentHitSources;
        std::chrono::steady_clock::time_point g_lastStatisticRefresh{};
        std::atomic_bool g_statisticFetchInFlight{ false };
        std::atomic_bool g_damageTrackingEnabled{ false };
        std::atomic_bool g_spellDamageTrackingEnabled{ false };
        std::atomic_uint64_t g_eventSequence{ 1 };

        std::uint64_t StableUniqueKey(
            const std::string_view a_value)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const unsigned char character : a_value) {
                hash ^= character;
                hash *= 1099511628211ull;
            }
            return hash == 0 ? 1 : hash;
        }

        RE::FormID ResolveFormID(
            const std::string_view,
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
                if (a_reference.empty()) {
                    return 0;
                }
                try {
                    return static_cast<RE::FormID>(
                        std::stoul(
                            std::string(a_reference),
                            nullptr,
                            16));
                }
                catch (...) {
                    return 0;
                }
            }
            try {
                const auto plugin =
                    std::string(a_reference.substr(0, separator));
                const auto local = static_cast<RE::FormID>(
                    std::stoul(
                        std::string(
                            a_reference.substr(separator + 1)),
                        nullptr,
                        16));
                if (_stricmp(plugin.c_str(), "Dynamic") == 0 ||
                    _stricmp(plugin.c_str(), "Created") == 0) {
                    return local;
                }
                auto* dataHandler =
                    RE::TESDataHandler::GetSingleton();
                return dataHandler ?
                    dataHandler->LookupFormID(local, plugin) :
                    0;
            }
            catch (...) {
                return 0;
            }
        }

        RE::Actor* ResolveSummonOwner(RE::Actor* a_actor)
        {
            auto* current = a_actor;
            for (std::size_t depth = 0;
                 current && current->IsSummoned() && depth < 8;
                 ++depth) {
                const auto commander =
                    current->GetCommandingActor();
                auto* next = commander ? commander.get() : nullptr;
                if (!next || next == current) {
                    break;
                }
                current = next;
            }
            return current;
        }

        bool IsActivePlayerFollower(RE::Actor* a_actor)
        {
            if (!a_actor || a_actor->IsPlayerRef() ||
                a_actor->IsSummoned()) {
                return false;
            }
            static auto* followerFaction =
                []() -> RE::TESFaction* {
                    auto* dataHandler =
                        RE::TESDataHandler::GetSingleton();
                    return dataHandler ?
                        dataHandler->LookupForm<RE::TESFaction>(
                            0x0001CA7D,
                            "Skyrim.esm") :
                        nullptr;
                }();
            return (followerFaction &&
                       a_actor->IsInFaction(followerFaction)) ||
                a_actor->IsPlayerTeammate();
        }

        RE::Actor* ResolveCreditedActor(RE::Actor* a_actor)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !a_actor) {
                return nullptr;
            }
            const auto* settings = Settings::GetSingleton();
            if (a_actor->IsSummoned() &&
                !settings->creditSummonActions) {
                return nullptr;
            }
            auto* owner = ResolveSummonOwner(a_actor);
            if (!owner) {
                return nullptr;
            }
            if (owner->IsPlayerRef()) {
                return player;
            }
            if (settings->creditFollowerActions &&
                IsActivePlayerFollower(owner)) {
                return player;
            }
            return nullptr;
        }

        std::uint64_t DamagePairKey(
            const RE::FormID a_attacker,
            const RE::FormID a_target)
        {
            return
                (static_cast<std::uint64_t>(a_attacker) << 32) |
                a_target;
        }

        void QueueHealthDamageProgress(
            RE::Actor* a_target,
            RE::Actor* a_attacker,
            const float a_damage)
        {
            if (!g_damageTrackingEnabled.load() ||
                !a_target || !a_attacker ||
                !std::isfinite(a_damage) ||
                a_damage <= 0.0f ||
                !ResolveCreditedActor(a_attacker)) {
                return;
            }
            const auto targetID = a_target->GetFormID();
            const auto attackerID = a_attacker->GetFormID();
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                return;
            }
            tasks->AddTask(
                [targetID, attackerID, a_damage]() {
                    auto* target =
                        RE::TESForm::LookupByID<RE::Actor>(
                            targetID);
                    auto* attacker =
                        RE::TESForm::LookupByID<RE::Actor>(
                            attackerID);
                    auto* credited =
                        ResolveCreditedActor(attacker);
                    if (!target || !attacker || !credited) {
                        return;
                    }
                    RE::TESForm* sourceForm = nullptr;
                    {
                        std::scoped_lock lock(g_runtimeLock);
                        const auto found =
                            g_recentHitSources.find(
                                DamagePairKey(
                                    attackerID,
                                    targetID));
                        if (found != g_recentHitSources.end() &&
                            std::chrono::steady_clock::now() -
                                    found->second.timestamp <
                                std::chrono::seconds(5)) {
                            sourceForm =
                                RE::TESForm::LookupByID(
                                    found->second.formID);
                        }
                    }
                    ReportProgressEvent({
                        ActivityType::kDamageDealt,
                        credited,
                        attacker,
                        target,
                        sourceForm,
                        attacker->GetParentCell(),
                        attacker->GetCurrentLocation(),
                        a_damage,
                        0,
                        EventProvenance::kGameplay
                    });
                });
        }

        template <std::size_t Index>
        struct HealthDamageHook
        {
            static void Thunk(
                RE::Actor* a_target,
                RE::Actor* a_attacker,
                const float a_damage)
            {
                Original(a_target, a_attacker, a_damage);
                QueueHealthDamageProgress(
                    a_target,
                    a_attacker,
                    a_damage);
            }

            static inline REL::Relocation<decltype(Thunk)> Original;
        };

        template <std::size_t Index>
        void InstallHealthDamageHook(
            const REL::VariantID a_vtable)
        {
            REL::Relocation<std::uintptr_t> vtable{ a_vtable };
            HealthDamageHook<Index>::Original =
                vtable.write_vfunc(
                    REL::Relocate(0x104, 0x104, 0x106),
                    HealthDamageHook<Index>::Thunk);
        }

        bool MatchActorFilter(
            RE::Actor* a_actor,
            const BlacklistFilter& a_filter)
        {
            if (!a_actor) {
                return false;
            }
            DistributionCore::FilterEvaluationServices services;
            services.resolveFormID = ResolveFormID;
            services.hasVirtualKeyword =
                [](RE::Actor* a_subject, RE::BGSKeyword* a_keyword) {
                    auto* npc = a_subject ?
                        a_subject->GetActorBase() :
                        nullptr;
                    return npc && a_keyword &&
                        npc->HasKeyword(a_keyword);
                };
            const auto result = DistributionCore::EvaluateFilter(
                a_actor,
                a_actor->GetActorBase(),
                a_filter,
                services);
            return result ==
                DistributionCore::FilterEvaluation::kMatch;
        }

        bool MatchActorFilters(
            RE::Actor* a_actor,
            const std::vector<BlacklistFilter>& a_filters,
            const bool a_requireAll)
        {
            if (a_filters.empty()) {
                return true;
            }
            const auto predicate =
                [&](const BlacklistFilter& a_filter) {
                    return MatchActorFilter(a_actor, a_filter);
                };
            return a_requireAll ?
                std::ranges::all_of(a_filters, predicate) :
                std::ranges::any_of(a_filters, predicate);
        }

        bool PlayerPrerequisitesMet(
            const Requirement& a_requirement)
        {
            return MatchActorFilters(
                RE::PlayerCharacter::GetSingleton(),
                a_requirement.playerPrerequisiteFilters,
                a_requirement.filtersRequireAll);
        }

        bool MatchesAuthorship(
            const Requirement& a_requirement,
            const ProgressEvent& a_event)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || a_event.creditedActor != player) {
                return false;
            }
            auto* source = a_event.sourceActor;
            if (!source || source->IsPlayerRef()) {
                return true;
            }
            if (source->IsSummoned()) {
                return a_requirement.allowSummonActions &&
                    Settings::GetSingleton()->creditSummonActions &&
                    ResolveSummonOwner(source) == player;
            }
            return a_requirement.allowFollowerActions &&
                Settings::GetSingleton()->creditFollowerActions &&
                IsActivePlayerFollower(source);
        }

        bool MatchSourceFilter(
            RE::TESForm* a_source,
            const BlacklistFilter& a_filter)
        {
            if (!a_source) {
                return false;
            }
            if (a_filter.type == "Source Plugin") {
                const auto* file = a_source->GetFile(0);
                const auto source = file ?
                    std::string(file->GetFilename()) :
                    std::string("Dynamic");
                const auto& expected = a_filter.optionText.empty() ?
                    a_filter.editorID :
                    a_filter.optionText;
                return _stricmp(
                    source.c_str(), expected.c_str()) == 0;
            }
            const auto expected = ResolveFormID(
                a_filter.type,
                a_filter.editorID,
                a_filter.formIDStr);
            if (a_filter.type == "Keyword") {
                auto* keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(
                        expected);
                auto* keywordForm =
                    skyrim_cast<RE::BGSKeywordForm*>(
                        a_source);
                return keyword && keywordForm &&
                    keywordForm->HasKeyword(keyword);
            }
            return expected != 0 &&
                a_source->GetFormID() == expected;
        }

        bool MatchSourceFilters(
            RE::TESForm* a_source,
            const std::vector<BlacklistFilter>& a_filters,
            const bool a_requireAll)
        {
            if (a_filters.empty()) {
                return true;
            }
            const auto predicate =
                [&](const BlacklistFilter& a_filter) {
                    const auto* descriptor =
                        DistributionCore::FilterRegistry().Find(a_filter.type);
                    if (descriptor &&
                        (descriptor->capabilities & DistributionCore::ToMask(
                            DistributionCore::TypeCapability::kRequiresForm)) != 0 &&
                        ResolveFormID(a_filter.type, a_filter.editorID,
                            a_filter.formIDStr) == 0) {
                        return false;
                    }
                    return ApplyFilterOperator(
                        MatchSourceFilter(a_source, a_filter),
                        a_filter.isNot);
                };
            return a_requireAll ?
                std::ranges::all_of(a_filters, predicate) :
                std::ranges::any_of(a_filters, predicate);
        }

        bool MatchEnvironmentFilter(
            const ProgressEvent& a_event,
            const BlacklistFilter& a_filter)
        {
            const auto expected = ResolveFormID(
                a_filter.type,
                a_filter.editorID,
                a_filter.formIDStr);
            if (a_filter.type == "Cell Type") {
                return a_event.cell &&
                    (static_cast<CellTypeFilter>(a_filter.optionMode) ==
                            CellTypeFilter::kInterior ?
                        a_event.cell->IsInteriorCell() :
                        !a_event.cell->IsInteriorCell());
            }
            if (a_filter.type == "City Status") {
                const auto inside = DistributionCore::IsCityLocation(
                    a_event.location);
                return static_cast<CityStatusFilter>(a_filter.optionMode) ==
                    CityStatusFilter::kInsideCity ? inside : !inside;
            }
            if (expected == 0) {
                return false;
            }
            if (a_filter.type == "Cell") {
                return a_event.cell &&
                    a_event.cell->GetFormID() == expected;
            }
            if (a_filter.type == "Location") {
                for (auto* location = a_event.location;
                     location;
                     location = location->parentLoc) {
                    if (location->GetFormID() == expected) {
                        return true;
                    }
                }
                return false;
            }
            if (a_filter.type == "Location Keyword") {
                auto* keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(expected);
                for (auto* location = a_event.location;
                     keyword && location;
                     location = location->parentLoc) {
                    if (location->HasKeyword(keyword)) {
                        return true;
                    }
                }
                return false;
            }
            if (a_filter.type == "Worldspace") {
                auto* actor = a_event.sourceActor ?
                    a_event.sourceActor :
                    a_event.creditedActor;
                const auto* worldspace = actor ?
                    actor->GetWorldspace() :
                    nullptr;
                return worldspace &&
                    worldspace->GetFormID() == expected;
            }
            return false;
        }

        bool MatchEnvironmentFilters(
            const ProgressEvent& a_event,
            const std::vector<BlacklistFilter>& a_filters,
            const bool a_requireAll)
        {
            if (a_filters.empty()) {
                return true;
            }
            const auto predicate =
                [&](const BlacklistFilter& a_filter) {
                    const auto* descriptor =
                        DistributionCore::FilterRegistry().Find(a_filter.type);
                    if (descriptor &&
                        (descriptor->capabilities & DistributionCore::ToMask(
                            DistributionCore::TypeCapability::kRequiresForm)) != 0 &&
                        ResolveFormID(a_filter.type, a_filter.editorID,
                            a_filter.formIDStr) == 0) {
                        return false;
                    }
                    return ApplyFilterOperator(
                        MatchEnvironmentFilter(a_event, a_filter),
                        a_filter.isNot);
                };
            return a_requireAll ?
                std::ranges::all_of(a_filters, predicate) :
                std::ranges::any_of(a_filters, predicate);
        }

        bool FiltersAreValidForScope(
            const Requirement& a_requirement,
            const FilterScope a_scope,
            const std::vector<BlacklistFilter>& a_filters)
        {
            return std::ranges::all_of(
                a_filters,
                [&](const BlacklistFilter& a_filter) {
                    return IsFilterAllowedForScope(
                        a_scope,
                        a_requirement.activity,
                        a_filter.type) &&
                        (a_filter.type != "City Status" ||
                            (a_filter.optionMode >= 0 &&
                             a_filter.optionMode <= 1));
                });
        }

        bool MatchesRequirement(
            const Requirement& a_requirement,
            const ProgressEvent& a_event)
        {
            if (a_requirement.activity != a_event.activity) {
                return false;
            }
            if (Settings::GetSingleton()->ignoreWIYTRewardEvents &&
                a_event.provenance ==
                    EventProvenance::kWIYTReward) {
                return false;
            }
            const auto reference = ResolveFormID(
                {},
                a_requirement.referenceEditorID,
                a_requirement.referenceFormID);
            if (reference != 0 &&
                (!a_event.sourceForm ||
                    a_event.sourceForm->GetFormID() != reference)) {
                return false;
            }
            if (!MatchesAuthorship(a_requirement, a_event) ||
                !FiltersAreValidForScope(
                    a_requirement,
                    FilterScope::kTargetActor,
                    a_requirement.targetActorFilters) ||
                !FiltersAreValidForScope(
                    a_requirement,
                    FilterScope::kSourceForm,
                    a_requirement.sourceFormFilters) ||
                !FiltersAreValidForScope(
                    a_requirement,
                    FilterScope::kEnvironment,
                    a_requirement.environmentFilters)) {
                return false;
            }
            return
                MatchActorFilters(
                       a_event.targetActor,
                       a_requirement.targetActorFilters,
                       a_requirement.filtersRequireAll) &&
                MatchSourceFilters(
                       a_event.sourceForm,
                       a_requirement.sourceFormFilters,
                       a_requirement.filtersRequireAll) &&
                MatchEnvironmentFilters(
                       a_event,
                       a_requirement.environmentFilters,
                       a_requirement.filtersRequireAll);
        }

        float RandomPercent()
        {
            static thread_local std::mt19937 generator{
                std::random_device{}()
            };
            static thread_local std::uniform_real_distribution<float>
                distribution(0.0f, 100.0f);
            return distribution(generator);
        }

        std::vector<RewardKey> SelectRewards(
            const TitleDefinition& a_title)
        {
            Rule rule;
            rule.rewardGroups = a_title.rewardGroups;
            std::vector<RewardKey> result;
            for (const auto& selection :
                 DistributionCore::RollRuleRewards(
                     rule,
                     [] { return RandomPercent(); })) {
                for (const auto rewardIndex :
                     selection.rewardIndices) {
                    result.push_back({
                        static_cast<std::uint32_t>(
                            selection.groupIndex),
                        static_cast<std::uint32_t>(rewardIndex)
                    });
                }
            }
            return result;
        }

        bool ApplyReward(
            const TitleDefinition& a_title,
            const RewardKey a_key)
        {
            if (a_key.group >= a_title.rewardGroups.size()) {
                return true;
            }
            const auto& group =
                a_title.rewardGroups[a_key.group];
            if (a_key.reward >= group.rewards.size()) {
                return true;
            }
            const auto& reward = group.rewards[a_key.reward];
            if (!DistributionCore::RewardRegistry().Supports(
                    reward.typeReward,
                    DistributionCore::Domain::kWIYT)) {
                logger::error(
                    "[WIYT] Title '{}' uses unsupported reward '{}'.",
                    a_title.name,
                    reward.typeReward);
                return true;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            if (reward.typeReward == "Skill Experience") {
                const auto actorValue =
                    ResolveActorValue(reward.editorID);
                if (actorValue == RE::ActorValue::kNone) {
                    return false;
                }
                player->AddSkillExperience(
                    actorValue,
                    static_cast<float>(reward.amount));
                return true;
            }
            if (reward.typeReward == "NSM Skill Experience") {
                return INLOS::NewSkillMenu::AddSkillExperience(
                    player->GetFormID(),
                    reward.editorID,
                    static_cast<float>(reward.amount));
            }
            if (reward.typeReward == "NSM Skill Bonus") {
                return INLOS::NewSkillMenu::AddSkillBonus(
                    player->GetFormID(),
                    reward.editorID,
                    static_cast<int>(reward.amount));
            }
            if (reward.typeReward == "NSM Perk Points") {
                return INLOS::NewSkillMenu::AddPerkPoints(
                    player->GetFormID(),
                    static_cast<int>(reward.amount));
            }
            auto formID = ResolveFormID(
                reward.typeReward,
                reward.editorID,
                reward.formIDStr);
            if (reward.typeReward == "Gold" && formID == 0) {
                formID = 0x0000000F;
            }
            if (reward.typeReward == "Leveled Item") {
                auto* list =
                    RE::TESForm::LookupByID<RE::TESLevItem>(
                        formID);
                if (!list) {
                    return false;
                }
                for (const auto& item :
                     DistributionCore::ResolveLeveledItems(
                         player,
                         list,
                         std::max<std::uint32_t>(
                             1,
                             reward.amount))) {
                    SuppressNextAcquisition(
                        item.item->GetFormID(),
                        static_cast<std::int32_t>(item.count));
                    player->AddObjectToContainer(
                        item.item,
                        nullptr,
                        static_cast<std::int32_t>(item.count),
                        nullptr);
                }
                return true;
            }
            if (reward.typeReward == "Spell") {
                auto* spell =
                    RE::TESForm::LookupByID<RE::SpellItem>(formID);
                if (!spell) {
                    return false;
                }
                if (!player->HasSpell(spell)) {
                    player->AddSpell(spell);
                }
                return true;
            }
            if (reward.typeReward == "Perk") {
                auto* perk =
                    RE::TESForm::LookupByID<RE::BGSPerk>(formID);
                if (!perk) {
                    return false;
                }
                if (!player->HasPerk(perk)) {
                    player->AddPerk(perk);
                }
                return true;
            }
            auto* item =
                RE::TESForm::LookupByID<RE::TESBoundObject>(
                    formID);
            if (!item) {
                return false;
            }
            const auto count = static_cast<std::int32_t>(
                std::max<std::uint32_t>(1, reward.amount));
            SuppressNextAcquisition(formID, count);
            player->AddObjectToContainer(
                item,
                nullptr,
                count,
                nullptr);
            return true;
        }

        void ProcessRewardsForTitle(
            const TitleDefinition& a_title)
        {
            auto progress =
                State::GetSingleton()->GetTitleProgress(a_title.id);
            if (!progress || !progress->completed ||
                progress->rewarded) {
                return;
            }
            if (!progress->rewardsSelected) {
                State::GetSingleton()->SetSelectedRewards(
                    a_title.id,
                    SelectRewards(a_title));
                progress =
                    State::GetSingleton()->GetTitleProgress(
                        a_title.id);
            }
            bool allDelivered = true;
            for (const auto reward : progress->selectedRewards) {
                if (State::GetSingleton()->IsRewardDelivered(
                        a_title.id,
                        reward)) {
                    continue;
                }
                if (ApplyReward(a_title, reward)) {
                    State::GetSingleton()->MarkRewardDelivered(
                        a_title.id,
                        reward);
                }
                else {
                    allDelivered = false;
                }
            }
            if (allDelivered) {
                State::GetSingleton()->MarkRewarded(a_title.id);
            }
        }

        void HandleProgressChange(
            const TitleDefinition& a_title,
            const ProgressChange& a_change)
        {
            if (!a_change.changed) {
                return;
            }
            DFGBridge::GetSingleton()->SynchronizeTitle(
                a_title.id,
                a_change.overall);
            if (a_change.newlyCompleted) {
                logger::info(
                    "[WIYT] Title earned: '{}'.",
                    a_title.name);
                ProcessRewardsForTitle(a_title);
            }
        }

        bool PrepareRequirementProgress(
            const TitleDefinition& a_title,
            const Requirement& a_requirement)
        {
            const auto prerequisitesMet =
                FiltersAreValidForScope(
                    a_requirement,
                    FilterScope::kPlayerPrerequisite,
                    a_requirement.playerPrerequisiteFilters) &&
                PlayerPrerequisitesMet(a_requirement);
            HandleProgressChange(
                a_title,
                State::GetSingleton()->SetPrerequisiteState(
                    a_title,
                    a_requirement,
                    prerequisitesMet));
            return prerequisitesMet ||
                a_requirement.prerequisiteMode ==
                    PrerequisiteMode::kRequiredToComplete;
        }

        void ApplyStatisticValue(
            const std::string_view a_name,
            const float a_value)
        {
            {
                std::scoped_lock lock(g_runtimeLock);
                g_statisticCache[std::string(a_name)] = a_value;
            }
            for (const auto& title :
                 Store::GetSingleton()->Titles()) {
                if (!title.enabled ||
                    Store::GetSingleton()->
                        IsTitlePendingDeletion(title.id)) {
                    continue;
                }
                for (const auto& requirement :
                     title.requirements) {
                    if (requirement.source !=
                            ProgressSource::kVanillaStatistic ||
                        _stricmp(
                            requirement.statisticName.c_str(),
                            std::string(a_name).c_str()) != 0) {
                        continue;
                    }
                    if (!PrepareRequirementProgress(
                            title, requirement)) {
                        continue;
                    }
                    HandleProgressChange(
                        title,
                        State::GetSingleton()->
                            SetAbsoluteProgress(
                                title,
                                requirement,
                                a_value));
                }
            }
        }

        void RefreshGlobalAndGraphSources()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            for (const auto& title :
                 Store::GetSingleton()->Titles()) {
                if (!title.enabled) {
                    continue;
                }
                for (const auto& requirement :
                     title.requirements) {
                    float value = 0.0f;
                    bool resolved = false;
                    if (requirement.source ==
                        ProgressSource::kGlobal) {
                        const auto formID = ResolveFormID(
                            "Global",
                            requirement.referenceEditorID,
                            requirement.referenceFormID);
                        if (auto* global =
                                RE::TESForm::LookupByID<
                                    RE::TESGlobal>(formID)) {
                            value = global->value;
                            resolved = true;
                        }
                    }
                    else if (requirement.source ==
                                 ProgressSource::kGraphVariable &&
                             player &&
                             !requirement.graphVariableName.empty()) {
                        const RE::BSFixedString name(
                            requirement.graphVariableName.c_str());
                        switch (requirement.graphVariableType) {
                        case 0: {
                            bool result = false;
                            resolved =
                                player->GetGraphVariableBool(
                                    name,
                                    result);
                            value = result ? 1.0f : 0.0f;
                            break;
                        }
                        case 1: {
                            int result = 0;
                            resolved =
                                player->GetGraphVariableInt(
                                    name,
                                    result);
                            value = static_cast<float>(result);
                            break;
                        }
                        default:
                            resolved =
                                player->GetGraphVariableFloat(
                                    name,
                                    value);
                            break;
                        }
                    }
                    if (resolved) {
                        if (!PrepareRequirementProgress(
                                title, requirement)) {
                            continue;
                        }
                        HandleProgressChange(
                            title,
                            State::GetSingleton()->
                                SetAbsoluteProgress(
                                    title,
                                    requirement,
                                    value));
                    }
                }
            }
        }

        struct AsyncFetchState
        {
            std::atomic_size_t remaining{ 0 };
        };

        class StatisticCallback :
            public RE::BSScript::IStackCallbackFunctor
        {
        public:
            StatisticCallback(
                std::string a_name,
                std::shared_ptr<AsyncFetchState> a_state) :
                _name(std::move(a_name)),
                _state(std::move(a_state))
            {}

            void operator()(
                RE::BSScript::Variable a_result) override
            {
                float value = 0.0f;
                if (a_result.IsInt()) {
                    value = static_cast<float>(
                        a_result.GetSInt());
                }
                else if (a_result.IsFloat()) {
                    value = a_result.GetFloat();
                }
                ApplyStatisticValue(_name, value);
                if (_state->remaining.fetch_sub(1) == 1) {
                    g_statisticFetchInFlight = false;
                    ProcessPendingRewards();
                }
            }

            bool CanSave() const override { return false; }
            void SetObject(
                const RE::BSTSmartPointer<
                    RE::BSScript::Object>&) override
            {}

        private:
            std::string _name;
            std::shared_ptr<AsyncFetchState> _state;
        };

        void FetchStatistics(
            const std::vector<std::string>& a_names)
        {
            if (a_names.empty()) {
                g_statisticFetchInFlight = false;
                return;
            }
            auto* virtualMachine =
                RE::BSScript::Internal::VirtualMachine::
                    GetSingleton();
            if (!virtualMachine) {
                g_statisticFetchInFlight = false;
                return;
            }
            auto state = std::make_shared<AsyncFetchState>();
            state->remaining = a_names.size();
            std::size_t failures = 0;
            for (const auto& name : a_names) {
                auto arguments = RE::MakeFunctionArguments(
                    RE::BSFixedString(name.c_str()));
                RE::BSTSmartPointer<
                    RE::BSScript::IStackCallbackFunctor> callback{
                    new StatisticCallback(name, state)
                };
                if (!virtualMachine->DispatchStaticCall(
                        "Game",
                        "QueryStat",
                        arguments,
                        callback)) {
                    ++failures;
                }
            }
            if (failures > 0 &&
                state->remaining.fetch_sub(failures) == failures) {
                g_statisticFetchInFlight = false;
            }
        }

        bool ConsumeSuppression(
            const RE::FormID a_formID,
            const std::int32_t a_count)
        {
            std::scoped_lock lock(g_runtimeLock);
            const auto found =
                g_suppressedAcquisitions.find(a_formID);
            if (found == g_suppressedAcquisitions.end()) {
                return false;
            }
            found->second -= std::max(1, a_count);
            if (found->second <= 0) {
                g_suppressedAcquisitions.erase(found);
            }
            return true;
        }

        float EstimateMagicDamage(RE::TESForm* a_source)
        {
            auto* spell = a_source ?
                a_source->As<RE::SpellItem>() :
                nullptr;
            if (!spell) {
                return 1.0f;
            }
            float amount = 0.0f;
            for (const auto* effect : spell->effects) {
                if (!effect || !effect->IsHostile()) {
                    continue;
                }
                amount += std::max(
                    0.0f,
                    effect->effectItem.magnitude) *
                    static_cast<float>(
                        std::max<std::uint32_t>(
                            1,
                            effect->effectItem.duration));
            }
            return std::max(1.0f, amount);
        }

        std::optional<ActivityType> ParseActivity(
            const std::string_view a_value)
        {
            for (std::size_t index = 0;
                 index < kActivityCount;
                 ++index) {
                const auto activity =
                    static_cast<ActivityType>(index);
                if (_stricmp(
                        ToString(activity),
                        std::string(a_value).c_str()) == 0) {
                    return activity;
                }
            }
            return std::nullopt;
        }
    }

    const std::vector<std::string>& KnownStatistics()
    {
        static const std::vector<std::string> values{
            "Locations Discovered",
            "Dungeons Cleared",
            "Hours Slept",
            "Hours Waiting",
            "Standing Stones Found",
            "Gold Found",
            "Most Gold Carried",
            "Chests Looted",
            "Skill Increases",
            "Skill Books Read",
            "Food Eaten",
            "Training Sessions",
            "Books Read",
            "Horses Owned",
            "Houses Owned",
            "Stores Invested In",
            "Barters",
            "Persuasions",
            "Bribes",
            "Intimidations",
            "Diseases Contracted",
            "Days as a Vampire",
            "Days as a Werewolf",
            "Necks Bitten",
            "Vampirism Cures",
            "Werewolf Transformations",
            "Mauls",
            "Quests Completed",
            "Misc Objectives Completed",
            "Main Quests Completed",
            "Side Quests Completed",
            "The Companions Quests Completed",
            "College of Winterhold Quests Completed",
            "Thieves' Guild Quests Completed",
            "The Dark Brotherhood Quests Completed",
            "Civil War Quests Completed",
            "Daedric Quests Completed",
            "Dawnguard Quests Completed",
            "Dragonborn Quests Completed",
            "Questlines Completed",
            "People Killed",
            "Animals Killed",
            "Creatures Killed",
            "Undead Killed",
            "Daedra Killed",
            "Automatons Killed",
            "Critical Strikes",
            "Sneak Attacks",
            "Backstabs",
            "Weapons Disarmed",
            "Brawls Won",
            "Bunnies Slaughtered",
            "Spells Learned",
            "Dragon Souls Collected",
            "Words Of Power Learned",
            "Words Of Power Unlocked",
            "Shouts Learned",
            "Shouts Unlocked",
            "Shouts Mastered",
            "Times Shouted",
            "Soul Gems Used",
            "Souls Trapped",
            "Magic Items Made",
            "Weapons Improved",
            "Weapons Made",
            "Armor Improved",
            "Armor Made",
            "Potions Mixed",
            "Potions Used",
            "Poisons Mixed",
            "Poisons Used",
            "Ingredients Harvested",
            "Ingredients Eaten",
            "Nirnroots Found",
            "Wings Plucked",
            "Total Lifetime Bounty",
            "Largest Bounty",
            "Locks Picked",
            "Pockets Picked",
            "Items Pickpocketed",
            "Times Jailed",
            "Days Jailed",
            "Fines Paid",
            "Jail Escapes",
            "Items Stolen",
            "Assaults",
            "Murders",
            "Horses Stolen",
            "Trespasses",
            "Eastmarch Bounty",
            "Falkreath Bounty",
            "Haafingar Bounty",
            "Hjaalmarch Bounty",
            "The Pale Bounty",
            "The Reach Bounty",
            "The Rift Bounty",
            "Tribal Orcs Bounty",
            "Whiterun Bounty",
            "Winterhold Bounty"
        };
        return values;
    }

    void RebuildRequirementIndex()
    {
        bool needsDamageTracking = false;
        bool needsSpellDamageTracking = false;
        for (auto& entries : g_eventIndex) {
            entries.clear();
        }
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            if (!title.enabled) {
                continue;
            }
            for (const auto& requirement :
                 title.requirements) {
                if (requirement.source !=
                    ProgressSource::kEventCounter) {
                    continue;
                }
                const auto index =
                    static_cast<std::size_t>(
                        requirement.activity);
                if (index < g_eventIndex.size()) {
                    g_eventIndex[index].push_back({
                        title.id,
                        requirement.id
                    });
                }
                needsDamageTracking =
                    needsDamageTracking ||
                    requirement.activity ==
                        ActivityType::kDamageDealt;
                needsSpellDamageTracking =
                    needsSpellDamageTracking ||
                    requirement.activity ==
                        ActivityType::kSpellDamageDealt;
            }
        }
        g_damageTrackingEnabled = needsDamageTracking;
        g_spellDamageTrackingEnabled =
            needsSpellDamageTracking;
        State::GetSingleton()->ReconcileDefinitions(
            Store::GetSingleton()->Titles());
    }

    void InstallDamageTracking()
    {
        static std::once_flag installed;
        std::call_once(installed, [] {
            InstallHealthDamageHook<0>(
                RE::VTABLE_Actor[0]);
            InstallHealthDamageHook<1>(
                RE::VTABLE_Character[0]);
            InstallHealthDamageHook<2>(
                RE::VTABLE_PlayerCharacter[0]);
            logger::info(
                "[WIYT] Actual health-damage tracking installed.");
        });
    }

    void ResetTransientTracking()
    {
        {
            std::scoped_lock lock(g_runtimeLock);
            g_recentHitSources.clear();
            g_suppressedAcquisitions.clear();
            g_statisticCache.clear();
        }
        g_statisticFetchInFlight = false;
        g_lastStatisticRefresh = {};
        EventHandler::GetSingleton()->ResetTransient();
    }

    void ReportProgressEvent(const ProgressEvent& a_event)
    {
        if (!Settings::GetSingleton()->enabled) {
            return;
        }
        const auto index =
            static_cast<std::size_t>(a_event.activity);
        if (index >= g_eventIndex.size()) {
            return;
        }
        const auto entries = g_eventIndex[index];
        for (const auto& entry : entries) {
            auto* title =
                Store::GetSingleton()->FindTitle(entry.titleID);
            if (!title || !title->enabled ||
                Store::GetSingleton()->
                    IsTitlePendingDeletion(title->id) ||
                Store::GetSingleton()->
                    IsPackagePendingDeletion(title->packageID)) {
                continue;
            }
            const auto requirement =
                std::ranges::find(
                    title->requirements,
                    entry.requirementID,
                    &Requirement::id);
            if (requirement == title->requirements.end() ||
                !MatchesRequirement(*requirement, a_event)) {
                continue;
            }
            if (!PrepareRequirementProgress(
                    *title, *requirement)) {
                continue;
            }
            HandleProgressChange(
                *title,
                State::GetSingleton()->AddEventProgress(
                    *title,
                    *requirement,
                    std::max(0.0f, a_event.amount),
                    a_event.uniqueKey));
        }
    }

    void RefreshProgressSources(const bool a_force)
    {
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            const auto progress =
                State::GetSingleton()->GetTitleProgress(title.id);
            if (!title.enabled ||
                (progress && progress->completed)) {
                continue;
            }
            for (const auto& requirement : title.requirements) {
                PrepareRequirementProgress(title, requirement);
            }
        }
        RefreshGlobalAndGraphSources();
        const auto now = std::chrono::steady_clock::now();
        if (!a_force &&
            now - g_lastStatisticRefresh <
                std::chrono::duration<float>(
                    Settings::GetSingleton()->
                        minimumStatisticRefreshSeconds)) {
            return;
        }
        if (g_statisticFetchInFlight.exchange(true)) {
            return;
        }
        g_lastStatisticRefresh = now;
        std::set<std::string> names;
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            const auto progress =
                State::GetSingleton()->GetTitleProgress(title.id);
            if (!title.enabled ||
                (progress && progress->completed)) {
                continue;
            }
            for (const auto& requirement :
                 title.requirements) {
                if (requirement.source ==
                        ProgressSource::kVanillaStatistic &&
                    !requirement.statisticName.empty()) {
                    names.emplace(requirement.statisticName);
                }
            }
        }
        FetchStatistics(std::vector<std::string>(
            names.begin(),
            names.end()));
    }

    void ProcessPendingRewards()
    {
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            ProcessRewardsForTitle(title);
        }
    }

    float GetCachedStatistic(const std::string_view a_name)
    {
        std::scoped_lock lock(g_runtimeLock);
        const auto found =
            g_statisticCache.find(std::string(a_name));
        return found != g_statisticCache.end() ?
            found->second :
            0.0f;
    }

    void SuppressNextAcquisition(
        const RE::FormID a_formID,
        const std::int32_t a_count)
    {
        if (a_formID == 0 || a_count <= 0) {
            return;
        }
        std::scoped_lock lock(g_runtimeLock);
        g_suppressedAcquisitions[a_formID] += a_count;
    }

    EventHandler* EventHandler::GetSingleton()
    {
        static EventHandler singleton;
        return std::addressof(singleton);
    }

    void EventHandler::ResetTransient()
    {
        std::scoped_lock lock(_lock);
        _pendingHarvest = {};
    }

    void EventHandler::Register()
    {
        if (auto* source =
                RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<RE::TESDeathEvent>(
                GetSingleton());
            source->AddEventSink<RE::TESContainerChangedEvent>(
                GetSingleton());
            source->AddEventSink<RE::TESActivateEvent>(
                GetSingleton());
            source->AddEventSink<RE::TESQuestStageEvent>(
                GetSingleton());
            source->AddEventSink<RE::TESHitEvent>(
                GetSingleton());
        }
        if (auto* source = SKSE::GetModCallbackEventSource()) {
            source->AddEventSink(
                static_cast<
                    RE::BSTEventSink<SKSE::ModCallbackEvent>*>(
                        GetSingleton()));
        }
        if (auto* source = RE::ItemCrafted::GetEventSource()) {
            source->AddEventSink(GetSingleton());
        }
        if (auto* source =
                RE::LocationDiscovery::GetEventSource()) {
            source->AddEventSink(GetSingleton());
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink(
                static_cast<
                    RE::BSTEventSink<RE::MenuOpenCloseEvent>*>(
                        GetSingleton()));
        }
        logger::info("[WIYT] Progress event sinks registered.");
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::TESDeathEvent* a_event,
        RE::BSTEventSource<RE::TESDeathEvent>*)
    {
        if (!a_event || !a_event->dead) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* targetRef = a_event->actorDying.get();
        auto* killerRef = a_event->actorKiller.get();
        auto* target = targetRef ?
            targetRef->As<RE::Actor>() :
            nullptr;
        auto* killer = killerRef ?
            killerRef->As<RE::Actor>() :
            nullptr;
        auto* credited = ResolveCreditedActor(killer);
        if (target && credited) {
            RE::TESForm* sourceForm = nullptr;
            if (killer) {
                std::scoped_lock lock(g_runtimeLock);
                const auto found = g_recentHitSources.find(
                    DamagePairKey(
                        killer->GetFormID(),
                        target->GetFormID()));
                if (found != g_recentHitSources.end()) {
                    sourceForm = RE::TESForm::LookupByID(
                        found->second.formID);
                }
            }
            auto* contextActor = killer ? killer : target;
            ReportProgressEvent({
                ActivityType::kActorKilled,
                credited,
                killer,
                target,
                sourceForm,
                contextActor->GetParentCell(),
                contextActor->GetCurrentLocation(),
                1.0f,
                (static_cast<std::uint64_t>(
                     target->GetFormID()) <<
                    32) ^
                    g_eventSequence.fetch_add(1),
                EventProvenance::kGameplay
            });
        }
        RefreshProgressSources();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::TESContainerChangedEvent* a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*)
    {
        if (!a_event || a_event->baseObj == 0 ||
            a_event->itemCount == 0) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const auto playerID = player->GetFormID();
        auto* item = RE::TESForm::LookupByID(a_event->baseObj);
        if (a_event->newContainer == playerID &&
            a_event->itemCount > 0) {
            const auto suppressed = ConsumeSuppression(
                a_event->baseObj,
                a_event->itemCount);
            const auto provenance = suppressed ?
                EventProvenance::kWIYTReward :
                EventProvenance::kGameplay;
            ProgressEvent progress{
                ActivityType::kItemAcquired,
                player,
                player,
                nullptr,
                item,
                player->GetParentCell(),
                player->GetCurrentLocation(),
                static_cast<float>(a_event->itemCount),
                static_cast<std::uint64_t>(a_event->baseObj),
                provenance
            };
            ReportProgressEvent(progress);
            bool harvested = false;
            {
                std::scoped_lock lock(_lock);
                harvested =
                    _pendingHarvest.itemID == a_event->baseObj &&
                    std::chrono::steady_clock::now() -
                            _pendingHarvest.timestamp <
                        std::chrono::seconds(3);
                if (harvested) {
                    _pendingHarvest = {};
                }
            }
            if (harvested && !suppressed) {
                progress.activity =
                    ActivityType::kItemHarvested;
                ReportProgressEvent(progress);
            }
            if (a_event->baseObj == 0x0000000F &&
                !suppressed) {
                progress.activity =
                    ActivityType::kGoldEarned;
                ReportProgressEvent(progress);
            }
        }
        else if (a_event->oldContainer == playerID &&
                 a_event->itemCount > 0 &&
                 a_event->baseObj == 0x0000000F) {
            ReportProgressEvent({
                ActivityType::kGoldSpent,
                player,
                player,
                nullptr,
                item,
                player->GetParentCell(),
                player->GetCurrentLocation(),
                static_cast<float>(a_event->itemCount),
                0,
                EventProvenance::kGameplay
            });
        }
        RefreshProgressSources();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::TESActivateEvent* a_event,
        RE::BSTEventSource<RE::TESActivateEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* action = a_event->actionRef.get();
        auto* activated = a_event->objectActivated.get();
        if (!player || action != player || !activated) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* base = activated->GetBaseObject();
        RE::TESBoundObject* produce = nullptr;
        if (auto* flora = base ?
                base->As<RE::TESFlora>() :
                nullptr) {
            produce = flora->produceItem;
        }
        else if (auto* tree = base ?
                     base->As<RE::TESObjectTREE>() :
                     nullptr) {
            produce = tree->produceItem;
        }
        if (produce) {
            std::scoped_lock lock(_lock);
            _pendingHarvest = {
                produce->GetFormID(),
                player->GetParentCell() ?
                    player->GetParentCell()->GetFormID() :
                    0,
                std::chrono::steady_clock::now()
            };
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::TESQuestStageEvent* a_event,
        RE::BSTEventSource<RE::TESQuestStageEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* quest =
            RE::TESForm::LookupByID<RE::TESQuest>(
                a_event->formID);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (quest && player && quest->IsCompleted()) {
            ReportProgressEvent({
                ActivityType::kQuestCompleted,
                player,
                player,
                nullptr,
                quest,
                player->GetParentCell(),
                player->GetCurrentLocation(),
                1.0f,
                quest->GetFormID(),
                EventProvenance::kGameplay
            });
        }
        RefreshProgressSources();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::TESHitEvent* a_event,
        RE::BSTEventSource<RE::TESHitEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* sourceRef = a_event->cause.get();
        auto* targetRef = a_event->target.get();
        auto* sourceActor = sourceRef ?
            sourceRef->As<RE::Actor>() :
            nullptr;
        auto* targetActor = targetRef ?
            targetRef->As<RE::Actor>() :
            nullptr;
        auto* credited = ResolveCreditedActor(sourceActor);
        if (!credited || !targetActor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* sourceForm =
            RE::TESForm::LookupByID(a_event->source);
        if (g_damageTrackingEnabled.load()) {
            std::scoped_lock lock(g_runtimeLock);
            if (g_recentHitSources.size() > 2048) {
                const auto cutoff =
                    std::chrono::steady_clock::now() -
                    std::chrono::seconds(10);
                std::erase_if(
                    g_recentHitSources,
                    [&](const auto& a_entry) {
                        return a_entry.second.timestamp < cutoff;
                    });
            }
            g_recentHitSources[
                DamagePairKey(
                    sourceActor->GetFormID(),
                    targetActor->GetFormID())] = {
                sourceForm ? sourceForm->GetFormID() : 0,
                std::chrono::steady_clock::now()
            };
        }
        if (g_spellDamageTrackingEnabled.load() &&
            sourceForm &&
            sourceForm->As<RE::SpellItem>()) {
            ReportProgressEvent({
                ActivityType::kSpellDamageDealt,
                credited,
                sourceActor,
                targetActor,
                sourceForm,
                sourceActor->GetParentCell(),
                sourceActor->GetCurrentLocation(),
                EstimateMagicDamage(sourceForm),
                0,
                EventProvenance::kGameplay
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::ItemCrafted::Event* a_event,
        RE::BSTEventSource<RE::ItemCrafted::Event>*)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_event || !a_event->item || !player) {
            return RE::BSEventNotifyControl::kContinue;
        }
        ReportProgressEvent({
            ActivityType::kItemCrafted,
            player,
            player,
            nullptr,
            a_event->item,
            player->GetParentCell(),
            player->GetCurrentLocation(),
            1.0f,
            a_event->item->GetFormID(),
            EventProvenance::kGameplay
        });
        RefreshProgressSources();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
    {
        if (a_event && !a_event->opening) {
            RefreshProgressSources(false);
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const RE::LocationDiscovery::Event* a_event,
        RE::BSTEventSource<RE::LocationDiscovery::Event>*)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_event || !player) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* location = player->GetCurrentLocation();
        std::string identity =
            a_event->worldspaceID ? a_event->worldspaceID : "";
        if (a_event->mapMarkerData) {
            identity += '|';
            if (const auto* name =
                    a_event->mapMarkerData->
                        locationName.GetFullName()) {
                identity += name;
            }
        }
        ReportProgressEvent({
            ActivityType::kLocationDiscovered,
            player,
            player,
            nullptr,
            location,
            player->GetParentCell(),
            location,
            1.0f,
            location ?
                static_cast<std::uint64_t>(
                    location->GetFormID()) :
                StableUniqueKey(identity),
            EventProvenance::kGameplay
        });
        RefreshProgressSources();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl EventHandler::ProcessEvent(
        const SKSE::ModCallbackEvent* a_event,
        RE::BSTEventSource<SKSE::ModCallbackEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const auto* name = a_event->eventName.c_str();
        if (_stricmp(name, "DynamicFormsGeneratorLoaded") == 0 ||
            _stricmp(
                name,
                "DynamicFormsGeneratorUpdated") == 0) {
            if (_stricmp(
                    name,
                    "DynamicFormsGeneratorUpdated") == 0) {
                Manager::GetSingleton()->RefreshLists(
                    a_event->strArg.c_str());
            }
            DFGBridge::GetSingleton()->SynchronizeAll();
            return RE::BSEventNotifyControl::kContinue;
        }
        if (_stricmp(name, "WIYTRefreshProgress") == 0) {
            RefreshProgressSources(true);
            return RE::BSEventNotifyControl::kContinue;
        }
        if (_stricmp(name, "INLOSActorDefeated") == 0) {
            auto* target = a_event->sender ?
                a_event->sender->As<RE::Actor>() :
                nullptr;
            RE::Actor* instigator = nullptr;
            const auto argument =
                std::string(a_event->strArg.c_str());
            const auto separator = argument.find('|');
            if (separator != std::string::npos) {
                try {
                    instigator =
                        RE::TESForm::LookupByID<RE::Actor>(
                            static_cast<RE::FormID>(
                                std::stoul(
                                    argument.substr(separator + 1),
                                    nullptr,
                                    16)));
                }
                catch (...) {
                    instigator = nullptr;
                }
            }
            auto* credited = ResolveCreditedActor(instigator);
            if (target && credited) {
                auto* contextActor = instigator ?
                    instigator :
                    target;
                ReportProgressEvent({
                    ActivityType::kActorDefeated,
                    credited,
                    instigator,
                    target,
                    nullptr,
                    contextActor->GetParentCell(),
                    contextActor->GetCurrentLocation(),
                    1.0f,
                    (static_cast<std::uint64_t>(
                         target->GetFormID()) <<
                        32) ^
                        g_eventSequence.fetch_add(1),
                    EventProvenance::kExternalMod
                });
            }
            RefreshProgressSources();
            return RE::BSEventNotifyControl::kContinue;
        }
        if (_stricmp(name, "WIYTReportProgress") == 0) {
            const auto argument =
                std::string(a_event->strArg.c_str());
            const auto separator = argument.find('|');
            const auto activityName =
                argument.substr(0, separator);
            const auto activity = ParseActivity(activityName);
            if (!activity) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto* sourceActor = a_event->sender ?
                a_event->sender->As<RE::Actor>() :
                nullptr;
            auto* credited = ResolveCreditedActor(sourceActor);
            if (!credited && sourceActor &&
                sourceActor->IsPlayerRef()) {
                credited = sourceActor;
            }
            if (credited) {
                ReportProgressEvent({
                    *activity,
                    credited,
                    sourceActor,
                    nullptr,
                    nullptr,
                    credited->GetParentCell(),
                    credited->GetCurrentLocation(),
                    std::max(0.0f, a_event->numArg),
                    g_eventSequence.fetch_add(1),
                    EventProvenance::kExternalMod
                });
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
}
