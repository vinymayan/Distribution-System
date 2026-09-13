#include "DistributionCore/Domain.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace DistributionCore
{
    bool TypeRegistry::Register(TypeDescriptor a_descriptor)
    {
        if (a_descriptor.id.empty()) {
            return false;
        }

        std::unique_lock lock(_lock);
        const auto existing = _types.find(a_descriptor.id);
        if (existing != _types.end()) {
            existing->second = std::move(a_descriptor);
            return false;
        }
        const auto key = a_descriptor.id;
        _types.emplace(key, std::move(a_descriptor));
        return true;
    }

    const TypeDescriptor* TypeRegistry::Find(
        const std::string_view a_id) const
    {
        std::shared_lock lock(_lock);
        const auto found = _types.find(a_id);
        return found != _types.end() ?
            std::addressof(found->second) :
            nullptr;
    }

    std::vector<TypeDescriptor> TypeRegistry::AvailableFor(
        const Domain a_domain) const
    {
        std::shared_lock lock(_lock);
        std::vector<TypeDescriptor> result;
        const auto domain = ToMask(a_domain);
        for (const auto& [id, type] : _types) {
            if ((type.domains & domain) != 0) {
                result.push_back(type);
            }
        }
        return result;
    }

    bool TypeRegistry::Supports(
        const std::string_view a_id,
        const Domain a_domain) const
    {
        const auto* descriptor = Find(a_id);
        return descriptor &&
            (descriptor->domains & ToMask(a_domain)) != 0;
    }

    TypeRegistry& FilterRegistry()
    {
        static TypeRegistry registry;
        return registry;
    }

    TypeRegistry& RewardRegistry()
    {
        static TypeRegistry registry;
        return registry;
    }

    void RegisterBuiltInTypes()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            constexpr auto form =
                ToMask(TypeCapability::kRequiresForm);
            constexpr auto numeric =
                ToMask(TypeCapability::kNumeric);
            constexpr auto physical =
                ToMask(TypeCapability::kRequiresForm) |
                ToMask(TypeCapability::kPhysicalItem);
            constexpr auto equipment =
                physical | ToMask(TypeCapability::kEquipment);

            for (const auto* type : {
                     "NPC", "Faction", "Keyword", "Race", "Perk",
                     "Spell", "Shout", "Cell", "Location",
                     "Worldspace", "Location Keyword", "Quest",
                     "Inventory Item", "Equipped Item", "Leveled NPC",
                     "Combat Style", "Voice Type", "Class", "Skin",
                     "Package", "Hair", "Facial Hair", "HeadPart Misc",
                     "HeadPart Face", "HeadPart Eyes", "HeadPart Scar",
                     "HeadPart Eyebrows" }) {
                FilterRegistry().Register(
                    { type, type, kAllDomains, form });
            }
            for (const auto* type : {
                     "Actor Value", "Inventory Count", "Gold", "Height", "Weight",
                     "Faction Rank", "Relationship Rank" }) {
                FilterRegistry().Register(
                    { type, type, kAllDomains, numeric });
            }
            for (const auto* type : {
                     "Source Plugin", "NPC Trait", "Cell Type", "City Status",
                     "Equipped Category" }) {
                FilterRegistry().Register(
                    { type, type, kAllDomains, 0 });
            }

            for (const auto* type : {
                     "Potion", "Ingredient", "Scroll", "Book", "Misc",
                     "SoulGem", "Key", "Gold", "Leveled Item" }) {
                RewardRegistry().Register(
                    { type, type, kAllDomains, physical });
            }
            for (const auto* type : {
                     "Weapon", "Armor", "Ammo", "Light" }) {
                RewardRegistry().Register(
                    { type, type, kAllDomains, equipment });
            }
            for (const auto* type : { "Spell", "Perk" }) {
                RewardRegistry().Register(
                    { type, type, kAllDomains, form });
            }
            for (const auto* type : {
                     "Shout", "Keyword", "Faction", "Outfit" }) {
                RewardRegistry().Register(
                    { type, type, ToMask(Domain::kEDF), form });
            }
            RewardRegistry().Register(
                { "Experience", "Experience", ToMask(Domain::kINLOS),
                    ToMask(TypeCapability::kOneShotOnly) |
                    ToMask(TypeCapability::kNumeric) });
            RewardRegistry().Register(
                { "Actor Value", "Actor Value",
                    ToMask(Domain::kEDF),
                    ToMask(TypeCapability::kNumeric) });
            RewardRegistry().Register(
                { "Actor Scale", "Actor Scale",
                    ToMask(Domain::kEDF),
                    ToMask(TypeCapability::kNumeric) });
            RewardRegistry().Register(
                { "Skill Experience", "Vanilla Skill Experience",
                    ToMask(Domain::kWIYT),
                    ToMask(TypeCapability::kOneShotOnly) |
                    ToMask(TypeCapability::kNumeric) });
            for (const auto* type : {
                     "NSM Skill Experience",
                     "NSM Skill Bonus" }) {
                RewardRegistry().Register(
                    { type, type,
                        ToMask(Domain::kWIYT),
                        ToMask(
                            TypeCapability::kOneShotOnly) |
                            ToMask(
                                TypeCapability::kNumeric) });
            }
            RewardRegistry().Register(
                { "NSM Perk Points", "NSM Perk Points",
                    ToMask(Domain::kWIYT),
                    ToMask(TypeCapability::kOneShotOnly) |
                        ToMask(TypeCapability::kNumeric) });
            for (const auto* type : {
                     "Skill XP", "Skill Level", "Perk Points" }) {
                RewardRegistry().Register(
                    { type, type, ToMask(Domain::kINLOS),
                        ToMask(TypeCapability::kOneShotOnly) |
                            ToMask(TypeCapability::kNumeric) });
            }
            RewardRegistry().Register(
                { "NSM Resource", "NSM Resource",
                    ToMask(Domain::kINLOS),
                    ToMask(TypeCapability::kOneShotOnly) |
                        ToMask(TypeCapability::kNumeric) });
        });
    }

    std::vector<ResolvedItem> ResolveLeveledItems(
        RE::Actor* a_subject,
        RE::TESLevItem* a_list,
        const std::uint32_t a_count)
    {
        std::vector<ResolvedItem> result;
        if (!a_subject || !a_list || a_count == 0) {
            return result;
        }

        RE::BSScrapArray<RE::CALCED_OBJECT> calculated;
        const auto actorLevel = static_cast<std::uint16_t>(
            std::clamp(
                static_cast<int>(a_subject->GetLevel()),
                1,
                0xFFFF));
        const auto requestedCount = static_cast<std::int16_t>(
            std::min<std::uint32_t>(
                a_count,
                static_cast<std::uint32_t>(
                    std::numeric_limits<std::int16_t>::max())));
        a_list->CalculateCurrentFormList(
            actorLevel,
            requestedCount,
            calculated,
            0,
            false);

        std::map<RE::FormID, ResolvedItem> aggregated;
        for (const auto& entry : calculated) {
            auto* item =
                entry.form ? entry.form->As<RE::TESBoundObject>() : nullptr;
            if (!item || item->As<RE::TESLevItem>() ||
                entry.count == 0) {
                continue;
            }
            auto& aggregate = aggregated[item->GetFormID()];
            aggregate.item = item;
            aggregate.count += entry.count;
        }
        for (const auto& [formID, item] : aggregated) {
            result.push_back(item);
        }
        return result;
    }
}
