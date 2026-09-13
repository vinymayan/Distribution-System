#pragma once

#include <Rule.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace INLOS
{
    enum class SkillSource : std::uint8_t
    {
        kInvalid = 0,
        kVanilla,
        kNSM
    };

    enum class SkillLevelMode : std::int32_t
    {
        kPermanent = 0,
        kBonus = 1
    };

    inline constexpr std::string_view kVanillaSkillPrefix = "Vanilla|";
    inline constexpr std::string_view kNSMSkillPrefix = "NSM|";

    [[nodiscard]] constexpr SkillSource GetSkillSource(
        const std::string_view a_reference)
    {
        if (a_reference.starts_with(kVanillaSkillPrefix)) {
            return SkillSource::kVanilla;
        }
        if (a_reference.starts_with(kNSMSkillPrefix)) {
            return SkillSource::kNSM;
        }
        return SkillSource::kInvalid;
    }

    [[nodiscard]] constexpr std::string_view GetSkillID(
        const std::string_view a_reference)
    {
        switch (GetSkillSource(a_reference)) {
        case SkillSource::kVanilla:
            return a_reference.substr(kVanillaSkillPrefix.size());
        case SkillSource::kNSM:
            return a_reference.substr(kNSMSkillPrefix.size());
        default:
            return {};
        }
    }

    [[nodiscard]] inline std::string MakeSkillReference(
        const SkillSource a_source,
        const std::string_view a_skillID)
    {
        switch (a_source) {
        case SkillSource::kVanilla:
            return std::string(kVanillaSkillPrefix) + std::string(a_skillID);
        case SkillSource::kNSM:
            return std::string(kNSMSkillPrefix) + std::string(a_skillID);
        default:
            return {};
        }
    }

    static_assert(GetSkillSource("Vanilla|OneHanded") == SkillSource::kVanilla);
    static_assert(GetSkillID("NSM|CustomSkill") == "CustomSkill");

    enum class Trigger : std::uint8_t
    {
        kDeath = 0,
        kDefeat = 1,
        kBoth = 2
    };

    enum class Destination : std::uint8_t
    {
        kVictim = 0,
        kPlayer = 1
    };

    struct LootRule
    {
        Rule criteria;
        Trigger trigger = Trigger::kDeath;
        Destination destination = Destination::kPlayer;
        bool requirePlayerKiller = false;
    };

    struct Package
    {
        std::string id;
        std::string displayName;
        bool enabled = true;
        std::filesystem::path path;
        int schemaVersion = 1;
    };
}
