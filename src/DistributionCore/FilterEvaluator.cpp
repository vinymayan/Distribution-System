#include "DistributionCore/FilterEvaluator.h"

#include <algorithm>
#include <cmath>

namespace DistributionCore
{
    namespace
    {
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
                return std::abs(
                    a_value - a_filter.minimumValue) <= 0.001f;
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

        FilterEvaluation Result(const bool a_value)
        {
            return a_value ?
                FilterEvaluation::kMatch :
                FilterEvaluation::kNoMatch;
        }

        bool MatchesActorValue(
            RE::Actor* a_actor,
            const BlacklistFilter& a_filter)
        {
            if (!a_actor) {
                return false;
            }
            const auto actorValue =
                ResolveActorValue(a_filter.actorValueName);
            auto* owner = a_actor->AsActorValueOwner();
            if (actorValue == RE::ActorValue::kNone || !owner) {
                return false;
            }
            float value = 0.0f;
            switch (a_filter.actorValueMode) {
            case ActorValueMode::kCurrent:
                value = owner->GetActorValue(actorValue);
                break;
            case ActorValueMode::kPermanent:
                value = owner->GetPermanentActorValue(actorValue);
                break;
            case ActorValueMode::kMaximum:
                if (!IsMaximumActorValueSupported(actorValue)) {
                    return false;
                }
                value = a_actor->GetActorValueMax(actorValue);
                break;
            default:
                return false;
            }
            return std::isfinite(value) &&
                MatchesNumeric(value, a_filter);
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

        bool IsNPCInLeveledList(
            RE::TESNPC* a_npc,
            RE::TESLevCharacter* a_list,
            std::set<RE::FormID>& a_visited)
        {
            if (!a_npc || !a_list ||
                !a_visited.insert(a_list->GetFormID()).second) {
                return false;
            }
            for (const auto& entry : a_list->entries) {
                auto* form = entry.form;
                if (!form) {
                    continue;
                }
                if (form->Is(RE::FormType::NPC) &&
                    form->GetFormID() == a_npc->GetFormID()) {
                    return true;
                }
                if (form->Is(RE::FormType::LeveledNPC) &&
                    IsNPCInLeveledList(
                        a_npc,
                        form->As<RE::TESLevCharacter>(),
                        a_visited)) {
                    return true;
                }
            }
            return false;
        }

        bool MatchesQuestAlias(
            RE::Actor* a_actor,
            RE::TESQuest* a_quest,
            const std::optional<std::uint32_t> a_aliasID)
        {
            if (!a_actor || !a_quest) {
                return false;
            }
            const auto matches = [&](const std::uint32_t a_id) {
                const auto handle = a_quest->GetAliasedRef(a_id);
                const auto reference = handle.get();
                return reference && reference.get() == a_actor;
            };
            if (a_aliasID) {
                return matches(*a_aliasID);
            }
            return std::ranges::any_of(
                a_quest->aliases,
                [&](const RE::BGSBaseAlias* a_alias) {
                    return a_alias && matches(a_alias->aliasID);
                });
        }

        bool MatchesQuest(
            RE::Actor* a_actor,
            RE::TESQuest* a_quest,
            const BlacklistFilter& a_filter)
        {
            if (!a_quest) {
                return false;
            }
            switch (static_cast<QuestFilterMode>(
                a_filter.optionMode)) {
            case QuestFilterMode::kRunning:
                return a_quest->IsRunning();
            case QuestFilterMode::kCompleted:
                return a_quest->IsCompleted();
            case QuestFilterMode::kStopped:
                return a_quest->IsStopped();
            case QuestFilterMode::kNotStarted:
                return !a_quest->alreadyRun &&
                    !a_quest->IsRunning() &&
                    !a_quest->IsCompleted();
            case QuestFilterMode::kStage:
                return a_quest->GetCurrentStageID() ==
                    static_cast<std::uint16_t>(
                        std::clamp(
                            a_filter.optionValue, 0, 0xFFFF));
            case QuestFilterMode::kSpecificAlias:
                return MatchesQuestAlias(
                    a_actor,
                    a_quest,
                    static_cast<std::uint32_t>(
                        std::max(0, a_filter.optionValue)));
            case QuestFilterMode::kAnyAlias:
                return MatchesQuestAlias(
                    a_actor, a_quest, std::nullopt);
            default:
                return false;
            }
        }

        bool MatchesEquippedCategory(
            RE::Actor* a_actor,
            const EquippedCategoryFilter a_category)
        {
            if (!a_actor) {
                return false;
            }
            const auto* left = a_actor->GetEquippedObject(true);
            const auto* right = a_actor->GetEquippedObject(false);
            const auto* leftWeapon =
                left ? left->As<RE::TESObjectWEAP>() : nullptr;
            const auto* rightWeapon =
                right ? right->As<RE::TESObjectWEAP>() : nullptr;
            const auto matchesWeapon =
                [a_category](const RE::TESObjectWEAP* a_weapon) {
                    if (!a_weapon) {
                        return false;
                    }
                    const auto type = a_weapon->GetWeaponType();
                    switch (a_category) {
                    case EquippedCategoryFilter::kAnyWeapon:
                        return true;
                    case EquippedCategoryFilter::kOneHanded:
                        return type >= RE::WEAPON_TYPE::kOneHandSword &&
                            type <= RE::WEAPON_TYPE::kOneHandMace;
                    case EquippedCategoryFilter::kTwoHanded:
                        return type == RE::WEAPON_TYPE::kTwoHandSword ||
                            type == RE::WEAPON_TYPE::kTwoHandAxe;
                    case EquippedCategoryFilter::kBow:
                        return a_weapon->IsBow();
                    case EquippedCategoryFilter::kCrossbow:
                        return a_weapon->IsCrossbow();
                    case EquippedCategoryFilter::kStaff:
                        return a_weapon->IsStaff();
                    default:
                        return false;
                    }
                };
            if (a_category == EquippedCategoryFilter::kUnarmed) {
                return !leftWeapon && !rightWeapon;
            }
            if (matchesWeapon(leftWeapon) ||
                matchesWeapon(rightWeapon)) {
                return true;
            }
            const auto inventory = a_actor->GetInventory();
            return std::ranges::any_of(
                inventory,
                [a_category](const auto& a_entry) {
                    const auto* armor = a_entry.first ?
                        a_entry.first->As<RE::TESObjectARMO>() :
                        nullptr;
                    const auto& data = a_entry.second.second;
                    if (!armor || !data || !data->IsWorn()) {
                        return false;
                    }
                    switch (a_category) {
                    case EquippedCategoryFilter::kShield:
                        return armor->IsShield();
                    case EquippedCategoryFilter::kHeavyArmor:
                        return armor->IsHeavyArmor();
                    case EquippedCategoryFilter::kLightArmor:
                        return armor->IsLightArmor();
                    case EquippedCategoryFilter::kClothing:
                        return armor->IsClothing();
                    default:
                        return false;
                    }
                });
        }
    }

    bool IsCityLocation(RE::BGSLocation* a_location)
    {
        static auto* cityKeyword = []() -> RE::BGSKeyword* {
            auto* form = RE::TESForm::LookupByEditorID("LocTypeCity");
            return form ? form->As<RE::BGSKeyword>() : nullptr;
        }();
        for (auto* location = a_location;
             cityKeyword && location;
             location = location->parentLoc) {
            if (location->HasKeyword(cityKeyword)) {
                return true;
            }
        }
        return false;
    }

    static FilterEvaluation EvaluateFilterRaw(
        RE::Actor* a_actor,
        RE::TESNPC* a_npc,
        const BlacklistFilter& a_filter,
        const FilterEvaluationServices& a_services)
    {
        if (a_filter.type == "Actor Value") {
            if (!IsActorValueFilterValid(a_filter)) {
                return FilterEvaluation::kInvalid;
            }
            return Result(MatchesActorValue(a_actor, a_filter));
        }
        if (a_filter.type == "Height") {
            return Result(a_npc && MatchesNumeric(a_npc->height, a_filter));
        }
        if (a_filter.type == "Weight") {
            return Result(a_npc && MatchesNumeric(a_npc->weight, a_filter));
        }
        if (a_filter.type == "Source Plugin") {
            const auto* file = a_npc ? a_npc->GetFile(0) : nullptr;
            const auto source = file ?
                std::string(file->GetFilename()) :
                std::string("Dynamic");
            const auto expected = a_filter.optionText.empty() ?
                a_filter.editorID :
                a_filter.optionText;
            return Result(
                _stricmp(source.c_str(), expected.c_str()) == 0);
        }
        if (a_filter.type == "NPC Trait") {
            if (!a_npc) {
                return FilterEvaluation::kNoMatch;
            }
            switch (static_cast<NPCTraitFilter>(
                a_filter.optionMode)) {
            case NPCTraitFilter::kUnique:
                return Result(a_npc->IsUnique());
            case NPCTraitFilter::kEssential:
                return Result(
                    a_actor ? a_actor->IsEssential() :
                        a_npc->IsEssential());
            case NPCTraitFilter::kProtected:
                return Result(
                    a_actor ? a_actor->IsProtected() :
                        a_npc->IsProtected());
            default:
                return FilterEvaluation::kNoMatch;
            }
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
            return Result(MatchesNumeric(
                static_cast<float>(rank), a_filter));
        }
        if (a_filter.type == "Cell Type") {
            const auto* cell =
                a_actor ? a_actor->GetParentCell() : nullptr;
            return Result(
                cell &&
                (static_cast<CellTypeFilter>(a_filter.optionMode) ==
                        CellTypeFilter::kInterior ?
                    cell->IsInteriorCell() :
                    !cell->IsInteriorCell()));
        }
        if (a_filter.type == "City Status") {
            if (a_filter.optionMode < 0 || a_filter.optionMode > 1) {
                return FilterEvaluation::kInvalid;
            }
            const auto inside = IsCityLocation(
                a_actor ? a_actor->GetCurrentLocation() : nullptr);
            return Result(
                static_cast<CityStatusFilter>(a_filter.optionMode) ==
                    CityStatusFilter::kInsideCity ? inside : !inside);
        }
        if (a_filter.type == "Equipped Category") {
            return Result(MatchesEquippedCategory(
                a_actor,
                static_cast<EquippedCategoryFilter>(
                    a_filter.optionMode)));
        }

        if (!a_services.resolveFormID) {
            return FilterEvaluation::kNotHandled;
        }
        const auto formID = a_services.resolveFormID(
            a_filter.type,
            a_filter.editorID,
            a_filter.formIDStr);
        if (a_filter.type != "Gold" && formID == 0) {
            return FilterEvaluation::kInvalid;
        }

        if (a_filter.type == "NPC") {
            return Result(a_npc && (
                a_npc->GetFormID() == formID ||
                (a_npc->baseTemplateForm &&
                    a_npc->baseTemplateForm->GetFormID() == formID) ||
                (a_actor && a_actor->GetTemplateBase() &&
                    a_actor->GetTemplateBase()->GetFormID() == formID)));
        }
        if (a_filter.type == "Leveled NPC") {
            std::set<RE::FormID> visited;
            return Result(IsNPCInLeveledList(
                a_npc,
                RE::TESForm::LookupByID<RE::TESLevCharacter>(formID),
                visited));
        }
        if (a_filter.type == "Keyword") {
            auto* keyword =
                RE::TESForm::LookupByID<RE::BGSKeyword>(formID);
            bool match = keyword && a_npc && (
                a_npc->HasKeyword(keyword) ||
                (a_npc->race && a_npc->race->HasKeyword(keyword)));
            if (!match && keyword && a_services.hasVirtualKeyword) {
                match = a_services.hasVirtualKeyword(a_actor, keyword);
            }
            return Result(match);
        }
        if (a_filter.type == "Faction" ||
            a_filter.type == "Faction Rank") {
            auto* faction =
                RE::TESForm::LookupByID<RE::TESFaction>(formID);
            if (!a_actor || !faction ||
                !a_actor->IsInFaction(faction)) {
                return FilterEvaluation::kNoMatch;
            }
            return Result(
                a_filter.type == "Faction" ||
                MatchesNumeric(
                    static_cast<float>(
                        a_actor->GetFactionRank(
                            faction, a_actor->IsPlayerRef())),
                    a_filter));
        }
        if (a_filter.type == "Perk") {
            auto* perk =
                RE::TESForm::LookupByID<RE::BGSPerk>(formID);
            return Result(
                a_actor && perk && a_actor->HasPerk(perk));
        }
        if (a_filter.type == "Spell") {
            auto* spell =
                RE::TESForm::LookupByID<RE::SpellItem>(formID);
            return Result(
                a_actor && spell && a_actor->HasSpell(spell));
        }
        if (a_filter.type == "Shout") {
            auto* shout =
                RE::TESForm::LookupByID<RE::TESShout>(formID);
            return Result(
                a_actor && shout && a_actor->HasShout(shout));
        }
        if (a_filter.type == "Race") {
            return Result(
                a_npc && a_npc->race &&
                a_npc->race->GetFormID() == formID);
        }
        if (a_filter.type == "Combat Style") {
            return Result(
                a_npc && a_npc->combatStyle &&
                a_npc->combatStyle->GetFormID() == formID);
        }
        if (a_filter.type == "Voice Type") {
            return Result(
                a_npc && a_npc->voiceType &&
                a_npc->voiceType->GetFormID() == formID);
        }
        if (a_filter.type == "Class") {
            return Result(
                a_npc && a_npc->npcClass &&
                a_npc->npcClass->GetFormID() == formID);
        }
        if (a_filter.type == "Skin") {
            return Result(
                a_npc && a_npc->skin &&
                a_npc->skin->GetFormID() == formID);
        }
        if (a_filter.type == "Inventory Item" ||
            a_filter.type == "Inventory Count") {
            auto* item =
                RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
            if (!a_actor || !item) {
                return FilterEvaluation::kNoMatch;
            }
            const auto count = std::max(
                0, a_actor->GetInventoryCount(item));
            return Result(
                a_filter.type == "Inventory Item" ?
                count > 0 :
                MatchesNumeric(
                    static_cast<float>(count), a_filter));
        }
        if (a_filter.type == "Gold") {
            return Result(
                a_actor &&
                MatchesNumeric(
                    static_cast<float>(
                        std::max(0, a_actor->GetGoldAmount())),
                    a_filter));
        }
        if (a_filter.type == "Equipped Item") {
            return Result(IsEquipped(
                a_actor,
                RE::TESForm::LookupByID<RE::TESBoundObject>(formID)));
        }
        if (a_filter.type == "Package") {
            return Result(
                a_npc &&
                std::ranges::any_of(
                    a_npc->aiPackages.packages,
                    [formID](const RE::TESPackage* a_package) {
                        return a_package &&
                            a_package->GetFormID() == formID;
                    }));
        }
        if (a_filter.type == "Hair" ||
            a_filter.type == "Facial Hair" ||
            a_filter.type.starts_with("HeadPart ")) {
            if (!a_npc || !a_npc->headParts) {
                return FilterEvaluation::kNoMatch;
            }
            for (std::int8_t index = 0;
                 index < a_npc->numHeadParts;
                 ++index) {
                if (a_npc->headParts[index] &&
                    a_npc->headParts[index]->GetFormID() == formID) {
                    return FilterEvaluation::kMatch;
                }
            }
            return FilterEvaluation::kNoMatch;
        }
        if (a_filter.type == "Location") {
            auto* target =
                RE::TESForm::LookupByID<RE::BGSLocation>(formID);
            for (auto* location =
                     a_actor ? a_actor->GetCurrentLocation() : nullptr;
                 target && location;
                 location = location->parentLoc) {
                if (location == target) {
                    return FilterEvaluation::kMatch;
                }
            }
            return FilterEvaluation::kNoMatch;
        }
        if (a_filter.type == "Cell") {
            const auto* cell =
                a_actor ? a_actor->GetParentCell() : nullptr;
            return Result(cell && cell->GetFormID() == formID);
        }
        if (a_filter.type == "Worldspace") {
            const auto* worldspace =
                a_actor ? a_actor->GetWorldspace() : nullptr;
            return Result(
                worldspace && worldspace->GetFormID() == formID);
        }
        if (a_filter.type == "Location Keyword") {
            auto* keyword =
                RE::TESForm::LookupByID<RE::BGSKeyword>(formID);
            for (auto* location =
                     a_actor ? a_actor->GetCurrentLocation() : nullptr;
                 keyword && location;
                 location = location->parentLoc) {
                if (location->HasKeyword(keyword)) {
                    return FilterEvaluation::kMatch;
                }
            }
            return FilterEvaluation::kNoMatch;
        }
        if (a_filter.type == "Quest") {
            return Result(MatchesQuest(
                a_actor,
                RE::TESForm::LookupByID<RE::TESQuest>(formID),
                a_filter));
        }
        return FilterEvaluation::kNotHandled;
    }

    FilterEvaluation EvaluateFilter(
        RE::Actor* a_actor,
        RE::TESNPC* a_npc,
        const BlacklistFilter& a_filter,
        const FilterEvaluationServices& a_services)
    {
        const auto result = EvaluateFilterRaw(
            a_actor, a_npc, a_filter, a_services);
        if (result == FilterEvaluation::kNotHandled ||
            result == FilterEvaluation::kInvalid ||
            !a_filter.isNot) {
            return result;
        }
        return result == FilterEvaluation::kMatch ?
            FilterEvaluation::kNoMatch : FilterEvaluation::kMatch;
    }
}
