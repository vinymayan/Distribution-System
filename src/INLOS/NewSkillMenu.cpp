#include "INLOS/NewSkillMenu.h"
#include "SkillMenuAPI.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
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

        SkillMenuAPI::Interface* GetInterfaceSnapshot()
        {
            std::scoped_lock lock(g_lock);
            return g_interface;
        }

        std::pair<std::size_t, std::size_t> GetListSizes()
        {
            std::scoped_lock lock(g_lock);
            return { g_skills.size(), g_resources.size() };
        }

        bool RefreshSkillsFromInterface(
            SkillMenuAPI::Interface* a_interface)
        {
            if (!a_interface ||
                a_interface->interfaceVersion < SkillMenuAPI::Version ||
                !a_interface->GetAvailableSkills) {
                return false;
            }

            // IMPORTANT: never hold INLOS::g_lock while crossing into
            // SkillMenu.dll. A callback/re-entrant path in NSM could otherwise
            // wait on INLOS while INLOS is waiting on NSM, creating an AB/BA
            // deadlock across DLL boundaries.
            auto values = CopyListView(
                a_interface->GetAvailableSkills());
            const auto nextRefresh =
                std::chrono::steady_clock::now() +
                (values.empty() ?
                    std::chrono::seconds(1) :
                    std::chrono::seconds(30));

            std::scoped_lock lock(g_lock);
            if (g_interface != a_interface) {
                return false;
            }
            g_skills = std::move(values);
            g_nextSkillRefresh = nextRefresh;
            return true;
        }

        bool RefreshResourcesFromInterface(
            SkillMenuAPI::Interface* a_interface)
        {
            if (!a_interface ||
                a_interface->interfaceVersion < SkillMenuAPI::Version ||
                !a_interface->GetAvailableResources) {
                return false;
            }

            auto values = CopyListView(
                a_interface->GetAvailableResources());
            const auto nextRefresh =
                std::chrono::steady_clock::now() +
                (values.empty() ?
                    std::chrono::seconds(1) :
                    std::chrono::seconds(30));

            std::scoped_lock lock(g_lock);
            if (g_interface != a_interface) {
                return false;
            }
            g_resources = std::move(values);
            g_nextResourceRefresh = nextRefresh;
            return true;
        }

        bool HasSkillSnapshot(const std::string_view a_skillID)
        {
            if (a_skillID.empty()) {
                return false;
            }
            std::scoped_lock lock(g_lock);
            return std::ranges::binary_search(
                g_skills,
                std::string(a_skillID));
        }

        bool HasResourceSnapshot(const std::string_view a_resourceID)
        {
            if (a_resourceID.empty()) {
                return false;
            }
            std::scoped_lock lock(g_lock);
            return std::ranges::binary_search(
                g_resources,
                std::string(a_resourceID));
        }
    }

    bool Initialize()
    {
        {
            std::scoped_lock lock(g_lock);
            if (g_interface) {
                return true;
            }
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

        // Cross the DLL boundary without holding the INLOS mutex.
        auto* candidate =
            static_cast<SkillMenuAPI::Interface*>(getter());
        if (!candidate ||
            candidate->interfaceVersion < SkillMenuAPI::Version) {
            logger::warn(
                "[INLOS] New Skill Menu API v{} or newer is required.",
                SkillMenuAPI::Version);
            return false;
        }

        SkillMenuAPI::Interface* activeInterface = nullptr;
        bool newlyConnected = false;
        {
            std::scoped_lock lock(g_lock);
            if (!g_interface) {
                g_interface = candidate;
                newlyConnected = true;
            }
            activeInterface = g_interface;
        }

        if (newlyConnected) {
            RefreshSkillsFromInterface(activeInterface);
            const auto [skillCount, resourceCount] = GetListSizes();
            (void)resourceCount;
            logger::info(
                "[INLOS] New Skill Menu API v{} connected ({} custom skills).",
                activeInterface->interfaceVersion,
                skillCount);
        }
        return activeInterface != nullptr;
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

        auto* api = GetInterfaceSnapshot();
        const auto skillsRefreshed =
            RefreshSkillsFromInterface(api);
        const auto resourcesRefreshed =
            RefreshResourcesFromInterface(api);

        if (skillsRefreshed || resourcesRefreshed) {
            const auto [skillCount, resourceCount] = GetListSizes();
            logger::info(
                "[INLOS] NSM lists refreshed ({} skills, {} resources).",
                skillCount,
                resourceCount);
        }
        return skillsRefreshed && resourcesRefreshed;
    }

    std::vector<std::string> AvailableSkills()
    {
        if (!IsAvailable() && !Initialize()) {
            return {};
        }

        auto* api = GetInterfaceSnapshot();
        bool shouldRefresh = false;
        {
            std::scoped_lock lock(g_lock);
            shouldRefresh =
                g_interface && g_skills.empty() &&
                std::chrono::steady_clock::now() >=
                g_nextSkillRefresh;
        }
        if (shouldRefresh) {
            RefreshSkillsFromInterface(api);
        }

        std::scoped_lock lock(g_lock);
        return g_skills;
    }

    bool HasSkill(const std::string_view a_skillID)
    {
        if (a_skillID.empty() || !Initialize()) {
            return false;
        }

        auto* api = GetInterfaceSnapshot();
        bool shouldRefresh = false;
        {
            std::scoped_lock lock(g_lock);
            shouldRefresh = g_skills.empty();
        }
        if (shouldRefresh) {
            RefreshSkillsFromInterface(api);
        }
        return HasSkillSnapshot(a_skillID);
    }

    std::vector<std::string> AvailableResources()
    {
        if (!IsAvailable() && !Initialize()) {
            return {};
        }

        auto* api = GetInterfaceSnapshot();
        bool shouldRefresh = false;
        {
            std::scoped_lock lock(g_lock);
            shouldRefresh =
                g_interface && g_resources.empty() &&
                std::chrono::steady_clock::now() >=
                g_nextResourceRefresh;
        }
        if (shouldRefresh) {
            RefreshResourcesFromInterface(api);
        }

        std::scoped_lock lock(g_lock);
        return g_resources;
    }

    bool HasResource(const std::string_view a_resourceID)
    {
        if (a_resourceID.empty() || !Initialize()) {
            return false;
        }

        auto* api = GetInterfaceSnapshot();
        bool shouldRefresh = false;
        {
            std::scoped_lock lock(g_lock);
            shouldRefresh = g_resources.empty();
        }
        if (shouldRefresh) {
            RefreshResourcesFromInterface(api);
        }
        return HasResourceSnapshot(a_resourceID);
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

        auto* api = GetInterfaceSnapshot();
        if (!api || !api->AddCustomSkillXPForActor) {
            return false;
        }

        const std::string skillID(a_skillID);
        api->AddCustomSkillXPForActor(
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

        auto* api = GetInterfaceSnapshot();
        if (!api || !api->ModCustomSkillBonusForActor) {
            return false;
        }

        const std::string skillID(a_skillID);
        api->ModCustomSkillBonusForActor(
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

        auto* api = GetInterfaceSnapshot();
        if (!api || !api->ModCustomSkillLevelForActor) {
            return false;
        }

        const std::string skillID(a_skillID);
        api->ModCustomSkillLevelForActor(
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

        auto* api = GetInterfaceSnapshot();
        if (!api || !api->ModActorPerkPoints) {
            return false;
        }

        api->ModActorPerkPoints(
            a_actorID,
            a_amount);
        return true;
    }

    bool AddResource(
        const RE::FormID a_actorID,
        const std::string_view a_resourceID,
        const float a_amount)
    {
        if (!std::isfinite(a_amount) ||
            a_amount <= 0.0f ||
            !HasResource(a_resourceID)) {
            return false;
        }

        auto* api = GetInterfaceSnapshot();
        if (!api || !api->ModActorResource) {
            return false;
        }

        const std::string resourceID(a_resourceID);
        return api->ModActorResource(
            a_actorID,
            resourceID.c_str(),
            a_amount);
    }
}
