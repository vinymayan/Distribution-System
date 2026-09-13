#include "WIYT/UI.h"

#include "DistributionCore/Domain.h"
#include "DistributionCore/UICommon.h"
#include "INLOS/NewSkillMenu.h"
#include "Manager.h"
#include "WIYT/DFGBridge.h"
#include "WIYT/Runtime.h"
#include "WIYT/Settings.h"
#include "WIYT/State.h"
#include "WIYT/Store.h"

#include "SKSEMCP/SKSEMenuFramework.hpp"

#include <array>
#include <numeric>

namespace WIYT::UI
{
    namespace
    {
        std::string g_activePackage =
            std::string(Store::kLocalPackageID);
        std::string g_packageFilter;
        std::string g_activeTitle;
        std::string g_activeRequirement;
        char g_newPackageName[128]{};
        char g_newTitleName[128]{};
        char g_selectionSearch[128]{};
        std::string g_selectionType = "All";
        std::string g_selectionPlugin = "All";
        bool g_requirementsOpen = false;
        bool g_rewardsOpen = false;
        bool g_filterPickerOpen = false;
        std::optional<FilterScope> g_filterScope;
        int g_activeRewardGroup = -1;
        bool g_rewardPickerOpen = false;

        std::vector<BlacklistFilter>& FiltersForScope(
            Requirement& a_requirement,
            const FilterScope a_scope)
        {
            switch (a_scope) {
            case FilterScope::kPlayerPrerequisite:
                return a_requirement.playerPrerequisiteFilters;
            case FilterScope::kTargetActor:
                return a_requirement.targetActorFilters;
            case FilterScope::kSourceForm:
                return a_requirement.sourceFormFilters;
            case FilterScope::kEnvironment:
            default:
                return a_requirement.environmentFilters;
            }
        }

        const char* ScopeLabel(const FilterScope a_scope)
        {
            switch (a_scope) {
            case FilterScope::kPlayerPrerequisite:
                return "Player Prerequisites";
            case FilterScope::kTargetActor:
                return "Target / Victim Filters";
            case FilterScope::kSourceForm:
                return "Source Item / Spell Filters";
            case FilterScope::kEnvironment:
            default:
                return "Environment Filters";
            }
        }

        const char* ScopeDescription(const FilterScope a_scope)
        {
            switch (a_scope) {
            case FilterScope::kPlayerPrerequisite:
                return "Conditions checked on the player.";
            case FilterScope::kTargetActor:
                return "Conditions checked on the event target or victim.";
            case FilterScope::kSourceForm:
                return "The item, spell, quest, or form that originated the event.";
            case FilterScope::kEnvironment:
            default:
                return "The city status, Cell, Location, Worldspace, or Location Keyword where the event occurred.";
            }
        }

        const char* RequirementSourceLabel(
            const Requirement& a_requirement)
        {
            return ToString(a_requirement.source);
        }

        bool RequirementDefinitionValid(
            const Requirement& a_requirement)
        {
            const auto valid = [&](
                                   const FilterScope a_scope,
                                   const auto& a_filters) {
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
            };
            return valid(
                       FilterScope::kPlayerPrerequisite,
                       a_requirement.playerPrerequisiteFilters) &&
                valid(
                       FilterScope::kTargetActor,
                       a_requirement.targetActorFilters) &&
                valid(
                       FilterScope::kSourceForm,
                       a_requirement.sourceFormFilters) &&
                valid(
                       FilterScope::kEnvironment,
                       a_requirement.environmentFilters);
        }

        std::string UniquePublicGlobalName(
            const TitleDefinition& a_title,
            const std::string_view a_titleName)
        {
            auto candidate = SanitizePublicGlobalName(a_titleName);
            const auto duplicate = std::ranges::any_of(
                Store::GetSingleton()->Titles(),
                [&](const TitleDefinition& a_other) {
                    return a_other.id != a_title.id &&
                        _stricmp(
                            a_other.publicGlobalEditorID.c_str(),
                            candidate.c_str()) == 0;
                });
            if (duplicate) {
                candidate += "_" + a_title.id.substr(0, 8);
            }
            return candidate;
        }

        bool InputString(
            const char* a_label,
            std::string& a_value,
            const std::size_t a_capacity = 256)
        {
            std::vector<char> buffer(
                std::max(
                    a_capacity,
                    a_value.size() + 2),
                '\0');
            std::ranges::copy(a_value, buffer.begin());
            if (!ImGuiMCP::InputText(
                    a_label,
                    buffer.data(),
                    buffer.size())) {
                return false;
            }
            a_value = buffer.data();
            return true;
        }

        template <class Enum>
        bool EnumCombo(
            const char* a_label,
            Enum& a_value,
            const std::initializer_list<
                std::pair<Enum, const char*>> a_options)
        {
            const auto current = std::ranges::find_if(
                a_options,
                [&](const auto& a_option) {
                    return a_option.first == a_value;
                });
            const auto* preview =
                current != a_options.end() ?
                current->second :
                "Unknown";
            if (!ImGuiMCP::BeginCombo(a_label, preview)) {
                return false;
            }
            bool changed = false;
            for (const auto& [value, label] : a_options) {
                const auto selected = value == a_value;
                if (ImGuiMCP::Selectable(label, selected)) {
                    a_value = value;
                    changed = true;
                }
                if (selected) {
                    ImGuiMCP::SetItemDefaultFocus();
                }
            }
            ImGuiMCP::EndCombo();
            return changed;
        }

        std::string FormReference(const InternalFormInfo& a_info)
        {
            if (_stricmp(
                    a_info.pluginName.c_str(),
                    "Dynamic") == 0) {
                return std::format(
                    "Dynamic|{:X}",
                    a_info.formID);
            }
            const auto localID =
                (a_info.formID & 0xFF000000) == 0xFE000000 ?
                (a_info.formID & 0x00000FFF) :
                (a_info.formID & 0x00FFFFFF);
            return std::format(
                "{}|{:X}",
                a_info.pluginName,
                localID);
        }

        bool DrawFormPicker(
            const char* a_label,
            const std::string& a_type,
            std::string& a_editorID,
            std::string& a_formID,
            const std::string_view a_stateID)
        {
            const auto& forms =
                Manager::GetSingleton()->GetList(a_type);
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            options.reserve(forms.size());
            for (const auto& form : forms) {
                const auto value = !form.editorID.empty() ?
                    form.editorID :
                    FormReference(form);
                options.push_back({
                    value,
                    std::format(
                        "{} [{}] {}|{:08X}",
                        form.GetDisplayName(),
                        form.editorID.empty() ?
                            "No EditorID" :
                            form.editorID,
                        form.pluginName,
                        form.formID)
                });
            }
            std::string selected =
                !a_editorID.empty() ?
                a_editorID :
                a_formID;
            const auto preview =
                selected.empty() ?
                "Select..." :
                selected.c_str();
            if (!DistributionCore::UI::DrawSearchableCombo(
                    a_label,
                    preview,
                    a_stateID,
                    options,
                    selected,
                    Manager::GetSingleton()->GetListRevision())) {
                return false;
            }
            const auto found = std::ranges::find_if(
                forms,
                [&](const InternalFormInfo& a_form) {
                    return (!a_form.editorID.empty() ?
                               a_form.editorID :
                               FormReference(a_form)) ==
                        selected;
                });
            if (found == forms.end()) {
                return false;
            }
            a_editorID = found->editorID;
            a_formID = FormReference(*found);
            return true;
        }

        bool MatchesForm(
            const BlacklistFilter& a_filter,
            const InternalFormInfo& a_info)
        {
            return a_filter.type == a_info.formType &&
                ((!a_filter.editorID.empty() &&
                     _stricmp(
                         a_filter.editorID.c_str(),
                         a_info.editorID.c_str()) == 0) ||
                    a_filter.formIDStr == FormReference(a_info));
        }

        bool MatchesForm(
            const Reward& a_reward,
            const InternalFormInfo& a_info)
        {
            return a_reward.typeReward == a_info.formType &&
                ((!a_reward.editorID.empty() &&
                     _stricmp(
                         a_reward.editorID.c_str(),
                         a_info.editorID.c_str()) == 0) ||
                    a_reward.formIDStr == FormReference(a_info));
        }

        BlacklistFilter MakeFilter(
            const InternalFormInfo& a_info)
        {
            BlacklistFilter filter;
            filter.type = a_info.formType;
            filter.editorID = a_info.editorID;
            filter.formIDStr = FormReference(a_info);
            filter.optionText = a_info.name;
            if (filter.type == "Source Plugin") {
                filter.optionText = a_info.editorID;
            }
            else if (filter.type == "NPC Trait" ||
                     filter.type == "Cell Type" ||
                     filter.type == "City Status" ||
                     filter.type == "Equipped Category") {
                filter.optionMode = std::max(
                    0,
                    static_cast<int>(a_info.formID) - 1);
            }
            if (IsNumericValueFilterType(filter.type)) {
                filter.comparison =
                    NumericComparison::kGreaterOrEqual;
                filter.minimumValue =
                    filter.type == "Faction Rank" ? 0.0f : 1.0f;
                filter.maximumValue = filter.minimumValue;
            }
            return filter;
        }

        void ResetSelectionState()
        {
            g_selectionSearch[0] = '\0';
            g_selectionType = "All";
            g_selectionPlugin = "All";
        }

        void DrawNSMSkillPicker(
            Reward& a_reward,
            const std::string_view a_stateID)
        {
            const auto& skills =
                INLOS::NewSkillMenu::AvailableSkills();
            if (!skills.empty()) {
                std::vector<
                    DistributionCore::UI::SearchableComboOption>
                    options;
                options.reserve(skills.size());
                for (const auto& skill : skills) {
                    options.push_back({ skill, skill });
                }
                DistributionCore::UI::DrawSearchableCombo(
                    "##NSMSkill",
                    a_reward.editorID.empty() ?
                        "Select Skill Tree..." :
                        a_reward.editorID.c_str(),
                    a_stateID,
                    options,
                    a_reward.editorID,
                    (static_cast<std::uint64_t>(
                         INLOS::NewSkillMenu::InterfaceVersion()) << 32) |
                        skills.size());
            }
            else {
                InputString("##NSMSkill", a_reward.editorID);
                ImGuiMCP::TextDisabled(
                    "Manual Skill ID (NSM API list unavailable)");
            }
        }

        void DrawNumericComparison(BlacklistFilter& a_filter)
        {
            ImGuiMCP::SetNextItemWidth(120.0f);
            EnumCombo(
                "Operator",
                a_filter.comparison,
                {
                    {
                        NumericComparison::kGreaterOrEqual,
                        ">="
                    },
                    {
                        NumericComparison::kLessOrEqual,
                        "<="
                    },
                    { NumericComparison::kEqual, "=" },
                    {
                        NumericComparison::kBetween,
                        "Between"
                    }
                });
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(120.0f);
            ImGuiMCP::InputFloat(
                "Value",
                &a_filter.minimumValue,
                1.0f,
                10.0f,
                "%.2f");
            if (a_filter.comparison ==
                NumericComparison::kBetween) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(120.0f);
                ImGuiMCP::InputFloat(
                    "Maximum",
                    &a_filter.maximumValue,
                    1.0f,
                    10.0f,
                    "%.2f");
            }
        }

        void DrawFilter(
            BlacklistFilter& a_filter,
            const std::string_view a_stateID,
            bool& a_remove)
        {
            DistributionCore::UI::DrawFilterOperator(
                "Match", a_filter);
            const auto descriptors =
                DistributionCore::FilterRegistry().AvailableFor(
                    DistributionCore::Domain::kWIYT);
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            options.reserve(descriptors.size());
            for (const auto& descriptor : descriptors) {
                options.push_back({
                    descriptor.id,
                    descriptor.displayName
                });
            }
            auto selected = a_filter.type;
            if (DistributionCore::UI::DrawSearchableCombo(
                    "Type",
                    a_filter.type.empty() ?
                        "Select..." :
                        a_filter.type.c_str(),
                    std::format("{}_type", a_stateID),
                    options,
                    selected,
                    options.size())) {
                const auto isNot = a_filter.isNot;
                a_filter = {};
                a_filter.type = selected;
                a_filter.isNot = isNot;
            }
            const auto* descriptor =
                DistributionCore::FilterRegistry().Find(
                    a_filter.type);
            if (descriptor &&
                (descriptor->capabilities &
                    DistributionCore::ToMask(
                        DistributionCore::TypeCapability::
                            kRequiresForm)) != 0) {
                DrawFormPicker(
                    "Form",
                    a_filter.type,
                    a_filter.editorID,
                    a_filter.formIDStr,
                    std::format("{}_form", a_stateID));
                InputString(
                    "EditorID",
                    a_filter.editorID);
            }
            if (a_filter.type == "Actor Value") {
                InputString(
                    "Actor Value",
                    a_filter.actorValueName);
                EnumCombo(
                    "Value Mode",
                    a_filter.actorValueMode,
                    {
                        {
                            ActorValueMode::kCurrent,
                            "Current"
                        },
                        {
                            ActorValueMode::kPermanent,
                            "Permanent"
                        },
                        {
                            ActorValueMode::kMaximum,
                            "Maximum"
                        }
                    });
            }
            if (descriptor &&
                (descriptor->capabilities &
                    DistributionCore::ToMask(
                        DistributionCore::TypeCapability::
                            kNumeric)) != 0) {
                DrawNumericComparison(a_filter);
            }
            if (a_filter.type == "Source Plugin") {
                InputString("Plugin", a_filter.optionText);
            }
            if (ImGuiMCP::Button("Remove Filter")) {
                a_remove = true;
            }
        }

        void DrawFilterScope(
            const char* a_label,
            std::vector<BlacklistFilter>& a_filters,
            bool& a_requiresAll,
            const std::string_view a_stateID)
        {
            if (!ImGuiMCP::CollapsingHeader(a_label)) {
                return;
            }
            ImGuiMCP::Checkbox(
                "Require all filters (AND)",
                &a_requiresAll);
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Add Filter")) {
                a_filters.emplace_back();
            }
            for (std::size_t index = 0;
                 index < a_filters.size();) {
                ImGuiMCP::PushID(
                    static_cast<int>(index));
                ImGuiMCP::Separator();
                bool remove = false;
                DrawFilter(
                    a_filters[index],
                    std::format("{}_{}", a_stateID, index),
                    remove);
                ImGuiMCP::PopID();
                if (remove) {
                    a_filters.erase(
                        a_filters.begin() +
                        static_cast<std::ptrdiff_t>(index));
                }
                else {
                    ++index;
                }
            }
        }

        void DrawSelectionTable(
            std::vector<BlacklistFilter>* a_filters,
            RewardGroup* a_group,
            const std::optional<FilterScope> a_scope = std::nullopt,
            const ActivityType a_activity = ActivityType::kCustom)
        {
            const auto rewardMode = a_group != nullptr;
            const auto descriptors = rewardMode ?
                DistributionCore::RewardRegistry().AvailableFor(
                    DistributionCore::Domain::kWIYT) :
                DistributionCore::FilterRegistry().AvailableFor(
                    DistributionCore::Domain::kWIYT);

            ImGuiMCP::SetNextItemWidth(200.0f);
            ImGuiMCP::InputText(
                "Search",
                g_selectionSearch,
                sizeof(g_selectionSearch));
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(200.0f);
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                typeOptions{
                    { "All", "All" },
                    { "Selected", "Selected" }
                };
            for (const auto& descriptor : descriptors) {
                if (!rewardMode && a_scope &&
                    !IsFilterAllowedForScope(
                        *a_scope,
                        a_activity,
                        descriptor.id)) {
                    continue;
                }
                if (!Manager::GetSingleton()
                         ->GetList(descriptor.id)
                         .empty()) {
                    typeOptions.push_back({
                        descriptor.id,
                        descriptor.displayName
                    });
                }
            }
            DistributionCore::UI::DrawSearchableCombo(
                "##Type",
                g_selectionType.c_str(),
                rewardMode ?
                    "WIYT.RewardType" :
                    "WIYT.FilterType",
                typeOptions,
                g_selectionType,
                Manager::GetSingleton()->GetListRevision());

            static bool playableOnly = true;
            if (rewardMode) {
                ImGuiMCP::SameLine();
                ImGuiMCP::Checkbox(
                    "Playable Forms Only",
                    &playableOnly);
            }

            std::vector<const InternalFormInfo*> source;
            if (g_selectionType == "Selected") {
                if (rewardMode) {
                    for (const auto& reward : a_group->rewards) {
                        const auto& list =
                            Manager::GetSingleton()->GetList(
                                reward.typeReward);
                        const auto found = std::ranges::find_if(
                            list,
                            [&](const InternalFormInfo& a_info) {
                                return MatchesForm(reward, a_info);
                            });
                        if (found != list.end()) {
                            source.push_back(std::addressof(*found));
                        }
                    }
                }
                else if (a_filters) {
                    for (const auto& filter : *a_filters) {
                        const auto& list =
                            Manager::GetSingleton()->GetList(
                                filter.type);
                        const auto found = std::ranges::find_if(
                            list,
                            [&](const InternalFormInfo& a_info) {
                                return MatchesForm(filter, a_info);
                            });
                        if (found != list.end()) {
                            source.push_back(std::addressof(*found));
                        }
                    }
                }
            }
            else {
                for (const auto& descriptor : descriptors) {
                    if (!rewardMode && a_scope &&
                        !IsFilterAllowedForScope(
                            *a_scope,
                            a_activity,
                            descriptor.id)) {
                        continue;
                    }
                    if (g_selectionType != "All" &&
                        descriptor.id != g_selectionType) {
                        continue;
                    }
                    for (const auto& info :
                         Manager::GetSingleton()->GetList(
                             descriptor.id)) {
                        source.push_back(std::addressof(info));
                    }
                }
            }

            std::set<std::string> plugins;
            for (const auto* info : source) {
                if (info && !info->pluginName.empty()) {
                    plugins.emplace(info->pluginName);
                }
            }
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                pluginOptions{ { "All", "All Plugins" } };
            for (const auto& plugin : plugins) {
                pluginOptions.push_back({ plugin, plugin });
            }
            if (g_selectionPlugin != "All" &&
                !plugins.contains(g_selectionPlugin)) {
                g_selectionPlugin = "All";
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(170.0f);
            DistributionCore::UI::DrawSearchableCombo(
                "##Plugin",
                g_selectionPlugin == "All" ?
                    "All Plugins" :
                    g_selectionPlugin.c_str(),
                rewardMode ?
                    "WIYT.RewardPlugin" :
                    "WIYT.FilterPlugin",
                pluginOptions,
                g_selectionPlugin,
                Manager::GetSingleton()->GetListRevision());

            std::string search = g_selectionSearch;
            std::ranges::transform(
                search,
                search.begin(),
                [](const unsigned char a_character) {
                    return static_cast<char>(
                        std::tolower(a_character));
                });
            std::vector<std::size_t> visible;
            visible.reserve(source.size());
            for (std::size_t index = 0;
                 index < source.size();
                 ++index) {
                const auto* info = source[index];
                if (!info ||
                    (rewardMode && playableOnly && !info->playable) ||
                    (g_selectionPlugin != "All" &&
                        info->pluginName != g_selectionPlugin)) {
                    continue;
                }
                auto searchable = info->name + " " +
                    info->editorID;
                std::ranges::transform(
                    searchable,
                    searchable.begin(),
                    [](const unsigned char a_character) {
                        return static_cast<char>(
                            std::tolower(a_character));
                    });
                if (search.empty() || searchable.contains(search)) {
                    visible.push_back(index);
                }
            }

            ImGuiMCP::ImVec2 available;
            ImGuiMCP::GetContentRegionAvail(&available);
            const auto columns = rewardMode ? 7 : 6;
            if (!ImGuiMCP::BeginTable(
                    "SelectionTable",
                    columns,
                    ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable |
                        ImGuiMCP::ImGuiTableFlags_ScrollY,
                    { 0.0f, available.y })) {
                return;
            }
            ImGuiMCP::TableSetupColumn(
                "Active",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                60.0f);
            ImGuiMCP::TableSetupColumn(
                "FormID",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                95.0f);
            ImGuiMCP::TableSetupColumn(
                "Name",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                "Type",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                130.0f);
            ImGuiMCP::TableSetupColumn(
                "Plugin",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                rewardMode ? "Qty" : "Value",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                rewardMode ? 60.0f : 90.0f);
            if (rewardMode) {
                ImGuiMCP::TableSetupColumn(
                    "Chance",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    70.0f);
            }
            ImGuiMCP::TableHeadersRow();

            static auto clipper =
                ImGuiMCP::ImGuiListClipperManager::Create();
            ImGuiMCP::ImGuiListClipperManager::Begin(
                clipper,
                static_cast<int>(visible.size()),
                -1.0f);
            while (ImGuiMCP::ImGuiListClipperManager::Step(
                clipper)) {
                for (auto row = clipper->DisplayStart;
                     row < clipper->DisplayEnd;
                     ++row) {
                    const auto* info = source[visible[
                        static_cast<std::size_t>(row)]];
                    ImGuiMCP::PushID(
                        std::format(
                            "{}:{}",
                            info->formType,
                            info->formID).c_str());
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    if (rewardMode) {
                        auto found = std::ranges::find_if(
                            a_group->rewards,
                            [&](const Reward& a_reward) {
                                return MatchesForm(a_reward, *info);
                            });
                        auto selected =
                            found != a_group->rewards.end();
                        if (ImGuiMCP::Checkbox(
                                "##Active",
                                &selected)) {
                            if (selected) {
                                Reward reward;
                                reward.typeReward = info->formType;
                                reward.formIDStr = FormReference(*info);
                                reward.editorID = info->editorID;
                                a_group->rewards.push_back(
                                    std::move(reward));
                            }
                            else if (found !=
                                     a_group->rewards.end()) {
                                a_group->rewards.erase(found);
                            }
                        }
                    }
                    else {
                        auto found = std::ranges::find_if(
                            *a_filters,
                            [&](const BlacklistFilter& a_filter) {
                                return MatchesForm(a_filter, *info);
                            });
                        auto selected = found != a_filters->end();
                        if (ImGuiMCP::Checkbox(
                                "##Active",
                                &selected)) {
                            if (selected) {
                                a_filters->push_back(MakeFilter(*info));
                            }
                            else if (found != a_filters->end()) {
                                a_filters->erase(found);
                            }
                        }
                    }
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::Text("%08X", info->formID);
                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::TextUnformatted(
                        info->GetDisplayName().c_str());
                    ImGuiMCP::TableSetColumnIndex(3);
                    ImGuiMCP::TextUnformatted(
                        info->formType.c_str());
                    ImGuiMCP::TableSetColumnIndex(4);
                    ImGuiMCP::TextUnformatted(
                        info->pluginName.c_str());
                    ImGuiMCP::TableSetColumnIndex(5);
                    ImGuiMCP::TextDisabled("-");
                    if (rewardMode) {
                        ImGuiMCP::TableSetColumnIndex(6);
                        ImGuiMCP::TextDisabled("-");
                    }
                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::EndTable();
        }

        void DrawStatisticPicker(Requirement& a_requirement)
        {
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            for (const auto& statistic : KnownStatistics()) {
                options.push_back({ statistic, statistic });
            }
            auto selected = a_requirement.statisticName;
            if (DistributionCore::UI::DrawSearchableCombo(
                    "Statistic",
                    selected.empty() ?
                        "Select..." :
                        selected.c_str(),
                    std::format(
                        "stat_{}",
                        a_requirement.id),
                    options,
                    selected,
                    options.size())) {
                a_requirement.statisticName = selected;
            }
            InputString(
                "Custom Statistic Name",
                a_requirement.statisticName);
            ImGuiMCP::Text(
                "Cached value: %.2f",
                GetCachedStatistic(
                    a_requirement.statisticName));
        }

        void DrawRequirement(
            Requirement& a_requirement,
            bool& a_remove)
        {
            InputString("Name", a_requirement.name);
            EnumCombo(
                "Progress Source",
                a_requirement.source,
                {
                    {
                        ProgressSource::kVanillaStatistic,
                        "Vanilla Statistic"
                    },
                    {
                        ProgressSource::kEventCounter,
                        "WIYT Event Counter"
                    },
                    {
                        ProgressSource::kGlobal,
                        "Global Variable"
                    },
                    {
                        ProgressSource::kGraphVariable,
                        "Player Graph Variable"
                    }
                });
            if (a_requirement.source ==
                ProgressSource::kVanillaStatistic) {
                DrawStatisticPicker(a_requirement);
            }
            else if (a_requirement.source ==
                     ProgressSource::kEventCounter) {
                EnumCombo(
                    "Activity",
                    a_requirement.activity,
                    {
                        {
                            ActivityType::kActorKilled,
                            "Actor Killed"
                        },
                        {
                            ActivityType::kActorDefeated,
                            "Actor Defeated"
                        },
                        {
                            ActivityType::kItemHarvested,
                            "Item Harvested"
                        },
                        {
                            ActivityType::kItemAcquired,
                            "Item Acquired"
                        },
                        {
                            ActivityType::kDamageDealt,
                            "Damage Dealt"
                        },
                        {
                            ActivityType::kSpellDamageDealt,
                            "Spell Damage Dealt"
                        },
                        {
                            ActivityType::kItemCrafted,
                            "Item Crafted"
                        },
                        {
                            ActivityType::kLocationDiscovered,
                            "Location Discovered"
                        },
                        {
                            ActivityType::kQuestCompleted,
                            "Quest Completed"
                        },
                        {
                            ActivityType::kGoldEarned,
                            "Gold Earned"
                        },
                        {
                            ActivityType::kGoldSpent,
                            "Gold Spent"
                        },
                        {
                            ActivityType::kCustom,
                            "Custom"
                        }
                    });
            }
            else if (a_requirement.source ==
                     ProgressSource::kGlobal) {
                DrawFormPicker(
                    "Global",
                    "Global",
                    a_requirement.referenceEditorID,
                    a_requirement.referenceFormID,
                    std::format(
                        "requirement_global_{}",
                        a_requirement.id));
            }
            else {
                InputString(
                    "Graph Variable",
                    a_requirement.graphVariableName);
                const char* types[]{
                    "Bool",
                    "Int",
                    "Float"
                };
                ImGuiMCP::Combo(
                    "Graph Value Type",
                    &a_requirement.graphVariableType,
                    types,
                    3);
            }
            EnumCombo(
                "Tracking Mode",
                a_requirement.trackingMode,
                {
                    {
                        TrackingMode::kLifetimeTotal,
                        "Lifetime Total"
                    },
                    {
                        TrackingMode::kSinceActivated,
                        "Since Title Activated"
                    },
                    {
                        TrackingMode::kHighestReached,
                        "Highest Reached"
                    }
                });
            if (a_requirement.source ==
                ProgressSource::kEventCounter) {
                EnumCombo(
                    "Aggregation",
                    a_requirement.aggregation,
                    {
                        { Aggregation::kCount, "Count" },
                        { Aggregation::kSum, "Sum" },
                        {
                            Aggregation::kUniqueCount,
                            "Unique Count"
                        },
                        {
                            Aggregation::kHighestValue,
                            "Highest Value"
                        }
                    });
            }
            ImGuiMCP::InputFloat(
                "Required Value",
                &a_requirement.targetAmount,
                1.0f,
                10.0f,
                "%.2f");
            a_requirement.targetAmount =
                std::max(0.0001f, a_requirement.targetAmount);

            ImGuiMCP::Separator();
            ImGuiMCP::TextUnformatted("Player Prerequisites");
            EnumCombo(
                "Prerequisite Mode",
                a_requirement.prerequisiteMode,
                {
                    {
                        PrerequisiteMode::kRequiredToCount,
                        "Required to Count Events"
                    },
                    {
                        PrerequisiteMode::kRequiredToComplete,
                        "Required to Complete"
                    }
                });
            if (ImGuiMCP::Button(
                    std::format(
                        "Player Prerequisites ({})##PlayerPrerequisites",
                        a_requirement.playerPrerequisiteFilters.size()).c_str())) {
                g_filterScope =
                    FilterScope::kPlayerPrerequisite;
                g_filterPickerOpen = false;
                ResetSelectionState();
            }
            ImGuiMCP::TextDisabled(
                "Empty means no player prerequisite is required.");

            if (a_requirement.source ==
                ProgressSource::kEventCounter) {
                ImGuiMCP::TextUnformatted("Event Authorship");
                ImGuiMCP::Checkbox(
                    "Allow Active Followers",
                    &a_requirement.allowFollowerActions);
                ImGuiMCP::SameLine();
                ImGuiMCP::Checkbox(
                    "Allow Player Summons",
                    &a_requirement.allowSummonActions);
                ImGuiMCP::TextDisabled(
                    "Direct player actions are always allowed. Global WIYT settings remain master switches.");
                ImGuiMCP::Separator();
                ImGuiMCP::TextUnformatted("Event Filters");
                for (const auto scope : {
                         FilterScope::kTargetActor,
                         FilterScope::kSourceForm,
                         FilterScope::kEnvironment }) {
                    const auto& filters = FiltersForScope(
                        a_requirement, scope);
                    if (ImGuiMCP::Button(
                            std::format(
                                "{} ({})##Scope{}",
                                ScopeLabel(scope),
                                filters.size(),
                                static_cast<int>(scope)).c_str())) {
                        g_filterScope = scope;
                        g_filterPickerOpen = false;
                        ResetSelectionState();
                    }
                    if (scope != FilterScope::kEnvironment) {
                        ImGuiMCP::SameLine();
                    }
                }
                ImGuiMCP::Checkbox(
                    "Require ALL filters (AND)",
                    &a_requirement.filtersRequireAll);
            }
            if (ImGuiMCP::Button("Delete Requirement")) {
                a_remove = true;
            }
        }

        void DrawFilterWorkspace(Requirement& a_requirement)
        {
            if (!g_filterScope) {
                return;
            }
            auto& filters = FiltersForScope(
                a_requirement, *g_filterScope);
            if (g_filterPickerOpen) {
                if (ImGuiMCP::Button("Back")) {
                    g_filterPickerOpen = false;
                }
                DrawSelectionTable(
                    std::addressof(filters),
                    nullptr,
                    g_filterScope,
                    a_requirement.activity);
                return;
            }

            if (ImGuiMCP::Button("Back")) {
                g_filterScope.reset();
                return;
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::TextUnformatted(
                ScopeLabel(*g_filterScope));
            ImGuiMCP::TextDisabled(
                ScopeDescription(*g_filterScope));
            ImGuiMCP::Checkbox(
                "Require ALL filters (AND)",
                &a_requirement.filtersRequireAll);
            ImGuiMCP::Separator();

            if (ImGuiMCP::Button("Add New Filter")) {
                ResetSelectionState();
                g_filterPickerOpen = true;
            }
            if (IsFilterAllowedForScope(
                    *g_filterScope,
                    a_requirement.activity,
                    "Actor Value")) {
                ImGuiMCP::SameLine();
            }
            if (IsFilterAllowedForScope(
                    *g_filterScope,
                    a_requirement.activity,
                    "Actor Value") &&
                ImGuiMCP::Button("+ Actor Value")) {
                BlacklistFilter filter;
                filter.type = "Actor Value";
                filter.actorValueName = "Health";
                filter.actorValueMode = ActorValueMode::kMaximum;
                filter.comparison =
                    NumericComparison::kGreaterOrEqual;
                filter.minimumValue = 100.0f;
                filters.push_back(std::move(filter));
            }
            if (IsFilterAllowedForScope(
                    *g_filterScope,
                    a_requirement.activity,
                    "Height")) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("+ Height")) {
                    BlacklistFilter filter;
                    filter.type = "Height";
                    filter.comparison = NumericComparison::kGreaterOrEqual;
                    filter.minimumValue = 1.0f;
                    filter.maximumValue = 1.0f;
                    filters.push_back(std::move(filter));
                }
            }
            if (IsFilterAllowedForScope(
                    *g_filterScope,
                    a_requirement.activity,
                    "Weight")) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("+ Weight")) {
                    BlacklistFilter filter;
                    filter.type = "Weight";
                    filter.comparison = NumericComparison::kGreaterOrEqual;
                    filter.minimumValue = 50.0f;
                    filter.maximumValue = 50.0f;
                    filters.push_back(std::move(filter));
                }
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::BeginCombo(
                    "+ Special Filter", "Select...")) {
                for (const auto& descriptor :
                     DistributionCore::FilterRegistry().AvailableFor(
                         DistributionCore::Domain::kWIYT)) {
                    if (descriptor.id == "Actor Value" ||
                        descriptor.id == "Height" ||
                        descriptor.id == "Weight" ||
                        !IsFilterAllowedForScope(
                            *g_filterScope,
                            a_requirement.activity,
                            descriptor.id) ||
                        !Manager::GetSingleton()
                             ->GetList(descriptor.id)
                             .empty()) {
                        continue;
                    }
                    if (ImGuiMCP::Selectable(
                            descriptor.displayName.c_str(), false)) {
                        BlacklistFilter filter;
                        filter.type = descriptor.id;
                        filters.push_back(std::move(filter));
                    }
                }
                ImGuiMCP::EndCombo();
            }

            if (!ImGuiMCP::BeginTable(
                    "WIYTFilters",
                    7,
                    ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable |
                        ImGuiMCP::ImGuiTableFlags_ScrollY)) {
                return;
            }
            ImGuiMCP::TableSetupColumn(
                "Match",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                100.0f);
            ImGuiMCP::TableSetupColumn(
                "Type",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                135.0f);
            ImGuiMCP::TableSetupColumn(
                "Name / Form",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                "Condition",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                400.0f);
            ImGuiMCP::TableSetupColumn(
                "Identifier",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                "Status",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                150.0f);
            ImGuiMCP::TableSetupColumn(
                "Action",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                55.0f);
            ImGuiMCP::TableHeadersRow();
            for (std::size_t index = 0;
                 index < filters.size();) {
                auto& filter = filters[index];
                ImGuiMCP::PushID(static_cast<int>(index));
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                DistributionCore::UI::DrawFilterOperator(
                    "##FilterOperator", filter);
                ImGuiMCP::TableSetColumnIndex(1);
                ImGuiMCP::TextUnformatted(filter.type.c_str());
                ImGuiMCP::TableSetColumnIndex(2);
                const auto* descriptor =
                    DistributionCore::FilterRegistry().Find(
                        filter.type);
                const auto requiresForm = descriptor &&
                    (descriptor->capabilities &
                        DistributionCore::ToMask(
                            DistributionCore::TypeCapability::
                                kRequiresForm)) != 0;
                if (requiresForm) {
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    DrawFormPicker(
                        "##Form",
                        filter.type,
                        filter.editorID,
                        filter.formIDStr,
                        std::format(
                            "wiyt_filter_{}_{}",
                            a_requirement.id,
                            index));
                }
                else if (filter.type == "Actor Value") {
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    InputString(
                        "##ActorValue", filter.actorValueName);
                }
                else if (filter.type == "Height" || filter.type == "Weight") {
                    ImGuiMCP::TextUnformatted("NPC base value");
                }
                else if (filter.type == "City Status") {
                    ImGuiMCP::TextUnformatted("Event location");
                }
                else {
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    InputString("##Option", filter.optionText);
                }
                ImGuiMCP::TableSetColumnIndex(3);
                if (descriptor &&
                    (descriptor->capabilities &
                        DistributionCore::ToMask(
                            DistributionCore::TypeCapability::
                                kNumeric)) != 0) {
                    DrawNumericComparison(filter);
                }
                else if (filter.type == "City Status") {
                    ImGuiMCP::SetNextItemWidth(170.0f);
                    EnumCombo(
                        "##CityStatus",
                        filter.optionMode,
                        { { 0, "Inside a City" },
                          { 1, "Outside a City" } });
                }
                else {
                    ImGuiMCP::TextDisabled("-");
                }
                ImGuiMCP::TableSetColumnIndex(4);
                ImGuiMCP::TextUnformatted(
                    !filter.editorID.empty() ?
                        filter.editorID.c_str() :
                        filter.formIDStr.c_str());
                ImGuiMCP::TableSetColumnIndex(5);
                const auto compatible =
                    IsFilterAllowedForScope(
                        *g_filterScope,
                        a_requirement.activity,
                        filter.type) &&
                    (filter.type != "City Status" ||
                        (filter.optionMode >= 0 &&
                         filter.optionMode <= 1));
                if (compatible) {
                    ImGuiMCP::TextColored(
                        { 0.3f, 0.9f, 0.4f, 1.0f },
                        "VALID");
                }
                else {
                    ImGuiMCP::TextColored(
                        { 1.0f, 0.4f, 0.3f, 1.0f },
                        "INCOMPATIBLE SCOPE");
                }
                ImGuiMCP::TableSetColumnIndex(6);
                if (ImGuiMCP::Button("X")) {
                    filters.erase(
                        filters.begin() +
                        static_cast<std::ptrdiff_t>(index));
                    ImGuiMCP::PopID();
                    continue;
                }
                ImGuiMCP::PopID();
                ++index;
            }
            ImGuiMCP::EndTable();
        }

        void DrawRequirementsWorkspace(TitleDefinition& a_title)
        {
            auto found = std::ranges::find(
                a_title.requirements,
                g_activeRequirement,
                &Requirement::id);
            if (found != a_title.requirements.end()) {
                if (g_filterScope) {
                    DrawFilterWorkspace(*found);
                    return;
                }
                if (ImGuiMCP::Button("Back")) {
                    g_activeRequirement.clear();
                    return;
                }
                ImGuiMCP::SameLine();
                ImGuiMCP::TextUnformatted(found->name.c_str());
                ImGuiMCP::Separator();
                bool remove = false;
                DrawRequirement(*found, remove);
                if (remove) {
                    a_title.requirements.erase(found);
                    g_activeRequirement.clear();
                }
                return;
            }
            g_activeRequirement.clear();
            if (ImGuiMCP::Button("Back")) {
                g_requirementsOpen = false;
                return;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ New Requirement")) {
                Requirement requirement;
                requirement.id = GenerateUUID();
                g_activeRequirement = requirement.id;
                a_title.requirements.push_back(
                    std::move(requirement));
                return;
            }
            ImGuiMCP::Separator();
            if (!ImGuiMCP::BeginTable(
                    "Requirements",
                    8,
                    ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable |
                        ImGuiMCP::ImGuiTableFlags_ScrollY)) {
                return;
            }
            ImGuiMCP::TableSetupColumn(
                "Name",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                "Source",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                150.0f);
            ImGuiMCP::TableSetupColumn(
                "Activity",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                150.0f);
            ImGuiMCP::TableSetupColumn(
                "Tracking",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                150.0f);
            ImGuiMCP::TableSetupColumn(
                "Aggregation",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                130.0f);
            ImGuiMCP::TableSetupColumn(
                "Required",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                100.0f);
            ImGuiMCP::TableSetupColumn(
                "Filters",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                80.0f);
            ImGuiMCP::TableSetupColumn(
                "Action",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                100.0f);
            ImGuiMCP::TableHeadersRow();
            for (auto& requirement : a_title.requirements) {
                ImGuiMCP::PushID(requirement.id.c_str());
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                if (RequirementDefinitionValid(requirement)) {
                    ImGuiMCP::TextUnformatted(requirement.name.c_str());
                }
                else {
                    ImGuiMCP::TextColored(
                        { 1.0f, 0.4f, 0.3f, 1.0f },
                        "%s [INVALID FILTER SCOPE]",
                        requirement.name.c_str());
                }
                ImGuiMCP::TableSetColumnIndex(1);
                ImGuiMCP::TextUnformatted(
                    RequirementSourceLabel(requirement));
                ImGuiMCP::TableSetColumnIndex(2);
                ImGuiMCP::TextUnformatted(
                    requirement.source ==
                            ProgressSource::kEventCounter ?
                        ToString(requirement.activity) :
                        "-");
                ImGuiMCP::TableSetColumnIndex(3);
                ImGuiMCP::TextUnformatted(
                    ToString(requirement.trackingMode));
                ImGuiMCP::TableSetColumnIndex(4);
                ImGuiMCP::TextUnformatted(
                    requirement.source ==
                            ProgressSource::kEventCounter ?
                        ToString(requirement.aggregation) :
                        "-");
                ImGuiMCP::TableSetColumnIndex(5);
                ImGuiMCP::Text("%.2f", requirement.targetAmount);
                ImGuiMCP::TableSetColumnIndex(6);
                ImGuiMCP::Text(
                    "%zu",
                    requirement.playerPrerequisiteFilters.size() +
                        requirement.targetActorFilters.size() +
                        requirement.sourceFormFilters.size() +
                        requirement.environmentFilters.size());
                ImGuiMCP::TableSetColumnIndex(7);
                if (ImGuiMCP::Button("Edit")) {
                    g_activeRequirement = requirement.id;
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
        }

        void DrawReward(
            Reward& a_reward,
            const std::string_view a_stateID,
            bool& a_remove)
        {
            const auto descriptors =
                DistributionCore::RewardRegistry().AvailableFor(
                    DistributionCore::Domain::kWIYT);
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            for (const auto& descriptor : descriptors) {
                options.push_back({
                    descriptor.id,
                    descriptor.displayName
                });
            }
            auto selected = a_reward.typeReward;
            if (DistributionCore::UI::DrawSearchableCombo(
                    "Reward Type",
                    selected.empty() ?
                        "Select..." :
                        selected.c_str(),
                    std::format("{}_type", a_stateID),
                    options,
                    selected,
                    options.size())) {
                a_reward = {};
                a_reward.typeReward = selected;
            }
            const auto* descriptor =
                DistributionCore::RewardRegistry().Find(
                    a_reward.typeReward);
            if (a_reward.typeReward == "Skill Experience") {
                InputString(
                    "Actor Value",
                    a_reward.editorID);
            }
            else if (a_reward.typeReward ==
                         "NSM Skill Experience" ||
                     a_reward.typeReward ==
                         "NSM Skill Bonus") {
                DrawNSMSkillPicker(
                    a_reward,
                    std::format("{}_nsm", a_stateID));
            }
            else if (descriptor &&
                     (descriptor->capabilities &
                         DistributionCore::ToMask(
                             DistributionCore::
                                 TypeCapability::
                                     kRequiresForm)) != 0) {
                DrawFormPicker(
                    "Form",
                    a_reward.typeReward,
                    a_reward.editorID,
                    a_reward.formIDStr,
                    std::format("{}_form", a_stateID));
                InputString("EditorID", a_reward.editorID);
            }
            auto amount = static_cast<int>(a_reward.amount);
            if (ImGuiMCP::InputInt("Amount", &amount)) {
                a_reward.amount = static_cast<std::uint32_t>(
                    std::max(1, amount));
            }
            ImGuiMCP::InputFloat(
                "Chance",
                &a_reward.chanceReward,
                1.0f,
                10.0f,
                "%.2f%%");
            a_reward.chanceReward = std::clamp(
                a_reward.chanceReward,
                0.0f,
                100.0f);
            if (ImGuiMCP::Button("Delete Reward")) {
                a_remove = true;
            }
        }

        void DrawRewards(TitleDefinition& a_title)
        {
            if (g_rewardPickerOpen &&
                g_activeRewardGroup >= 0 &&
                static_cast<std::size_t>(g_activeRewardGroup) <
                    a_title.rewardGroups.size()) {
                if (ImGuiMCP::Button("Back")) {
                    g_rewardPickerOpen = false;
                }
                DrawSelectionTable(
                    nullptr,
                    std::addressof(a_title.rewardGroups[
                        static_cast<std::size_t>(
                            g_activeRewardGroup)]));
                return;
            }
            if (ImGuiMCP::Button("Back")) {
                g_rewardsOpen = false;
                return;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ New Reward Group")) {
                a_title.rewardGroups.emplace_back();
            }
            ImGuiMCP::Separator();
            for (std::size_t groupIndex = 0;
                 groupIndex < a_title.rewardGroups.size();) {
                auto& group =
                    a_title.rewardGroups[groupIndex];
                ImGuiMCP::PushID(
                    static_cast<int>(groupIndex));
                const auto open = ImGuiMCP::CollapsingHeader(
                    std::format(
                        "{}##Group",
                        group.name).c_str(),
                    ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen);
                bool removeGroup = false;
                if (open) {
                    InputString("Group Name", group.name);
                    ImGuiMCP::Checkbox(
                        "Exclusive Group",
                        &group.isExclusive);
                    ImGuiMCP::InputFloat(
                        "Group Chance",
                        &group.chanceGroup,
                        1.0f,
                        10.0f,
                        "%.2f%%");
                    group.chanceGroup = std::clamp(
                        group.chanceGroup,
                        0.0f,
                        100.0f);
                    if (ImGuiMCP::Button("Add Form Rewards")) {
                        g_activeRewardGroup =
                            static_cast<int>(groupIndex);
                        g_rewardPickerOpen = true;
                        ResetSelectionState();
                    }
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::BeginCombo(
                            "+ Special Reward", "Select...")) {
                        for (const auto& descriptor :
                             DistributionCore::RewardRegistry().AvailableFor(
                                 DistributionCore::Domain::kWIYT)) {
                            if (!Manager::GetSingleton()
                                     ->GetList(descriptor.id)
                                     .empty()) {
                                continue;
                            }
                            if (ImGuiMCP::Selectable(
                                    descriptor.displayName.c_str(), false)) {
                                Reward reward;
                                reward.typeReward = descriptor.id;
                                group.rewards.push_back(
                                    std::move(reward));
                            }
                        }
                        ImGuiMCP::EndCombo();
                    }

                    if (ImGuiMCP::BeginTable(
                            "RewardTable",
                            6,
                            ImGuiMCP::ImGuiTableFlags_Borders |
                                ImGuiMCP::ImGuiTableFlags_RowBg |
                                ImGuiMCP::ImGuiTableFlags_Resizable)) {
                        ImGuiMCP::TableSetupColumn(
                            "Type",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            140.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Reward",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableSetupColumn(
                            "Identifier",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableSetupColumn(
                            "Qty",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            65.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Chance",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            85.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Action",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            55.0f);
                        ImGuiMCP::TableHeadersRow();
                        for (std::size_t rewardIndex = 0;
                             rewardIndex < group.rewards.size();) {
                            auto& reward =
                                group.rewards[rewardIndex];
                            ImGuiMCP::PushID(
                                static_cast<int>(rewardIndex));
                            ImGuiMCP::TableNextRow();
                            ImGuiMCP::TableSetColumnIndex(0);
                            ImGuiMCP::TextUnformatted(
                                reward.typeReward.c_str());
                            ImGuiMCP::TableSetColumnIndex(1);
                            if (reward.typeReward ==
                                    "NSM Skill Experience" ||
                                reward.typeReward ==
                                    "NSM Skill Bonus") {
                                ImGuiMCP::SetNextItemWidth(-1.0f);
                                DrawNSMSkillPicker(
                                    reward,
                                    std::format(
                                        "wiyt_nsm_{}_{}_{}",
                                        a_title.id,
                                        groupIndex,
                                        rewardIndex));
                            }
                            else if (reward.typeReward ==
                                     "Skill Experience") {
                                ImGuiMCP::SetNextItemWidth(-1.0f);
                                InputString(
                                    "##ActorValue",
                                    reward.editorID);
                            }
                            else if (const auto* descriptor =
                                         DistributionCore::RewardRegistry().Find(
                                             reward.typeReward);
                                     descriptor &&
                                     (descriptor->capabilities &
                                         DistributionCore::ToMask(
                                             DistributionCore::TypeCapability::
                                                 kRequiresForm)) != 0) {
                                ImGuiMCP::SetNextItemWidth(-1.0f);
                                DrawFormPicker(
                                    "##Form",
                                    reward.typeReward,
                                    reward.editorID,
                                    reward.formIDStr,
                                    std::format(
                                        "wiyt_reward_{}_{}_{}",
                                        a_title.id,
                                        groupIndex,
                                        rewardIndex));
                            }
                            else {
                                ImGuiMCP::TextDisabled("Numeric Reward");
                            }
                            ImGuiMCP::TableSetColumnIndex(2);
                            ImGuiMCP::TextUnformatted(
                                !reward.editorID.empty() ?
                                    reward.editorID.c_str() :
                                    reward.formIDStr.c_str());
                            ImGuiMCP::TableSetColumnIndex(3);
                            auto amount =
                                static_cast<int>(reward.amount);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            if (ImGuiMCP::InputInt(
                                    "##Amount", &amount, 0, 0)) {
                                reward.amount =
                                    static_cast<std::uint32_t>(
                                        std::max(1, amount));
                            }
                            ImGuiMCP::TableSetColumnIndex(4);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            ImGuiMCP::InputFloat(
                                "##Chance",
                                &reward.chanceReward,
                                0.0f,
                                0.0f,
                                "%.1f");
                            reward.chanceReward = std::clamp(
                                reward.chanceReward,
                                0.0f,
                                100.0f);
                            ImGuiMCP::TableSetColumnIndex(5);
                            if (ImGuiMCP::Button("X")) {
                                group.rewards.erase(
                                    group.rewards.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        rewardIndex));
                                ImGuiMCP::PopID();
                                continue;
                            }
                            ImGuiMCP::PopID();
                            ++rewardIndex;
                        }
                        ImGuiMCP::EndTable();
                    }
                    if (ImGuiMCP::Button(
                            "Delete Reward Group")) {
                        removeGroup = true;
                    }
                }
                ImGuiMCP::PopID();
                if (removeGroup) {
                    a_title.rewardGroups.erase(
                        a_title.rewardGroups.begin() +
                        static_cast<std::ptrdiff_t>(
                            groupIndex));
                }
                else {
                    ++groupIndex;
                }
            }
        }

        void DrawTitle(TitleDefinition& a_title)
        {
            ImGuiMCP::Checkbox("Enabled", &a_title.enabled);
            const auto generatedBefore =
                UniquePublicGlobalName(a_title, a_title.name);
            const auto globalFollowsName =
                a_title.version == 0 &&
                _stricmp(
                    generatedBefore.c_str(),
                    a_title.publicGlobalEditorID.c_str()) == 0;
            if (InputString("Title Name", a_title.name) &&
                globalFollowsName) {
                a_title.publicGlobalEditorID =
                    UniquePublicGlobalName(a_title, a_title.name);
            }
            InputString(
                "Description",
                a_title.description,
                1024);
            if (a_title.version == 0) {
                InputString(
                    "Public Global",
                    a_title.publicGlobalEditorID,
                    128);
                if (ImGuiMCP::Button(
                        "Generate Global from Name")) {
                    a_title.publicGlobalEditorID =
                        UniquePublicGlobalName(
                            a_title,
                            a_title.name);
                }
            }
            else {
                ImGuiMCP::Text(
                    "Public Global: %s",
                    a_title.publicGlobalEditorID.c_str());
                ImGuiMCP::TextDisabled(
                    "The EditorID remains stable after the first save.");
            }
            const auto global =
                DFGBridge::GetSingleton()->GetGlobalFormID(
                    a_title.id);
            ImGuiMCP::Text(
                "DFG Global FormID: %s",
                global ?
                    std::format("{:08X}", *global).c_str() :
                    "Unresolved");
            const auto progress =
                State::GetSingleton()->GetTitleProgress(
                    a_title.id);
            const auto waitingForPrerequisites =
                progress && std::ranges::any_of(
                    progress->requirements,
                    [](const auto& a_entry) {
                        return a_entry.second.
                            waitingForPrerequisites;
                    });
            ImGuiMCP::Text(
                "Progress: %.1f%% | %s",
                progress ?
                    progress->rawOverallProgress * 100.0f :
                    0.0f,
                progress && progress->completed ?
                    "EARNED" :
                    (waitingForPrerequisites ?
                        "WAITING FOR PLAYER PREREQUISITES" :
                        "IN PROGRESS"));
            ImGuiMCP::Separator();
            if (ImGuiMCP::Button("Manage Requirements")) {
                g_activeTitle = a_title.id;
                g_activeRequirement.clear();
                g_filterScope.reset();
                g_filterPickerOpen = false;
                g_requirementsOpen = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Manage Rewards")) {
                g_activeTitle = a_title.id;
                g_activeRewardGroup = -1;
                g_rewardPickerOpen = false;
                g_rewardsOpen = true;
            }
            const auto rewardCount = std::accumulate(
                a_title.rewardGroups.begin(),
                a_title.rewardGroups.end(),
                std::size_t{ 0 },
                [](const std::size_t a_total,
                   const RewardGroup& a_group) {
                    return a_total + a_group.rewards.size();
                });
            ImGuiMCP::Text(
                "Requirements: %zu | Reward Groups: %zu | Rewards: %zu",
                a_title.requirements.size(),
                a_title.rewardGroups.size(),
                rewardCount);
            ImGuiMCP::Separator();
            const auto pending =
                Store::GetSingleton()->
                    IsTitlePendingDeletion(a_title.id);
            if (pending) {
                ImGuiMCP::TextColored(
                    { 1.0f, 0.4f, 0.3f, 1.0f },
                    "Pending deletion. Save to confirm.");
                if (ImGuiMCP::Button("Cancel Title Deletion")) {
                    Store::GetSingleton()->
                        CancelTitleDeletion(a_title.id);
                }
            }
            else if (ImGuiMCP::Button("Delete Title")) {
                Store::GetSingleton()->
                    MarkTitleForDeletion(a_title.id);
            }
        }

        void DrawPackageWorkspace()
        {
            auto* store = Store::GetSingleton();
            const auto& packages = store->Packages();
            if (!std::ranges::any_of(
                    packages,
                    [&](const Package& a_package) {
                        return a_package.id == g_activePackage &&
                            !store->IsPackagePendingDeletion(
                                a_package.id);
                    })) {
                g_activePackage =
                    std::string(Store::kLocalPackageID);
            }
            if (!ImGuiMCP::CollapsingHeader(
                    "Package Workspace",
                    ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            for (const auto& package : packages) {
                if (store->IsPackagePendingDeletion(package.id)) {
                    continue;
                }
                options.push_back({
                    package.id,
                    package.displayName
                });
            }
            auto selected = g_activePackage;
            const auto package = std::ranges::find(
                packages,
                selected,
                &Package::id);
            ImGuiMCP::SetNextItemWidth(260.0f);
            if (DistributionCore::UI::DrawSearchableCombo(
                    "Active Package",
                    package != packages.end() ?
                        package->displayName.c_str() :
                        "Select...",
                    "wiyt_active_package",
                    options,
                    selected,
                    options.size())) {
                g_activePackage = selected;
            }
            if (package != packages.end() &&
                package->id != Store::kLocalPackageID) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Delete Active Package")) {
                    if (store->MarkPackageForDeletion(
                            package->id)) {
                        if (g_packageFilter == package->id) {
                            g_packageFilter.clear();
                        }
                        g_activePackage =
                            std::string(Store::kLocalPackageID);
                    }
                }
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(
                        "Deletion is applied only when Save is pressed.");
                }
            }
            ImGuiMCP::SetNextItemWidth(220.0f);
            ImGuiMCP::InputText(
                "New Package",
                g_newPackageName,
                sizeof(g_newPackageName));
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Create Package") &&
                g_newPackageName[0] != '\0') {
                if (const auto created =
                        store->CreatePackage(
                            g_newPackageName)) {
                    g_activePackage = *created;
                    g_newPackageName[0] = '\0';
                }
            }

            auto filterOptions = options;
            filterOptions.insert(
                filterOptions.begin(),
                { "", "All Packages" });
            const auto filteredPackage = std::ranges::find(
                packages,
                g_packageFilter,
                &Package::id);
            ImGuiMCP::SetNextItemWidth(260.0f);
            DistributionCore::UI::DrawSearchableCombo(
                "Package Filter",
                filteredPackage != packages.end() ?
                    filteredPackage->displayName.c_str() :
                    "All Packages",
                "WIYT.PackageFilter",
                filterOptions,
                g_packageFilter,
                packages.size());

            if (ImGuiMCP::Button(" + New Title ")) {
                ImGuiMCP::OpenPopup("NewTitle");
            }
            if (ImGuiMCP::BeginPopup("NewTitle")) {
                ImGuiMCP::InputText(
                    "Name",
                    g_newTitleName,
                    sizeof(g_newTitleName));
                if (ImGuiMCP::Button("Create")) {
                    auto& created =
                        store->CreateTitle(g_activePackage);
                    if (g_newTitleName[0] != '\0') {
                        created.name = g_newTitleName;
                        created.publicGlobalEditorID =
                            UniquePublicGlobalName(
                                created, created.name);
                    }
                    g_activeTitle = created.id;
                    g_newTitleName[0] = '\0';
                    ImGuiMCP::CloseCurrentPopup();
                }
                ImGuiMCP::EndPopup();
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Save")) {
                if (store->SaveAll()) {
                    RebuildRequirementIndex();
                    RefreshProgressSources(true);
                    DFGBridge::GetSingleton()->
                        SynchronizeAll();
                }
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Refresh Progress")) {
                RefreshProgressSources(true);
            }
        }
    }

    void RenderTitles()
    {
        DrawPackageWorkspace();
        auto* store = Store::GetSingleton();
        for (const auto& package : store->Packages()) {
            if (!g_packageFilter.empty() &&
                package.id != g_packageFilter) {
                continue;
            }
            const auto count = std::ranges::count(
                store->Titles(),
                package.id,
                &TitleDefinition::packageID);
            const auto pending =
                store->IsPackagePendingDeletion(package.id);
            const auto label = std::format(
                "{} ({}){}##Package_{}",
                package.displayName,
                count,
                pending ? " [DELETE ON SAVE]" : "",
                package.id);
            if (!ImGuiMCP::CollapsingHeader(label.c_str())) {
                continue;
            }
            if (package.id != Store::kLocalPackageID) {
                if (pending) {
                    if (ImGuiMCP::Button(
                            "Cancel Package Deletion")) {
                        store->CancelPackageDeletion(
                            package.id);
                    }
                }
                else if (ImGuiMCP::Button(
                             "Delete Package")) {
                    store->MarkPackageForDeletion(
                        package.id);
                }
            }
            for (auto& title : store->Titles()) {
                if (title.packageID != package.id) {
                    continue;
                }
                ImGuiMCP::PushID(title.id.c_str());
                const auto modified = title.IsModified();
                const auto progress =
                    State::GetSingleton()->GetTitleProgress(
                        title.id);
                const auto header = std::format(
                    "{}{}{} [V:{}]",
                    title.name,
                    modified ? " (Need save)" : "",
                    progress && progress->completed ?
                        " [EARNED]" :
                        "",
                    title.version);
                if (ImGuiMCP::CollapsingHeader(
                        header.c_str())) {
                    g_activeTitle = title.id;
                    DrawTitle(title);
                }
                ImGuiMCP::PopID();
            }
        }

        auto* activeTitle = g_activeTitle.empty() ?
            nullptr :
            store->FindTitle(g_activeTitle);
        const auto* viewport = ImGuiMCP::GetMainViewport();
        if (g_requirementsOpen) {
            ImGuiMCP::SetNextWindowSize({
                viewport->Size.x / 1.2f,
                viewport->Size.y / 1.2f
            });
            ImGuiMCP::OpenPopup("Manage Requirements");
        }
        if (ImGuiMCP::BeginPopupModal(
                "Manage Requirements",
                &g_requirementsOpen)) {
            if (activeTitle) {
                DrawRequirementsWorkspace(*activeTitle);
            }
            else {
                g_requirementsOpen = false;
            }
            ImGuiMCP::EndPopup();
        }
        if (g_rewardsOpen) {
            ImGuiMCP::SetNextWindowSize({
                viewport->Size.x / 1.2f,
                viewport->Size.y / 1.2f
            });
            ImGuiMCP::OpenPopup("Rewards");
        }
        if (ImGuiMCP::BeginPopupModal(
                "Rewards",
                &g_rewardsOpen)) {
            if (activeTitle) {
                DrawRewards(*activeTitle);
            }
            else {
                g_rewardsOpen = false;
            }
            ImGuiMCP::EndPopup();
        }
    }

    void RenderProgress()
    {
        if (ImGuiMCP::Button("Refresh All Sources")) {
            RefreshProgressSources(true);
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::Text(
            "DFG: %s",
            DFGBridge::GetSingleton()->IsAvailable() ?
                "Connected" :
                "Unavailable");
        ImGuiMCP::Separator();
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            const auto progress =
                State::GetSingleton()->GetTitleProgress(
                    title.id);
            if (!progress) {
                continue;
            }
            ImGuiMCP::PushID(title.id.c_str());
            const auto header = std::format(
                "{} - {:.1f}% {}##ProgressTitle",
                title.name,
                progress->rawOverallProgress * 100.0f,
                progress->completed ? "[EARNED]" : "");
            if (ImGuiMCP::CollapsingHeader(
                    header.c_str(),
                    ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
                if (!title.description.empty()) {
                    ImGuiMCP::TextWrapped(
                        "%s", title.description.c_str());
                }
                ImGuiMCP::Text(
                    "Overall Progress: %.1f%%",
                    progress->rawOverallProgress * 100.0f);
                if (ImGuiMCP::BeginTable(
                        "RequirementProgress",
                        7,
                        ImGuiMCP::ImGuiTableFlags_Borders |
                            ImGuiMCP::ImGuiTableFlags_RowBg |
                            ImGuiMCP::ImGuiTableFlags_Resizable)) {
                    ImGuiMCP::TableSetupColumn(
                        "Requirement",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                    ImGuiMCP::TableSetupColumn(
                        "Current",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        100.0f);
                    ImGuiMCP::TableSetupColumn(
                        "Required",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        100.0f);
                    ImGuiMCP::TableSetupColumn(
                        "Progress",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        190.0f);
                    ImGuiMCP::TableSetupColumn(
                        "Player Prerequisites",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        180.0f);
                    ImGuiMCP::TableSetupColumn(
                        "Mode",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        190.0f);
                    ImGuiMCP::TableSetupColumn(
                        "Status",
                        ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                        200.0f);
                    ImGuiMCP::TableHeadersRow();
                    for (const auto& requirement :
                         title.requirements) {
                        const auto found =
                            progress->requirements.find(
                                requirement.id);
                        const auto value =
                            found != progress->requirements.end() ?
                                found->second.value :
                                0.0f;
                        const auto ratio = std::clamp(
                            value / std::max(
                                0.0001f,
                                requirement.targetAmount),
                            0.0f,
                            1.0f);
                        const auto prerequisitesMet =
                            found != progress->requirements.end() ?
                                found->second.prerequisitesMet :
                                requirement.playerPrerequisiteFilters.empty();
                        const auto waiting =
                            found != progress->requirements.end() &&
                                found->second.waitingForPrerequisites;
                        const auto definitionValid =
                            RequirementDefinitionValid(requirement);
                        ImGuiMCP::TableNextRow();
                        ImGuiMCP::TableSetColumnIndex(0);
                        ImGuiMCP::TextUnformatted(
                            requirement.name.c_str());
                        ImGuiMCP::TableSetColumnIndex(1);
                        ImGuiMCP::Text("%.2f", value);
                        ImGuiMCP::TableSetColumnIndex(2);
                        ImGuiMCP::Text(
                            "%.2f", requirement.targetAmount);
                        ImGuiMCP::TableSetColumnIndex(3);
                        const auto overlay = std::format(
                            "{:.1f}%", ratio * 100.0f);
                        ImGuiMCP::ProgressBar(
                            ratio,
                            { -1.0f, 0.0f },
                            overlay.c_str());
                        ImGuiMCP::TableSetColumnIndex(4);
                        if (requirement.playerPrerequisiteFilters.empty()) {
                            ImGuiMCP::TextDisabled("Not Required");
                        }
                        else if (prerequisitesMet) {
                            ImGuiMCP::TextColored(
                                { 0.3f, 0.9f, 0.4f, 1.0f },
                                "MET");
                        }
                        else {
                            ImGuiMCP::TextColored(
                                { 1.0f, 0.65f, 0.2f, 1.0f },
                                "MISSING");
                        }
                        ImGuiMCP::TableSetColumnIndex(5);
                        ImGuiMCP::TextUnformatted(
                            ToString(requirement.prerequisiteMode));
                        ImGuiMCP::TableSetColumnIndex(6);
                        if (!definitionValid) {
                            ImGuiMCP::TextColored(
                                { 1.0f, 0.4f, 0.3f, 1.0f },
                                "Invalid Definition");
                        }
                        else if (!title.enabled) {
                            ImGuiMCP::TextDisabled("Disabled");
                        }
                        else if (progress->completed) {
                            ImGuiMCP::TextColored(
                                { 0.3f, 0.9f, 0.4f, 1.0f },
                                "Completed");
                        }
                        else if (waiting) {
                            ImGuiMCP::TextColored(
                                { 1.0f, 0.65f, 0.2f, 1.0f },
                                "Waiting for Player Prerequisites");
                        }
                        else {
                            ImGuiMCP::TextUnformatted("In Progress");
                        }
                    }
                    ImGuiMCP::EndTable();
                }
                if (ImGuiMCP::Button("Reset Progress")) {
                    State::GetSingleton()->ResetTitle(title.id);
                    State::GetSingleton()->ReconcileDefinitions(
                        Store::GetSingleton()->Titles());
                    RefreshProgressSources(true);
                    DFGBridge::GetSingleton()->SynchronizeTitle(
                        title.id,
                        0.0f);
                }
            }
            ImGuiMCP::PopID();
        }
    }

    void RenderSettings()
    {
        auto* settings = Settings::GetSingleton();
        ImGuiMCP::Checkbox("Enable WIYT", &settings->enabled);
        ImGuiMCP::Checkbox(
            "Follower actions credit the player",
            &settings->creditFollowerActions);
        ImGuiMCP::Checkbox(
            "Summon actions credit their owner",
            &settings->creditSummonActions);
        ImGuiMCP::Checkbox(
            "Ignore rewards granted by WIYT",
            &settings->ignoreWIYTRewardEvents);
        ImGuiMCP::InputFloat(
            "Minimum Statistic Refresh (seconds)",
            &settings->minimumStatisticRefreshSeconds,
            0.25f,
            1.0f,
            "%.2f");
        settings->minimumStatisticRefreshSeconds =
            std::clamp(
                settings->minimumStatisticRefreshSeconds,
                0.25f,
                60.0f);
        if (ImGuiMCP::Button("Save Settings")) {
            settings->Save();
        }
        ImGuiMCP::Separator();
        ImGuiMCP::TextWrapped(
            "Public Globals use WIYT_<TitleName>, contain normalized "
            "progress from 0.0 to 1.0, and are synchronized through DFG.");
        ImGuiMCP::BulletText(
            "WIYTTitleProgressUpdated: strArg=Global EditorID, "
            "numArg=progress.");
        ImGuiMCP::BulletText(
            "WIYTTitleEarned: strArg=titleID, numArg=1.0.");
        ImGuiMCP::BulletText(
            "WIYTReportProgress: strArg=activity name, "
            "numArg=amount, sender=source Actor.");
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::error(
                "[WIYT] SKSEMenuFramework is not installed.");
            return;
        }
        SKSEMenuFramework::SetSection("Distribution System");
        SKSEMenuFramework::AddSectionItem(
            "WIYT/Titles Manager",
            RenderTitles);
        SKSEMenuFramework::AddSectionItem(
            "WIYT/Title Progress",
            RenderProgress);
        SKSEMenuFramework::AddSectionItem(
            "WIYT/Settings",
            RenderSettings);
        logger::info("[WIYT] UI registered.");
    }
}
