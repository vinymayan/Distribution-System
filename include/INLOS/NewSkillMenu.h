#pragma once

#include "RE/T/TESForm.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace INLOS::NewSkillMenu
{
    bool Initialize();
    bool IsAvailable();
    std::uint32_t InterfaceVersion();

    bool RefreshSkills();
    std::vector<std::string> AvailableSkills();
    bool HasSkill(std::string_view a_skillID);
    std::vector<std::string> AvailableResources();
    bool HasResource(std::string_view a_resourceID);

    bool AddSkillExperience(
        RE::FormID a_actorID,
        std::string_view a_skillID,
        float a_amount);
    bool AddSkillBonus(
        RE::FormID a_actorID,
        std::string_view a_skillID,
        int a_amount);
    bool AddSkillLevel(
        RE::FormID a_actorID,
        std::string_view a_skillID,
        int a_amount);
    bool AddPerkPoints(
        RE::FormID a_actorID,
        int a_amount);
    bool AddResource(
        RE::FormID a_actorID,
        std::string_view a_resourceID,
        float a_amount);
}
