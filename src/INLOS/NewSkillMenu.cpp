#include "INLOS/NewSkillMenu.h"
#include "SkillMenuAPI.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <windows.h>

namespace INLOS::NewSkillMenu
{
    namespace
    {
        using GetInterface = void* (*)();

        std::mutex g_lock;
        SkillMenuAPI::Interface* g_interface = nullptr;
        std::vector<std::string> g_skills;
        std::vector<std::string> g_resources;
        std::chrono::steady_clock::time_point g_nextSkillRefresh{};
        std::chrono::steady_clock::time_point g_nextResourceRefresh{};

        std::vector<std::string> CopyListView(
            const SkillMenuAPI::SkillListView a_view)
        {
            std::vector<std::string> values;
            values.reserve(a_view.count);
            for (std::uint32_t index = 0;
                 index < a_view.count;
                 ++index) {
                if (a_view.items && a_view.items[index] &&
                    a_view.items[index][0] != '\0') {
                    values.emplace_back(a_view.items[index]);
                }
            }
            std::ranges::sort(values);
            values.erase(
                std::unique(values.begin(), values.end()),
                values.end());
            return values;
        }

        bool HasSkillLocked(const std::string_view a_skillID)
        {
            return !a_skillID.empty() &&
                std::ranges::binary_search(
                    g_skills,
                    std::string(a_skillID));
        }

        bool RefreshSkillsLocked()
        {
            if (!g_interface ||
                g_interface->interfaceVersion <
                    SkillMenuAPI::Version ||
                !g_interface->GetAvailableSkills) {
                return false;
            }
            const auto view =
                g_interface->GetAvailableSkills();
            g_skills = CopyListView(view);
            g_nextSkillRefresh =
                std::chrono::steady_clock::now() +
                (g_skills.empty() ?
                    std::chrono::seconds(1) :
                    std::chrono::seconds(30));
            return true;
        }

        bool RefreshResourcesLocked()
        {
            if (!g_interface ||
                g_interface->interfaceVersion <
                    SkillMenuAPI::Version ||
                !g_interface->GetAvailableResources) {
                return false;
            }
            g_resources = CopyListView(
                g_interface->GetAvailableResources());
            g_nextResourceRefresh =
                std::chrono::steady_clock::now() +
                (g_resources.empty() ?
                    std::chrono::seconds(1) :
                    std::chrono::seconds(30));
            return true;
        }
    }

    bool Initialize()
    {
        std::scoped_lock lock(g_lock);
        if (g_interface) {
            return true;
        }
        auto* module = GetModuleHandleA("SkillMenu.dll");
        if (!module) {
            return false;
        }
        const auto getter = reinterpret_cast<GetInterface>(
            GetProcAddress(module, "GetSkillMenuAPI"));
        if (!getter) {
            logger::warn(
                "[INLOS] SkillMenu.dll does not export GetSkillMenuAPI.");
            return false;
        }
        auto* candidate =
            static_cast<SkillMenuAPI::Interface*>(getter());
        if (!candidate ||
            candidate->interfaceVersion <
                SkillMenuAPI::Version) {
            logger::warn(
                "[INLOS] New Skill Menu API v{} or newer is required.",
                SkillMenuAPI::Version);
            return false;
        }
        g_interface = candidate;
        RefreshSkillsLocked();
        logger::info(
            "[INLOS] New Skill Menu API v{} connected ({} custom skills).",
            g_interface->interfaceVersion,
            g_skills.size());
        return true;
    }

    bool IsAvailable()
    {
        std::scoped_lock lock(g_lock);
        return g_interface != nullptr;
    }

    std::uint32_t InterfaceVersion()
    {
        std::scoped_lock lock(g_lock);
        return g_interface ?
            g_interface->interfaceVersion :
            0;
    }

    bool RefreshSkills()
    {
        if (!Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        const auto skillsRefreshed = RefreshSkillsLocked();
        const auto resourcesRefreshed = RefreshResourcesLocked();
        if (skillsRefreshed || resourcesRefreshed) {
            logger::info(
                "[INLOS] NSM lists refreshed ({} skills, {} resources).",
                g_skills.size(),
                g_resources.size());
        }
        return skillsRefreshed && resourcesRefreshed;
    }

    const std::vector<std::string>& AvailableSkills()
    {
        if (!IsAvailable()) {
            Initialize();
        }
        std::scoped_lock lock(g_lock);
        if (g_interface && g_skills.empty() &&
            std::chrono::steady_clock::now() >=
                g_nextSkillRefresh) {
            RefreshSkillsLocked();
        }
        return g_skills;
    }

    bool HasSkill(const std::string_view a_skillID)
    {
        if (!Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (g_skills.empty()) {
            RefreshSkillsLocked();
        }
        return HasSkillLocked(a_skillID);
    }

    const std::vector<std::string>& AvailableResources()
    {
        if (!IsAvailable()) {
            Initialize();
        }
        std::scoped_lock lock(g_lock);
        if (g_interface && g_resources.empty() &&
            std::chrono::steady_clock::now() >=
                g_nextResourceRefresh) {
            RefreshResourcesLocked();
        }
        return g_resources;
    }

    bool HasResource(const std::string_view a_resourceID)
    {
        if (a_resourceID.empty() || !Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (g_resources.empty()) {
            RefreshResourcesLocked();
        }
        return std::ranges::binary_search(
            g_resources,
            std::string(a_resourceID));
    }

    bool AddSkillExperience(
        const RE::FormID a_actorID,
        const std::string_view a_skillID,
        const float a_amount)
    {
        if (!std::isfinite(a_amount) ||
            a_amount <= 0.0f ||
            !HasSkill(a_skillID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->AddCustomSkillXPForActor) {
            return false;
        }
        const std::string skillID(a_skillID);
        g_interface->AddCustomSkillXPForActor(
            a_actorID,
            skillID.c_str(),
            a_amount);
        return true;
    }

    bool AddSkillBonus(
        const RE::FormID a_actorID,
        const std::string_view a_skillID,
        const int a_amount)
    {
        if (a_amount == 0 || !HasSkill(a_skillID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModCustomSkillBonusForActor) {
            return false;
        }
        const std::string skillID(a_skillID);
        g_interface->ModCustomSkillBonusForActor(
            a_actorID,
            skillID.c_str(),
            a_amount);
        return true;
    }

    bool AddSkillLevel(
        const RE::FormID a_actorID,
        const std::string_view a_skillID,
        const int a_amount)
    {
        if (a_amount == 0 || !HasSkill(a_skillID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModCustomSkillLevelForActor) {
            return false;
        }
        const std::string skillID(a_skillID);
        g_interface->ModCustomSkillLevelForActor(
            a_actorID,
            skillID.c_str(),
            a_amount);
        return true;
    }

    bool AddPerkPoints(
        const RE::FormID a_actorID,
        const int a_amount)
    {
        if (a_amount == 0 || !Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModActorPerkPoints) {
            return false;
        }
        g_interface->ModActorPerkPoints(
            a_actorID,
            a_amount);
        return true;
    }

    bool AddResource(
        const RE::FormID a_actorID,
        const std::string_view a_resourceID,
        const float a_amount)
    {
        if (!std::isfinite(a_amount) || a_amount <= 0.0f ||
            !HasResource(a_resourceID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModActorResource) {
            return false;
        }
        const std::string resourceID(a_resourceID);
        return g_interface->ModActorResource(
            a_actorID,
            resourceID.c_str(),
            a_amount);
    }
}
