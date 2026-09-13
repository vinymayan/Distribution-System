#pragma once

#include "RE/T/TESForm.h"

#include <cstdint>

namespace SkillMenuAPI
{
    inline constexpr auto Name = "SkillMenuAPI";
    inline constexpr std::uint32_t Version = 5;

    struct SkillListView
    {
        const char* const* items;
        std::uint32_t count;
    };

    struct Interface
    {
        std::uint32_t interfaceVersion;

        int (*GetCustomSkillLevel)(const char* skillId);
        void (*AddCustomSkillXP)(const char* skillId, float xpAmount);
        float (*GetCustomSkillXP)(const char* skillId);
        float (*GetSkillFormulaValue)(const char* skillId, int valueType);

        int (*GetCustomSkillTotalLevel)(const char* skillId);
        int (*GetCustomSkillBonus)(const char* skillId);
        void (*ModCustomSkillBonus)(const char* skillId, int amount);
        void (*SetCustomSkillBonus)(const char* skillId, int amount);

        void (*AddCustomSkillXPForActor)(
            RE::FormID actorFormID,
            const char* skillId,
            float xpAmount);
        int (*GetCustomSkillLevelForActor)(
            RE::FormID actorFormID,
            const char* skillId);
        float (*GetCustomSkillXPForActor)(
            RE::FormID actorFormID,
            const char* skillId);
        int (*GetCustomSkillTotalLevelForActor)(
            RE::FormID actorFormID,
            const char* skillId);
        int (*GetCustomSkillBonusForActor)(
            RE::FormID actorFormID,
            const char* skillId);
        void (*ModCustomSkillBonusForActor)(
            RE::FormID actorFormID,
            const char* skillId,
            int amount);
        void (*SetCustomSkillBonusForActor)(
            RE::FormID actorFormID,
            const char* skillId,
            int amount);
        bool (*HasCustomPerkForActor)(
            RE::FormID actorFormID,
            const char* perkId);
        bool (*AddCustomPerkForActor)(
            RE::FormID actorFormID,
            const char* perkId);
        bool (*RemoveCustomPerkForActor)(
            RE::FormID actorFormID,
            const char* perkId);

        int (*GetActorPerkPoints)(RE::FormID actorFormID);
        int (*ModActorPerkPoints)(RE::FormID actorFormID, int amount);
        float (*GetActorResource)(
            RE::FormID actorFormID,
            const char* resourceId);
        bool (*ModActorResource)(
            RE::FormID actorFormID,
            const char* resourceId,
            float amount);

        SkillListView (*GetAvailableSkills)();
        SkillListView (*GetAvailableResources)();

        void (*ModCustomSkillLevelForActor)(
            RE::FormID actorFormID,
            const char* skillId,
            int amount);
        void (*SetCustomSkillLevelForActor)(
            RE::FormID actorFormID,
            const char* skillId,
            int level);
    };
}
