#include "INLOS/UI.h"

#include "DistributionCore/Domain.h"
#include "DistributionCore/UICommon.h"
#include "INLOS/NewSkillMenu.h"
#include "INLOS/Runtime.h"
#include "INLOS/Settings.h"
#include "INLOS/Store.h"
#include "Manager.h"

#include <array>
#include <fstream>
#include <numeric>
#include <rapidjson/document.h>
#include <set>
#include <sstream>
#include <unordered_map>

namespace INLOS::UI
{
    namespace
    {
        std::string g_activePackage = std::string(Store::kLocalPackageID);
        std::string g_packageFilter;
        std::string g_activeRule;
        char g_newPackage[128]{};
        bool g_targetsOpen = false;
        bool g_blacklistOpen = false;
        bool g_rewardsOpen = false;
        bool g_filterPickerOpen = false;
        bool g_rewardPickerOpen = false;
        int g_activeRewardGroup = -1;
        char g_selectionSearch[128]{};
        char g_newRuleName[64]{};
        std::string g_selectionType = "All";
        std::string g_selectionPlugin = "All";
        std::set<std::string> g_activeTypeFilters;
        std::unordered_map<std::string, std::string> g_language;

        void DrawSkillField(
            Reward& a_reward,
            std::string_view a_stateID);
        void DrawSkillLevelMode(Reward& a_reward);

        void LoadLanguage()
        {
            g_language.clear();
            std::ifstream file(
                "Data/Viny Mods/INLOS/Language.json",
                std::ios::binary);
            if (!file) {
                return;
            }
            std::stringstream content;
            content << file.rdbuf();
            rapidjson::Document document;
            document.Parse(content.str().c_str());
            if (document.HasParseError() ||
                !document.IsObject()) {
                logger::warn(
                    "[INLOS] Language.json is invalid.");
                return;
            }
            for (auto category = document.MemberBegin();
                 category != document.MemberEnd();
                 ++category) {
                if (!category->value.IsObject()) {
                    continue;
                }
                for (auto value =
                         category->value.MemberBegin();
                     value != category->value.MemberEnd();
                     ++value) {
                    if (value->value.IsString()) {
                        g_language.emplace(
                            std::format(
                                "{}.{}",
                                category->name.GetString(),
                                value->name.GetString()),
                            value->value.GetString());
                    }
                }
            }
        }

        const char* GetLoc(
            const std::string_view a_key,
            const char* a_fallback)
        {
            const auto found = g_language.find(
                std::string(a_key));
            return found != g_language.end() ?
                found->second.c_str() :
                a_fallback;
        }

        const std::vector<
            DistributionCore::UI::SearchableComboOption>&
            GetActorValueOptions()
        {
            static std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            if (!options.empty()) {
                return options;
            }
            for (auto index = 0;
                 index <
                     std::to_underlying(
                         RE::ActorValue::kTotal);
                 ++index) {
                const auto actorValue =
                    static_cast<RE::ActorValue>(index);
                const auto* name =
                    RE::ActorValueList::
                        GetActorValueName(actorValue);
                if (name && name[0] != '\0') {
                    options.push_back({ name, name });
                }
            }
            return options;
        }

        bool InputString(
            const char* a_label,
            std::string& a_value,
            const std::size_t a_capacity = 256)
        {
            std::vector<char> buffer(
                std::max(a_capacity, a_value.size() + 2), '\0');
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
            const auto* preview = current != a_options.end() ?
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

        bool DrawFormCombo(
            const char* a_label,
            const std::string& a_type,
            std::string& a_editorID,
            std::string& a_formID,
            const bool a_playableOnly)
        {
            const auto& forms =
                Manager::GetSingleton()->GetList(a_type);
            const auto selected = std::ranges::find_if(
                forms,
                [&](const InternalFormInfo& a_info) {
                    return (!a_editorID.empty() &&
                               _stricmp(
                                   a_info.editorID.c_str(),
                                   a_editorID.c_str()) == 0) ||
                        FormReference(a_info) == a_formID;
                });
            const auto preview =
                selected != forms.end() ?
                selected->GetDisplayName() :
                (!a_editorID.empty() ? a_editorID :
                    (!a_formID.empty() ? a_formID : "Select..."));
            if (!ImGuiMCP::BeginCombo(a_label, preview.c_str())) {
                return false;
            }

            struct PickerState
            {
                char search[128]{};
                std::string appliedSearch;
                std::uint64_t revision =
                    static_cast<std::uint64_t>(-1);
                bool playableOnly = true;
                std::vector<std::size_t> matches;
            };
            static std::unordered_map<std::string, PickerState> states;
            static auto clipper =
                ImGuiMCP::ImGuiListClipperManager::Create();
            auto& state = states[
                std::string(a_label) + '\x1f' + a_type];
            const auto playableChanged =
                state.playableOnly != a_playableOnly;
            state.playableOnly = a_playableOnly;
            if (ImGuiMCP::IsWindowAppearing()) {
                state.search[0] = '\0';
                state.appliedSearch.clear();
                state.revision =
                    static_cast<std::uint64_t>(-1);
                ImGuiMCP::SetKeyboardFocusHere();
            }
            ImGuiMCP::SetNextItemWidth(-1.0f);
            const auto changedSearch =
                ImGuiMCP::InputTextWithHint(
                    "##Search",
                    "Search...",
                    state.search,
                    sizeof(state.search));
            std::string normalized = state.search;
            std::ranges::transform(
                normalized,
                normalized.begin(),
                [](const unsigned char a_character) {
                    return static_cast<char>(
                        std::tolower(a_character));
                });
            const auto revision =
                Manager::GetSingleton()->GetListRevision();
            if (changedSearch ||
                playableChanged ||
                state.appliedSearch != normalized ||
                state.revision != revision) {
                state.matches.clear();
                for (std::size_t index = 0;
                     index < forms.size();
                     ++index) {
                    const auto& info = forms[index];
                    if (a_playableOnly && !info.playable) {
                        continue;
                    }
                    auto searchable = info.name + " " +
                        info.editorID + " " + info.pluginName;
                    std::ranges::transform(
                        searchable,
                        searchable.begin(),
                        [](const unsigned char a_character) {
                            return static_cast<char>(
                                std::tolower(a_character));
                        });
                    if (normalized.empty() ||
                        searchable.contains(normalized)) {
                        state.matches.push_back(index);
                    }
                }
                state.appliedSearch = std::move(normalized);
                state.revision = revision;
            }

            bool changed = false;
            if (ImGuiMCP::BeginChild(
                    "##Results",
                    { 0.0f, 240.0f },
                    0)) {
                ImGuiMCP::ImGuiListClipperManager::Begin(
                    clipper,
                    static_cast<int>(state.matches.size()),
                    -1.0f);
                while (ImGuiMCP::ImGuiListClipperManager::Step(
                    clipper)) {
                    for (auto visible = clipper->DisplayStart;
                         visible < clipper->DisplayEnd;
                         ++visible) {
                        const auto& info = forms[
                            state.matches[
                                static_cast<std::size_t>(visible)]];
                        const auto display = std::format(
                            "{} | {} | {}",
                            info.GetDisplayName(),
                            info.editorID,
                            info.pluginName);
                        ImGuiMCP::PushID(
                            static_cast<int>(info.formID));
                        if (ImGuiMCP::Selectable(
                                display.c_str(),
                                selected != forms.end() &&
                                    selected->formID ==
                                        info.formID)) {
                            a_editorID = info.editorID;
                            a_formID = FormReference(info);
                            changed = true;
                        }
                        ImGuiMCP::PopID();
                    }
                }
            }
            ImGuiMCP::EndChild();
            ImGuiMCP::EndCombo();
            return changed;
        }

        void DrawNumericComparison(BlacklistFilter& a_filter)
        {
            ImGuiMCP::SetNextItemWidth(120.0f);
            EnumCombo(
                "Operator",
                a_filter.comparison,
                {
                    { NumericComparison::kGreaterOrEqual, ">=" },
                    { NumericComparison::kLessOrEqual, "<=" },
                    { NumericComparison::kEqual, "=" },
                    { NumericComparison::kBetween, "Between" }
                });
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(100.0f);
            ImGuiMCP::InputFloat(
                "Value",
                &a_filter.minimumValue,
                1.0f,
                10.0f,
                "%.2f");
            if (a_filter.comparison == NumericComparison::kBetween) {
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(100.0f);
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
            bool& a_remove)
        {
            DistributionCore::UI::DrawFilterOperator(
                "Match", a_filter);
            const auto options =
                DistributionCore::FilterRegistry().AvailableFor(
                    DistributionCore::Domain::kINLOS);
            if (ImGuiMCP::BeginCombo(
                    "Type",
                    a_filter.type.empty() ?
                        "Select..." :
                        a_filter.type.c_str())) {
                for (const auto& option : options) {
                    const auto selected =
                        option.id == a_filter.type;
                    if (ImGuiMCP::Selectable(
                            option.displayName.c_str(),
                            selected)) {
                        a_filter.type = option.id;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            const auto* descriptor =
                DistributionCore::FilterRegistry().Find(a_filter.type);
            if (descriptor &&
                (descriptor->capabilities &
                    DistributionCore::ToMask(
                        DistributionCore::TypeCapability::kRequiresForm)) != 0) {
                InputString("EditorID", a_filter.editorID);
                InputString(
                    "Plugin|FormID",
                    a_filter.formIDStr);
            }
            if (a_filter.type == "Actor Value") {
                InputString(
                    "Actor Value",
                    a_filter.actorValueName);
                EnumCombo(
                    "Value Mode",
                    a_filter.actorValueMode,
                    {
                        { ActorValueMode::kCurrent, "Current" },
                        { ActorValueMode::kPermanent, "Permanent" },
                        { ActorValueMode::kMaximum, "Maximum" }
                    });
            }
            if (descriptor &&
                (descriptor->capabilities &
                    DistributionCore::ToMask(
                        DistributionCore::TypeCapability::kNumeric)) != 0) {
                DrawNumericComparison(a_filter);
            }
            if (a_filter.type == "Source Plugin") {
                InputString("Plugin", a_filter.optionText);
            }
            if (a_filter.type == "NPC Trait" ||
                a_filter.type == "Cell Type") {
                ImGuiMCP::InputInt(
                    "Option",
                    &a_filter.optionMode);
            }
            if (a_filter.type == "City Status") {
                EnumCombo(
                    "City Status",
                    a_filter.optionMode,
                    { { 0, "Inside a City" }, { 1, "Outside a City" } });
            }
            if (ImGuiMCP::Button("Remove Filter")) {
                a_remove = true;
            }
        }

        void DrawFilters(
            const char* a_label,
            std::vector<BlacklistFilter>& a_filters,
            bool& a_requiresAll)
        {
            if (!ImGuiMCP::CollapsingHeader(a_label)) {
                return;
            }
            ImGuiMCP::Checkbox("Require all (AND)", &a_requiresAll);
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Add Filter")) {
                a_filters.emplace_back();
            }
            for (std::size_t index = 0; index < a_filters.size();) {
                ImGuiMCP::PushID(static_cast<int>(index));
                bool remove = false;
                ImGuiMCP::Separator();
                DrawFilter(a_filters[index], remove);
                ImGuiMCP::PopID();
                if (remove) {
                    a_filters.erase(a_filters.begin() + index);
                }
                else {
                    ++index;
                }
            }
        }

        void ResetSelectionState()
        {
            g_selectionSearch[0] = '\0';
            g_selectionType = "All";
            g_selectionPlugin = "All";
        }

        bool SameText(
            const std::string_view a_left,
            const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                std::equal(
                    a_left.begin(),
                    a_left.end(),
                    a_right.begin(),
                    [](const unsigned char a_lhs,
                       const unsigned char a_rhs) {
                        return std::tolower(a_lhs) ==
                            std::tolower(a_rhs);
                    });
        }

        bool MatchesForm(
            const BlacklistFilter& a_filter,
            const InternalFormInfo& a_info)
        {
            return a_filter.type == a_info.formType &&
                ((!a_filter.editorID.empty() &&
                    SameText(
                        a_filter.editorID,
                        a_info.editorID)) ||
                    a_filter.formIDStr ==
                        FormReference(a_info));
        }

        bool MatchesForm(
            const Reward& a_reward,
            const InternalFormInfo& a_info)
        {
            return a_reward.typeReward == a_info.formType &&
                ((!a_reward.editorID.empty() &&
                    SameText(
                        a_reward.editorID,
                        a_info.editorID)) ||
                    a_reward.formIDStr ==
                        FormReference(a_info));
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
                filter.optionMode =
                    std::max(
                        0,
                        static_cast<int>(a_info.formID) - 1);
            }
            if (IsNumericValueFilterType(filter.type)) {
                filter.comparison =
                    NumericComparison::kGreaterOrEqual;
                filter.minimumValue =
                    filter.type == "Faction Rank" ?
                        0.0f :
                        1.0f;
                filter.maximumValue = filter.minimumValue;
            }
            return filter;
        }

        void DrawSelectionTable(
            std::vector<BlacklistFilter>* a_filters,
            RewardGroup* a_group)
        {
            const auto rewardMode = a_group != nullptr;
            const auto descriptors = rewardMode ?
                DistributionCore::RewardRegistry().AvailableFor(
                    DistributionCore::Domain::kINLOS) :
                DistributionCore::FilterRegistry().AvailableFor(
                    DistributionCore::Domain::kINLOS);

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
                if ((!rewardMode &&
                        descriptor.id == "Actor Value") ||
                    (rewardMode &&
                        (descriptor.id == "Experience" ||
                         descriptor.id == "Skill XP" ||
                         descriptor.id == "Skill Level" ||
                         descriptor.id == "Perk Points" ||
                         descriptor.id == "NSM Resource"))) {
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
                "##FilterType",
                g_selectionType.c_str(),
                rewardMode ?
                    "INLOS.RewardType" :
                    "INLOS.FilterType",
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
                            source.push_back(
                                std::addressof(*found));
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
                            source.push_back(
                                std::addressof(*found));
                        }
                    }
                }
            }
            else {
                for (const auto& descriptor : descriptors) {
                    if (g_selectionType != "All" &&
                        descriptor.id != g_selectionType) {
                        continue;
                    }
                    const auto& list =
                        Manager::GetSingleton()->GetList(
                            descriptor.id);
                    for (const auto& info : list) {
                        source.push_back(std::addressof(info));
                    }
                }
            }

            if (source.empty()) {
                ImGuiMCP::TextDisabled(
                    "No items found in this category.");
                return;
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
            ImGuiMCP::SetNextItemWidth(300.0f);
            DistributionCore::UI::DrawSearchableCombo(
                "##Plugin",
                g_selectionPlugin == "All" ?
                    "All Plugins" :
                    g_selectionPlugin.c_str(),
                rewardMode ?
                    "INLOS.RewardPlugin" :
                    "INLOS.FilterPlugin",
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
            std::vector<std::size_t> visibleIndices;
            visibleIndices.reserve(source.size());
            for (std::size_t index = 0;
                 index < source.size();
                 ++index) {
                const auto* info = source[index];
                if (!info ||
                    (rewardMode && playableOnly &&
                        !info->playable) ||
                    (g_selectionPlugin != "All" &&
                        info->pluginName !=
                            g_selectionPlugin)) {
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
                if (search.empty() ||
                    searchable.contains(search)) {
                    visibleIndices.push_back(index);
                }
            }

            ImGuiMCP::ImVec2 available;
            ImGuiMCP::GetContentRegionAvail(&available);
            const auto columns = rewardMode ? 7 : 6;
            const auto flags =
                ImGuiMCP::ImGuiTableFlags_Borders |
                ImGuiMCP::ImGuiTableFlags_RowBg |
                ImGuiMCP::ImGuiTableFlags_Resizable |
                ImGuiMCP::ImGuiTableFlags_ScrollY;
            if (!ImGuiMCP::BeginTable(
                    "SelectionTable",
                    columns,
                    flags,
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
                180.0f);
            ImGuiMCP::TableSetupColumn(
                "Plugin",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (rewardMode) {
                ImGuiMCP::TableSetupColumn(
                    "Qty",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    70.0f);
                ImGuiMCP::TableSetupColumn(
                    "Chance",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    70.0f);
            }
            else {
                ImGuiMCP::TableSetupColumn(
                    "Value",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    90.0f);
            }
            ImGuiMCP::TableHeadersRow();

            static auto clipper =
                ImGuiMCP::ImGuiListClipperManager::Create();
            ImGuiMCP::ImGuiListClipperManager::Begin(
                clipper,
                static_cast<int>(visibleIndices.size()),
                -1.0f);
            while (ImGuiMCP::ImGuiListClipperManager::Step(
                clipper)) {
                for (auto row = clipper->DisplayStart;
                     row < clipper->DisplayEnd;
                     ++row) {
                    const auto* info = source[
                        visibleIndices[
                            static_cast<std::size_t>(row)]];
                    if (!info) {
                        continue;
                    }
                    ImGuiMCP::PushID(
                        std::format(
                            "{}:{}",
                            info->formType,
                            info->formID)
                            .c_str());
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    if (rewardMode) {
                        auto found = std::ranges::find_if(
                            a_group->rewards,
                            [&](const Reward& a_reward) {
                                return MatchesForm(
                                    a_reward, *info);
                            });
                        auto selected =
                            found != a_group->rewards.end();
                        if (ImGuiMCP::Checkbox(
                                "##Active",
                                &selected)) {
                            if (selected) {
                                Reward reward;
                                reward.typeReward =
                                    info->formType;
                                reward.formIDStr =
                                    FormReference(*info);
                                reward.editorID =
                                    info->editorID;
                                a_group->rewards.push_back(
                                    std::move(reward));
                            }
                            else if (
                                found !=
                                    a_group->rewards.end()) {
                                a_group->rewards.erase(found);
                            }
                        }
                    }
                    else if (a_filters) {
                        auto found = std::ranges::find_if(
                            *a_filters,
                            [&](const BlacklistFilter& a_filter) {
                                return MatchesForm(
                                    a_filter, *info);
                            });
                        auto selected =
                            found != a_filters->end();
                        if (ImGuiMCP::Checkbox(
                                "##Active",
                                &selected)) {
                            if (selected) {
                                a_filters->push_back(
                                    MakeFilter(*info));
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

                    if (rewardMode) {
                        const auto found = std::ranges::find_if(
                            a_group->rewards,
                            [&](const Reward& a_reward) {
                                return MatchesForm(
                                    a_reward, *info);
                            });
                        if (found != a_group->rewards.end()) {
                            ImGuiMCP::TableSetColumnIndex(5);
                            auto amount =
                                static_cast<int>(found->amount);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            if (ImGuiMCP::InputInt(
                                    "##Amount",
                                    &amount,
                                    0,
                                    0)) {
                                found->amount =
                                    static_cast<std::uint32_t>(
                                        std::max(1, amount));
                            }
                            ImGuiMCP::TableSetColumnIndex(6);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            ImGuiMCP::InputFloat(
                                "##Chance",
                                &found->chanceReward,
                                0.0f,
                                0.0f,
                                "%.1f");
                            found->chanceReward = std::clamp(
                                found->chanceReward,
                                0.0f,
                                100.0f);
                        }
                    }
                    else if (a_filters) {
                        ImGuiMCP::TableSetColumnIndex(5);
                        const auto found = std::ranges::find_if(
                            *a_filters,
                            [&](const BlacklistFilter& a_filter) {
                                return MatchesForm(
                                    a_filter, *info);
                            });
                        if (found != a_filters->end() &&
                            IsNumericValueFilterType(
                                found->type)) {
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            ImGuiMCP::InputFloat(
                                "##Value",
                                &found->minimumValue,
                                0.0f,
                                0.0f,
                                "%.1f");
                            if (found->comparison !=
                                NumericComparison::kBetween) {
                                found->maximumValue =
                                    found->minimumValue;
                            }
                        }
                        else {
                            ImGuiMCP::TextDisabled("-");
                        }
                    }
                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::EndTable();
        }

        void DrawFilterWorkspace(
            LootRule& a_lootRule,
            const bool a_blacklist)
        {
            auto& rule = a_lootRule.criteria;
            auto& filters = a_blacklist ?
                rule.blacklistFilters :
                rule.targetFilters;
            auto& gender = a_blacklist ?
                rule.blacklistedGender :
                rule.targetGender;
            auto& body = a_blacklist ?
                rule.blacklistedHumanoid :
                rule.targetHumanoid;
            auto& age = a_blacklist ?
                rule.blacklistedChild :
                rule.targetChild;
            auto& requireAll = a_blacklist ?
                rule.blacklistRequiresAll :
                rule.targetRequiresAll;

            if (ImGuiMCP::Button("Back")) {
                if (a_blacklist) {
                    g_blacklistOpen = false;
                }
                else {
                    g_targetsOpen = false;
                }
            }
            ImGuiMCP::SameLine();
            EnumCombo(
                a_blacklist ? "Excluded Gender" : "Target Gender",
                gender,
                {
                    { 0, a_blacklist ? "None" : "All" },
                    { 1, "Male" },
                    { 2, "Female" }
                });
            ImGuiMCP::SameLine();
            EnumCombo(
                a_blacklist ? "Excluded Body" : "Target Body",
                body,
                {
                    { 0, a_blacklist ? "None" : "Both" },
                    { 1, "Humanoid Only" },
                    { 2, "Non-humanoid Only" }
                });
            ImGuiMCP::SameLine();
            EnumCombo(
                a_blacklist ? "Excluded Age" : "Target Age",
                age,
                {
                    { 0, a_blacklist ? "None" : "Both" },
                    { 1, "Children Only" },
                    { 2, "Exclude Children" }
                });
            ImGuiMCP::Checkbox(
                a_blacklist ?
                    "Require ALL filters to invalidate (AND)" :
                    "Require ALL filters (AND)",
                &requireAll);

            const auto options =
                DistributionCore::FilterRegistry().AvailableFor(
                    DistributionCore::Domain::kINLOS);
            if (ImGuiMCP::BeginCombo(
                    a_blacklist ?
                        "Add Blacklist Filter" :
                        "Add Target Filter",
                    "Select...")) {
                for (const auto& option : options) {
                    if (ImGuiMCP::Selectable(
                            option.displayName.c_str(),
                            false)) {
                        BlacklistFilter filter;
                        filter.type = option.id;
                        filter.minimumValue =
                            option.id == "Faction Rank" ? 0.0f : 1.0f;
                        filters.push_back(std::move(filter));
                    }
                }
                ImGuiMCP::EndCombo();
            }

            const auto flags =
                ImGuiMCP::ImGuiTableFlags_Borders |
                ImGuiMCP::ImGuiTableFlags_RowBg |
                ImGuiMCP::ImGuiTableFlags_Resizable |
                ImGuiMCP::ImGuiTableFlags_ScrollY;
            if (!ImGuiMCP::BeginTable(
                    a_blacklist ?
                        "INLOSBlacklistTable" :
                        "INLOSTargetTable",
                    5,
                    flags,
                    { 0.0f, 0.0f })) {
                return;
            }
            ImGuiMCP::TableSetupColumn(
                "Type",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                135.0f);
            ImGuiMCP::TableSetupColumn(
                "Form / Value",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(
                "Condition",
                ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                330.0f);
            ImGuiMCP::TableSetupColumn(
                "Identifier",
                ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
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
                ImGuiMCP::TextUnformatted(filter.type.c_str());

                ImGuiMCP::TableSetColumnIndex(1);
                const auto& formList =
                    Manager::GetSingleton()->GetList(filter.type);
                if (!formList.empty()) {
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    const auto formChanged = DrawFormCombo(
                        "##FilterForm",
                        filter.type,
                        filter.editorID,
                        filter.formIDStr,
                        false);
                    if (formChanged) {
                        const auto selected =
                            std::ranges::find_if(
                                formList,
                                [&](const InternalFormInfo& a_info) {
                                    return _stricmp(
                                        a_info.editorID.c_str(),
                                        filter.editorID.c_str()) == 0;
                                });
                        if (selected != formList.end()) {
                            if (filter.type == "Source Plugin") {
                                filter.optionText =
                                    selected->editorID;
                            }
                            else if (
                                filter.type == "NPC Trait" ||
                                filter.type == "Cell Type" ||
                                filter.type ==
                                    "Equipped Category") {
                                filter.optionMode =
                                    static_cast<int>(
                                        selected->formID) - 1;
                            }
                        }
                    }
                }
                else if (filter.type == "Actor Value") {
                    InputString(
                        "##ActorValue",
                        filter.actorValueName);
                }
                else if (filter.type == "Source Plugin") {
                    InputString(
                        "##SourcePlugin",
                        filter.optionText);
                }
                else {
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    ImGuiMCP::InputInt(
                        "##Option",
                        &filter.optionMode);
                }

                ImGuiMCP::TableSetColumnIndex(2);
                const auto* descriptor =
                    DistributionCore::FilterRegistry().Find(
                        filter.type);
                const auto numeric =
                    (descriptor &&
                        (descriptor->capabilities &
                            DistributionCore::ToMask(
                                DistributionCore::
                                    TypeCapability::kNumeric)) != 0) ||
                    IsNumericValueFilterType(filter.type);
                if (numeric) {
                    ImGuiMCP::SetNextItemWidth(120.0f);
                    EnumCombo(
                        "##Operator",
                        filter.comparison,
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
                    ImGuiMCP::SetNextItemWidth(70.0f);
                    ImGuiMCP::InputFloat(
                        "##Minimum",
                        &filter.minimumValue,
                        0.0f,
                        0.0f,
                        "%.1f");
                    if (filter.comparison ==
                        NumericComparison::kBetween) {
                        ImGuiMCP::SameLine();
                        ImGuiMCP::SetNextItemWidth(70.0f);
                        ImGuiMCP::InputFloat(
                            "##Maximum",
                            &filter.maximumValue,
                            0.0f,
                            0.0f,
                            "%.1f");
                    }
                }
                else {
                    ImGuiMCP::TextDisabled("-");
                }

                ImGuiMCP::TableSetColumnIndex(3);
                ImGuiMCP::TextUnformatted(
                    !filter.editorID.empty() ?
                        filter.editorID.c_str() :
                        (!filter.formIDStr.empty() ?
                            filter.formIDStr.c_str() :
                            filter.optionText.c_str()));
                ImGuiMCP::TableSetColumnIndex(4);
                if (ImGuiMCP::Button("X")) {
                    filters.erase(filters.begin() + index);
                    ImGuiMCP::PopID();
                    continue;
                }
                ImGuiMCP::PopID();
                ++index;
            }
            ImGuiMCP::EndTable();
        }

        void DrawRewardsWorkspace(LootRule& a_lootRule)
        {
            auto& groups = a_lootRule.criteria.rewardGroups;
            if (ImGuiMCP::Button("Back")) {
                g_rewardsOpen = false;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ New Group")) {
                groups.push_back(
                    RewardGroup{ .name = "New Group" });
            }
            ImGuiMCP::SameLine();
            static bool playableOnly = true;
            ImGuiMCP::Checkbox(
                "Playable Forms Only",
                &playableOnly);

            for (std::size_t groupIndex = 0;
                 groupIndex < groups.size();) {
                auto& group = groups[groupIndex];
                ImGuiMCP::PushID(
                    static_cast<int>(groupIndex));
                const auto header = std::format(
                    "{} ({} rewards)###INLOSGroup{}",
                    group.name,
                    group.rewards.size(),
                    groupIndex);
                bool removeGroup = false;
                if (ImGuiMCP::CollapsingHeader(
                        header.c_str(),
                        ImGuiMCP::
                            ImGuiTreeNodeFlags_DefaultOpen)) {
                    InputString("Group Name", group.name, 128);
                    ImGuiMCP::SameLine();
                    removeGroup =
                        ImGuiMCP::Button("Delete Group");
                    ImGuiMCP::InputFloat(
                        "Activation Chance (%)",
                        &group.chanceGroup,
                        1.0f,
                        10.0f,
                        "%.1f");
                    group.chanceGroup = std::clamp(
                        group.chanceGroup,
                        0.0f,
                        100.0f);
                    ImGuiMCP::Checkbox(
                        "Exclusive (pick one reward)",
                        &group.isExclusive);

                    const auto options =
                        DistributionCore::RewardRegistry().
                            AvailableFor(
                                DistributionCore::Domain::kINLOS);
                    if (ImGuiMCP::BeginCombo(
                            "Add Reward",
                            "Select...")) {
                        for (const auto& option : options) {
                            if (ImGuiMCP::Selectable(
                                    option.displayName.c_str(),
                                    false)) {
                                Reward reward;
                                reward.typeReward = option.id;
                                group.rewards.push_back(
                                    std::move(reward));
                            }
                        }
                        ImGuiMCP::EndCombo();
                    }

                    const auto flags =
                        ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable;
                    if (ImGuiMCP::BeginTable(
                            "INLOSRewardsTable",
                            6,
                            flags)) {
                        ImGuiMCP::TableSetupColumn(
                            "Type",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthFixed,
                            170.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Reward",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableSetupColumn(
                            "Identifier",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableSetupColumn(
                            "Qty",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthFixed,
                            75.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Chance",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthFixed,
                            85.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Action",
                            ImGuiMCP::
                                ImGuiTableColumnFlags_WidthFixed,
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
                            const auto& formList =
                                Manager::GetSingleton()->GetList(
                                    reward.typeReward);
                            if (!formList.empty()) {
                                ImGuiMCP::SetNextItemWidth(-1.0f);
                                DrawFormCombo(
                                    "##RewardForm",
                                    reward.typeReward,
                                    reward.editorID,
                                    reward.formIDStr,
                                    playableOnly);
                            }
                            else if (reward.typeReward == "Skill XP" ||
                                     reward.typeReward == "Skill Level") {
                                DrawSkillField(
                                    reward,
                                    std::format(
                                        "INLOS.Picker.Skill.{}.{}",
                                        groupIndex,
                                        rewardIndex));
                                if (reward.typeReward == "Skill Level") {
                                    DrawSkillLevelMode(reward);
                                }
                            }
                            else {
                                ImGuiMCP::TextDisabled(
                                    "Numeric reward");
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
                                    "##Amount",
                                    &amount,
                                    0,
                                    0)) {
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
                                    rewardIndex);
                                ImGuiMCP::PopID();
                                continue;
                            }
                            ImGuiMCP::PopID();
                            ++rewardIndex;
                        }
                        ImGuiMCP::EndTable();
                    }
                }
                ImGuiMCP::PopID();
                if (removeGroup) {
                    groups.erase(groups.begin() + groupIndex);
                }
                else {
                    ++groupIndex;
                }
            }
        }

        void DrawEDFStyleFilterWorkspace(
            LootRule& a_lootRule,
            const bool a_blacklist)
        {
            auto& rule = a_lootRule.criteria;
            auto& filters = a_blacklist ?
                rule.blacklistFilters :
                rule.targetFilters;
            auto& gender = a_blacklist ?
                rule.blacklistedGender :
                rule.targetGender;
            auto& body = a_blacklist ?
                rule.blacklistedHumanoid :
                rule.targetHumanoid;
            auto& age = a_blacklist ?
                rule.blacklistedChild :
                rule.targetChild;
            auto& requiresAll = a_blacklist ?
                rule.blacklistRequiresAll :
                rule.targetRequiresAll;

            if (g_filterPickerOpen) {
                if (ImGuiMCP::Button("Back")) {
                    g_filterPickerOpen = false;
                }
                DrawSelectionTable(
                    std::addressof(filters),
                    nullptr);
                return;
            }

            if (ImGuiMCP::Button("Back")) {
                if (a_blacklist) {
                    g_blacklistOpen = false;
                }
                else {
                    g_targetsOpen = false;
                }
            }
            ImGuiMCP::SameLine();

            gender = std::clamp(gender, 0, 2);
            body = std::clamp(body, 0, 2);
            age = std::clamp(age, 0, 2);
            const char* genderOptions[] = {
                a_blacklist ? "None" : "All",
                "Male",
                "Female"
            };
            ImGuiMCP::TextUnformatted(
                a_blacklist ?
                    "Excluded Gender:" :
                    "Target Gender:");
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(150.0f);
            if (ImGuiMCP::BeginCombo(
                    "##Gender",
                    genderOptions[gender])) {
                for (int option = 0; option < 3; ++option) {
                    if (ImGuiMCP::Selectable(
                            genderOptions[option],
                            gender == option)) {
                        gender = option;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            ImGuiMCP::SameLine();
            ImGuiMCP::TextUnformatted(
                a_blacklist ?
                    "Excluded Body:" :
                    "Target Body:");
            ImGuiMCP::SameLine();
            const char* bodyOptions[] = {
                a_blacklist ? "None" : "Both",
                "Only Humanoids",
                "Only Non-Humanoids"
            };
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::BeginCombo(
                    "##Body",
                    bodyOptions[body])) {
                for (int option = 0; option < 3; ++option) {
                    if (ImGuiMCP::Selectable(
                            bodyOptions[option],
                            body == option)) {
                        body = option;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            ImGuiMCP::SameLine();
            ImGuiMCP::TextUnformatted(
                a_blacklist ?
                    "Excluded Age:" :
                    "Target Age:");
            ImGuiMCP::SameLine();
            const char* ageOptions[] = {
                a_blacklist ? "None" : "Both",
                "Only Children",
                "Only Non-Children"
            };
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::BeginCombo(
                    "##Age",
                    ageOptions[age])) {
                for (int option = 0; option < 3; ++option) {
                    if (ImGuiMCP::Selectable(
                            ageOptions[option],
                            age == option)) {
                        age = option;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            if (!a_blacklist) {
                ImGuiMCP::TextUnformatted("Summoned Status:");
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(190.0f);
                EnumCombo(
                    "##Summoned",
                    rule.summonedState,
                    {
                        { RuleSummonedState::kAny, "Any Actor" },
                        { RuleSummonedState::kSummonedOnly, "Summoned Only" },
                        { RuleSummonedState::kExcludeSummoned, "Exclude Summoned" }
                    });
                ImGuiMCP::SameLine();
                ImGuiMCP::TextUnformatted("Hostility:");
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(190.0f);
                EnumCombo(
                    "##Hostility",
                    rule.hostilityState,
                    {
                        { RuleHostilityState::kAny, "Any Actor" },
                        { RuleHostilityState::kHostileToPlayer, "Hostile to Player" },
                        { RuleHostilityState::kFriendlyOrAlly, "Friendly / Ally" }
                    });

                ImGuiMCP::TextUnformatted("Follower Status:");
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(220.0f);
                EnumCombo(
                    "##Follower",
                    rule.followerState,
                    {
                        { RuleFollowerState::kAny, "Any Actor" },
                        { RuleFollowerState::kActiveOnly, "Active Followers Only" },
                        { RuleFollowerState::kExcludeActive, "Exclude Active Followers" }
                    });
            }

            ImGuiMCP::Checkbox(
                a_blacklist ?
                    "Require ALL filters to invalidate (AND)" :
                    "Require ALL filters (AND)",
                &requiresAll);
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    a_blacklist ?
                        "If unchecked, any matching filter invalidates the rule." :
                        "If unchecked, any matching filter validates the rule.");
            }

            ImGuiMCP::Separator();
            if (ImGuiMCP::Button(
                    a_blacklist ?
                        "Add New Filter" :
                        "Add New Target Filter")) {
                ResetSelectionState();
                g_filterPickerOpen = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ Actor Value")) {
                BlacklistFilter filter;
                filter.type = "Actor Value";
                filter.actorValueName = "Health";
                filter.actorValueMode =
                    ActorValueMode::kMaximum;
                filter.comparison =
                    NumericComparison::kGreaterOrEqual;
                filter.minimumValue = 100.0f;
                filter.maximumValue = 100.0f;
                filters.push_back(std::move(filter));
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ Height")) {
                BlacklistFilter filter;
                filter.type = "Height";
                filter.optionText = "Height";
                filter.comparison = NumericComparison::kGreaterOrEqual;
                filter.minimumValue = 1.0f;
                filter.maximumValue = 1.0f;
                filters.push_back(std::move(filter));
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ Weight")) {
                BlacklistFilter filter;
                filter.type = "Weight";
                filter.optionText = "Weight";
                filter.comparison = NumericComparison::kGreaterOrEqual;
                filter.minimumValue = 50.0f;
                filter.maximumValue = 50.0f;
                filters.push_back(std::move(filter));
            }

            const auto hasActorValues =
                std::ranges::any_of(
                    filters,
                    [](const BlacklistFilter& a_filter) {
                        return a_filter.type ==
                            "Actor Value";
                    });
            std::vector<std::size_t> actorValueIndices;
            actorValueIndices.reserve(filters.size());
            for (std::size_t index = 0;
                 index < filters.size();
                 ++index) {
                if (filters[index].type == "Actor Value") {
                    actorValueIndices.push_back(index);
                }
            }
            if (hasActorValues &&
                ImGuiMCP::BeginTable(
                    a_blacklist ?
                        "BlacklistActorValues" :
                        "TargetActorValues",
                    9,
                    ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable)) {
                ImGuiMCP::TableSetupColumn(
                    "Match",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    100.0f);
                ImGuiMCP::TableSetupColumn(
                    "Actor Value",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    330.0f);
                ImGuiMCP::TableSetupColumn(
                    "Known Values",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    165.0f);
                ImGuiMCP::TableSetupColumn(
                    "Value Mode",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    135.0f);
                ImGuiMCP::TableSetupColumn(
                    "Operator",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    200.0f);
                ImGuiMCP::TableSetupColumn(
                    "Value",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    120.0f);
                ImGuiMCP::TableSetupColumn(
                    "Maximum",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    120.0f);
                ImGuiMCP::TableSetupColumn(
                    "Status",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    130.0f);
                ImGuiMCP::TableSetupColumn(
                    "Action",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    75.0f);
                ImGuiMCP::TableHeadersRow();

                static auto actorValueClipper =
                    ImGuiMCP::ImGuiListClipperManager::Create();
                ImGuiMCP::ImGuiListClipperManager::Begin(
                    actorValueClipper,
                    static_cast<int>(actorValueIndices.size()),
                    -1.0f);
                auto removedActorValue = false;
                while (!removedActorValue &&
                       ImGuiMCP::ImGuiListClipperManager::Step(
                           actorValueClipper)) {
                  for (auto visible = actorValueClipper->DisplayStart;
                       visible < actorValueClipper->DisplayEnd;
                       ++visible) {
                    const auto index = actorValueIndices[
                        static_cast<std::size_t>(visible)];
                    auto& filter = filters[index];
                    ImGuiMCP::PushID(
                        static_cast<int>(index));
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    DistributionCore::UI::DrawFilterOperator(
                        "##FilterOperator", filter);
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    InputString(
                        "##ActorValue",
                        filter.actorValueName);
                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    DistributionCore::UI::
                        DrawSearchableCombo(
                            "##Known",
                            "Select...",
                            std::format(
                                "INLOS.{}.{}.ActorValue.{}",
                                rule.id,
                                a_blacklist ?
                                    "Blacklist" :
                                    "Target",
                                index),
                            GetActorValueOptions(),
                            filter.actorValueName,
                            1);
                    ImGuiMCP::TableSetColumnIndex(3);
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    EnumCombo(
                        "##Mode",
                        filter.actorValueMode,
                        {
                            { ActorValueMode::kCurrent, "Current" },
                            { ActorValueMode::kPermanent, "Permanent" },
                            { ActorValueMode::kMaximum, "Maximum" }
                        });
                    ImGuiMCP::TableSetColumnIndex(4);
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    EnumCombo(
                        "##Comparison",
                        filter.comparison,
                        {
                            { NumericComparison::kGreaterOrEqual, ">=" },
                            { NumericComparison::kLessOrEqual, "<=" },
                            { NumericComparison::kEqual, "=" },
                            { NumericComparison::kBetween, "Between" }
                        });
                    ImGuiMCP::TableSetColumnIndex(5);
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    ImGuiMCP::InputFloat(
                        "##Minimum",
                        &filter.minimumValue,
                        0.0f,
                        0.0f,
                        "%.1f");
                    ImGuiMCP::TableSetColumnIndex(6);
                    if (filter.comparison ==
                        NumericComparison::kBetween) {
                        ImGuiMCP::SetNextItemWidth(-1.0f);
                        ImGuiMCP::InputFloat(
                            "##Maximum",
                            &filter.maximumValue,
                            0.0f,
                            0.0f,
                            "%.1f");
                    }
                    else {
                        filter.maximumValue =
                            filter.minimumValue;
                        ImGuiMCP::TextDisabled("-");
                    }
                    ImGuiMCP::TableSetColumnIndex(7);
                    if (IsActorValueFilterValid(filter)) {
                        ImGuiMCP::TextColored(
                            { 0.3f, 0.9f, 0.4f, 1.0f },
                            "VALID");
                    }
                    else {
                        ImGuiMCP::TextColored(
                            { 1.0f, 0.25f, 0.25f, 1.0f },
                            "INVALID");
                    }
                    ImGuiMCP::TableSetColumnIndex(8);
                    if (ImGuiMCP::Button("X")) {
                        filters.erase(
                            filters.begin() + index);
                        ImGuiMCP::PopID();
                        removedActorValue = true;
                        break;
                    }
                    ImGuiMCP::PopID();
                  }
                }
                ImGuiMCP::EndTable();
            }

            std::vector<std::size_t> regularFilterIndices;
            regularFilterIndices.reserve(filters.size());
            for (std::size_t index = 0;
                 index < filters.size();
                 ++index) {
                if (filters[index].type != "Actor Value") {
                    regularFilterIndices.push_back(index);
                }
            }
            if (ImGuiMCP::BeginTable(
                    a_blacklist ?
                        "BlacklistTable" :
                        "TargetsTable",
                    6,
                    ImGuiMCP::ImGuiTableFlags_Borders)) {
                ImGuiMCP::TableSetupColumn(
                    "Match",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    100.0f);
                ImGuiMCP::TableSetupColumn(
                    "Type",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    120.0f);
                ImGuiMCP::TableSetupColumn(
                    "Name",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                ImGuiMCP::TableSetupColumn(
                    "Condition",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                ImGuiMCP::TableSetupColumn(
                    "Identifier",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                ImGuiMCP::TableSetupColumn(
                    "Action",
                    ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                    60.0f);
                ImGuiMCP::TableHeadersRow();

                static auto filterClipper =
                    ImGuiMCP::ImGuiListClipperManager::Create();
                ImGuiMCP::ImGuiListClipperManager::Begin(
                    filterClipper,
                    static_cast<int>(regularFilterIndices.size()),
                    -1.0f);
                auto removedFilter = false;
                while (!removedFilter &&
                       ImGuiMCP::ImGuiListClipperManager::Step(
                           filterClipper)) {
                  for (auto visible = filterClipper->DisplayStart;
                       visible < filterClipper->DisplayEnd;
                       ++visible) {
                    const auto index = regularFilterIndices[
                        static_cast<std::size_t>(visible)];
                    auto& filter = filters[index];
                    ImGuiMCP::PushID(
                        static_cast<int>(index));
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    DistributionCore::UI::DrawFilterOperator(
                        "##FilterOperator", filter);
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::TextUnformatted(
                        filter.type.c_str());
                    ImGuiMCP::TableSetColumnIndex(2);
                    const auto& list =
                        Manager::GetSingleton()->GetList(
                            filter.type);
                    const auto found = std::ranges::find_if(
                        list,
                        [&](const InternalFormInfo& a_info) {
                            return MatchesForm(
                                filter, a_info);
                        });
                    ImGuiMCP::TextUnformatted(
                        found != list.end() ?
                            found->GetDisplayName().c_str() :
                            (!filter.optionText.empty() ?
                                filter.optionText.c_str() :
                                (!filter.editorID.empty() ?
                                    filter.editorID.c_str() :
                                    "Not Found")));

                    ImGuiMCP::TableSetColumnIndex(3);
                    if (IsNumericValueFilterType(
                            filter.type)) {
                        ImGuiMCP::SetNextItemWidth(120.0f);
                        EnumCombo(
                            "##Comparison",
                            filter.comparison,
                            {
                                { NumericComparison::kGreaterOrEqual, ">=" },
                                { NumericComparison::kLessOrEqual, "<=" },
                                { NumericComparison::kEqual, "=" },
                                { NumericComparison::kBetween, "Between" }
                            });
                        ImGuiMCP::SameLine();
                        ImGuiMCP::SetNextItemWidth(80.0f);
                        ImGuiMCP::InputFloat(
                            "##Minimum",
                            &filter.minimumValue,
                            0.0f,
                            0.0f,
                            "%.1f");
                        if (filter.comparison ==
                            NumericComparison::kBetween) {
                            ImGuiMCP::SameLine();
                            ImGuiMCP::SetNextItemWidth(80.0f);
                            ImGuiMCP::InputFloat(
                                "##Maximum",
                                &filter.maximumValue,
                                0.0f,
                                0.0f,
                                "%.1f");
                        }
                        else {
                            filter.maximumValue =
                                filter.minimumValue;
                        }
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
                        filter.type == "Source Plugin" ?
                            filter.optionText.c_str() :
                            filter.formIDStr.c_str());
                    ImGuiMCP::TableSetColumnIndex(5);
                    if (ImGuiMCP::Button("X")) {
                        filters.erase(
                            filters.begin() + index);
                        ImGuiMCP::PopID();
                        removedFilter = true;
                        break;
                    }
                    ImGuiMCP::PopID();
                  }
                }
                ImGuiMCP::EndTable();
            }
        }

        struct VanillaSkillOption
        {
            std::string_view id;
            std::string_view label;
        };

        constexpr std::array<VanillaSkillOption, 18> kVanillaSkills{ {
            { "OneHanded", "One Handed" },
            { "TwoHanded", "Two Handed" },
            { "Archery", "Archery" },
            { "Block", "Block" },
            { "Smithing", "Smithing" },
            { "HeavyArmor", "Heavy Armor" },
            { "LightArmor", "Light Armor" },
            { "Pickpocket", "Pickpocket" },
            { "Lockpicking", "Lockpicking" },
            { "Sneak", "Sneak" },
            { "Alchemy", "Alchemy" },
            { "Speech", "Speech" },
            { "Alteration", "Alteration" },
            { "Conjuration", "Conjuration" },
            { "Destruction", "Destruction" },
            { "Illusion", "Illusion" },
            { "Restoration", "Restoration" },
            { "Enchanting", "Enchanting" }
        } };

        std::string SkillDisplayName(const std::string_view a_reference)
        {
            const auto source = GetSkillSource(a_reference);
            const auto skillID = GetSkillID(a_reference);
            if (source == SkillSource::kVanilla) {
                const auto found = std::ranges::find_if(
                    kVanillaSkills,
                    [&](const VanillaSkillOption& a_skill) {
                        return a_skill.id == skillID;
                    });
                if (found != kVanillaSkills.end()) {
                    return std::format("{} (Vanilla)", found->label);
                }
            }
            if (source == SkillSource::kNSM && !skillID.empty()) {
                return std::format("{} (NSM)", skillID);
            }
            return {};
        }

        void InitializeSkillReward(Reward& a_reward)
        {
            if (GetSkillSource(a_reward.editorID) == SkillSource::kInvalid) {
                a_reward.editorID = MakeSkillReference(
                    SkillSource::kVanilla,
                    kVanillaSkills.front().id);
            }
            if (a_reward.typeReward == "Skill Level" &&
                a_reward.functionOnType !=
                    static_cast<int>(SkillLevelMode::kBonus)) {
                a_reward.functionOnType =
                    static_cast<int>(SkillLevelMode::kPermanent);
            }
        }

        void DrawSkillField(
            Reward& a_reward,
            const std::string_view a_stateID)
        {
            InitializeSkillReward(a_reward);
            const auto& skills =
                NewSkillMenu::AvailableSkills();
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                options;
            options.reserve(kVanillaSkills.size() + skills.size());
            for (const auto& skill : kVanillaSkills) {
                options.push_back({
                    MakeSkillReference(SkillSource::kVanilla, skill.id),
                    std::format("{} (Vanilla)", skill.label)
                });
            }
            for (const auto& skill : skills) {
                options.push_back({
                    MakeSkillReference(SkillSource::kNSM, skill),
                    std::format("{} (NSM)", skill)
                });
            }
            const auto preview = SkillDisplayName(a_reward.editorID);
            ImGuiMCP::ImVec2 available;
            ImGuiMCP::GetContentRegionAvail(&available);
            ImGuiMCP::SetNextItemWidth(
                std::max(160.0f, available.x - 105.0f));
            DistributionCore::UI::DrawSearchableCombo(
                "##SkillID",
                preview.empty() ? "Select Skill..." : preview.c_str(),
                a_stateID,
                options,
                a_reward.editorID,
                (static_cast<std::uint64_t>(
                    NewSkillMenu::InterfaceVersion()) << 32) |
                    skills.size());

            const auto source = GetSkillSource(a_reward.editorID);
            const auto skillID = GetSkillID(a_reward.editorID);
            const auto vanillaValid =
                source == SkillSource::kVanilla &&
                std::ranges::any_of(
                    kVanillaSkills,
                    [&](const VanillaSkillOption& a_skill) {
                        return a_skill.id == skillID;
                    });
            const auto nsmValid = source == SkillSource::kNSM &&
                NewSkillMenu::HasSkill(skillID);
            ImGuiMCP::SameLine();
            if (vanillaValid && a_reward.typeReward == "Skill XP") {
                ImGuiMCP::TextColored(
                    { 0.95f, 0.75f, 0.3f, 1.0f },
                    "PLAYER ONLY");
            }
            else if (vanillaValid || nsmValid) {
                ImGuiMCP::TextColored(
                    { 0.3f, 0.9f, 0.4f, 1.0f },
                    "VALID");
            }
            else {
                ImGuiMCP::TextColored(
                    { 1.0f, 0.65f, 0.2f, 1.0f },
                    "UNRESOLVED");
            }
        }

        void DrawSkillLevelMode(Reward& a_reward)
        {
            auto mode = a_reward.functionOnType ==
                    static_cast<int>(SkillLevelMode::kBonus) ?
                static_cast<int>(SkillLevelMode::kBonus) :
                static_cast<int>(SkillLevelMode::kPermanent);
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (EnumCombo(
                    "##SkillLevelMode",
                    mode,
                    { { static_cast<int>(SkillLevelMode::kPermanent),
                          "Permanent Level" },
                      { static_cast<int>(SkillLevelMode::kBonus),
                          "Bonus Level" } })) {
                a_reward.functionOnType = mode;
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(
                    mode == static_cast<int>(SkillLevelMode::kPermanent) ?
                        "Changes the skill's base level." :
                        "Adds a permanent modifier without changing its base level.");
            }
        }

        void DrawNSMResourceField(
            Reward& a_reward,
            const std::string_view a_stateID)
        {
            const auto& resources =
                NewSkillMenu::AvailableResources();
            ImGuiMCP::ImVec2 available;
            ImGuiMCP::GetContentRegionAvail(&available);
            ImGuiMCP::SetNextItemWidth(
                std::max(120.0f, available.x - 95.0f));
            if (!resources.empty()) {
                std::vector<
                    DistributionCore::UI::
                        SearchableComboOption>
                    options;
                options.reserve(resources.size());
                for (const auto& resource : resources) {
                    options.push_back({ resource, resource });
                }
                DistributionCore::UI::DrawSearchableCombo(
                    "##NSMResourceID",
                    a_reward.editorID.empty() ?
                        "Select Resource..." :
                        a_reward.editorID.c_str(),
                    a_stateID,
                    options,
                    a_reward.editorID,
                    resources.size());
            }
            else {
                InputString(
                    "##NSMResourceID",
                    a_reward.editorID);
            }
            ImGuiMCP::SameLine();
            if (NewSkillMenu::HasResource(
                    a_reward.editorID)) {
                ImGuiMCP::TextColored(
                    { 0.3f, 0.9f, 0.4f, 1.0f },
                    "VALID");
            }
            else {
                ImGuiMCP::TextColored(
                    { 1.0f, 0.65f, 0.2f, 1.0f },
                    "UNRESOLVED");
            }
        }

        void DrawEDFStyleRewardWorkspace(
            LootRule& a_lootRule)
        {
            auto& rule = a_lootRule.criteria;
            if (g_rewardPickerOpen) {
                if (ImGuiMCP::Button("Back")) {
                    g_rewardPickerOpen = false;
                }
                if (g_activeRewardGroup >= 0 &&
                    g_activeRewardGroup <
                        static_cast<int>(
                            rule.rewardGroups.size())) {
                    ImGuiMCP::SameLine();
                    ImGuiMCP::Text(
                        "Editing Group: %s",
                        rule.rewardGroups[
                            static_cast<std::size_t>(
                                g_activeRewardGroup)]
                            .name.c_str());
                    DrawSelectionTable(
                        nullptr,
                        std::addressof(
                            rule.rewardGroups[
                                static_cast<std::size_t>(
                                    g_activeRewardGroup)]));
                }
                return;
            }

            if (ImGuiMCP::Button("Back")) {
                g_rewardsOpen = false;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("+ New Group")) {
                std::string name = "New Group";
                for (int suffix = 1;
                     std::ranges::any_of(
                         rule.rewardGroups,
                         [&](const RewardGroup& a_group) {
                             return a_group.name == name;
                         });
                     ++suffix) {
                    name = std::format(
                        "New Group ({})",
                        suffix);
                }
                rule.rewardGroups.push_back(
                    RewardGroup{ .name = name });
            }
            ImGuiMCP::SameLine();
            if (NewSkillMenu::IsAvailable() ||
                NewSkillMenu::Initialize()) {
                if (ImGuiMCP::Button(
                        "Refresh NSM Lists")) {
                    NewSkillMenu::RefreshSkills();
                }
            }
            else {
                ImGuiMCP::TextDisabled(
                    "NSM unavailable");
            }
            constexpr ImGuiMCP::ImVec4 progressionTextColor{
                0.78f, 0.86f, 0.98f, 1.0f
            };
            ImGuiMCP::TextColored(
                progressionTextColor,
                "Progression rewards are applied directly and never go to the corpse.");
            ImGuiMCP::TextColored(
                progressionTextColor,
                "The receiver is the killer or defeater allowed by INLOS Loot Receivers.");
            ImGuiMCP::TextColored(
                progressionTextColor,
                "General experience and vanilla skill XP require the receiver to be the Player.");
            if (rule.isExclusive) {
                const auto total = std::accumulate(
                    rule.rewardGroups.begin(),
                    rule.rewardGroups.end(),
                    0.0f,
                    [](const float a_sum,
                       const RewardGroup& a_group) {
                        return a_sum + a_group.chanceGroup;
                    });
                ImGuiMCP::SameLine();
                if (total > 100.0f) {
                    ImGuiMCP::TextColored(
                        { 1.0f, 0.0f, 0.0f, 1.0f },
                        " [!] Group Sum: %.1f%% (Exceeds 100%%)",
                        total);
                }
                else {
                    ImGuiMCP::TextDisabled(
                        " | Group Sum: %.1f%%",
                        total);
                }
            }
            ImGuiMCP::Separator();

            const auto drawRewardGroups =
                ImGuiMCP::BeginChild(
                    "##INLOSRewardGroupsScroll",
                    { 0.0f, 0.0f },
                    false);
            if (drawRewardGroups) {
            for (std::size_t groupIndex = 0;
                 groupIndex < rule.rewardGroups.size();) {
                auto& group =
                    rule.rewardGroups[groupIndex];
                ImGuiMCP::PushID(
                    static_cast<int>(groupIndex));
                auto header = std::format(
                    "{} ({} rewards)###INLOSGroup{}",
                    group.name,
                    group.rewards.size(),
                    groupIndex);
                if (group.isExclusive) {
                    header = "[EXCL] " + header;
                }
                auto removeGroup = false;
                if (ImGuiMCP::CollapsingHeader(
                        header.c_str())) {
                    ImGuiMCP::Indent();
                    ImGuiMCP::SetNextItemWidth(200.0f);
                    InputString(
                        "Group Name",
                        group.name,
                        128);
                    ImGuiMCP::SameLine();
                    removeGroup =
                        ImGuiMCP::Button("Delete Group");

                    ImGuiMCP::SetNextItemWidth(200.0f);
                    if (ImGuiMCP::InputFloat(
                            "Activation Chance (%)",
                            &group.chanceGroup,
                            1.0f,
                            10.0f,
                            "%.1f")) {
                        group.chanceGroup = std::clamp(
                            group.chanceGroup,
                            0.0f,
                            100.0f);
                    }
                    if (ImGuiMCP::Button("Manage Rewards")) {
                        ResetSelectionState();
                        g_activeRewardGroup =
                            static_cast<int>(groupIndex);
                        g_rewardPickerOpen = true;
                    }
                    ImGuiMCP::SameLine();
                    ImGuiMCP::Checkbox(
                        "Exclusive (Picks only one from list)",
                        &group.isExclusive);

                    if (ImGuiMCP::Button(
                            "+ Player Experience")) {
                        Reward reward;
                        reward.typeReward = "Experience";
                        group.rewards.push_back(
                            std::move(reward));
                    }
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button(
                            "+ Skill XP")) {
                        Reward reward;
                        reward.typeReward = "Skill XP";
                        InitializeSkillReward(reward);
                        group.rewards.push_back(
                            std::move(reward));
                    }
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button(
                            "+ Skill Level")) {
                        Reward reward;
                        reward.typeReward = "Skill Level";
                        InitializeSkillReward(reward);
                        group.rewards.push_back(
                            std::move(reward));
                    }
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button(
                            "+ Perk Points")) {
                        Reward reward;
                        reward.typeReward = "Perk Points";
                        group.rewards.push_back(
                            std::move(reward));
                    }
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button(
                            "+ NSM Resource")) {
                        Reward reward;
                        reward.typeReward =
                            "NSM Resource";
                        if (!NewSkillMenu::
                                AvailableResources().empty()) {
                            reward.editorID =
                                NewSkillMenu::
                                    AvailableResources().front();
                        }
                        group.rewards.push_back(
                            std::move(reward));
                    }

                    if (group.isExclusive) {
                        const auto total =
                            std::accumulate(
                                group.rewards.begin(),
                                group.rewards.end(),
                                0.0f,
                                [](const float a_sum,
                                   const Reward& a_reward) {
                                    return a_sum +
                                        a_reward.chanceReward;
                                });
                        if (total > 100.0f) {
                            ImGuiMCP::TextColored(
                                { 1.0f, 0.0f, 0.0f, 1.0f },
                                "Warning: Sum of chances (%.1f%%) exceeds 100%%!",
                                total);
                        }
                        else {
                            ImGuiMCP::TextDisabled(
                                "Total accumulated chance: %.1f%%",
                                total);
                        }
                    }

                    ImGuiMCP::Spacing();
                    ImGuiMCP::TextUnformatted(
                        "Rewards in Group:");
                    const auto flags =
                        ImGuiMCP::ImGuiTableFlags_Borders |
                        ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable;
                    if (ImGuiMCP::BeginTable(
                            "GroupRewardsSummary",
                            5,
                            flags)) {
                        ImGuiMCP::TableSetupColumn(
                            "Type",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            150.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Reward",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableSetupColumn(
                            "Qty",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            70.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Chance",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            80.0f);
                        ImGuiMCP::TableSetupColumn(
                            "Action",
                            ImGuiMCP::ImGuiTableColumnFlags_WidthFixed,
                            40.0f);
                        ImGuiMCP::TableHeadersRow();

                        static auto rewardClipper =
                            ImGuiMCP::ImGuiListClipperManager::Create();
                        ImGuiMCP::ImGuiListClipperManager::Begin(
                            rewardClipper,
                            static_cast<int>(group.rewards.size()),
                            -1.0f);
                        auto removedReward = false;
                        while (!removedReward &&
                               ImGuiMCP::ImGuiListClipperManager::Step(
                                   rewardClipper)) {
                          for (auto visible = rewardClipper->DisplayStart;
                               visible < rewardClipper->DisplayEnd;
                               ++visible) {
                            const auto rewardIndex =
                                static_cast<std::size_t>(visible);
                            auto& reward =
                                group.rewards[rewardIndex];
                            ImGuiMCP::PushID(
                                static_cast<int>(rewardIndex));
                            ImGuiMCP::TableNextRow();
                            ImGuiMCP::TableSetColumnIndex(0);
                            ImGuiMCP::TextUnformatted(
                                reward.typeReward.c_str());
                            ImGuiMCP::TableSetColumnIndex(1);
                            if (reward.typeReward == "Skill XP" ||
                                reward.typeReward == "Skill Level") {
                                DrawSkillField(
                                    reward,
                                    std::format(
                                        "INLOS.{}.Skill.{}.{}",
                                        rule.id,
                                        groupIndex,
                                        rewardIndex));
                                if (reward.typeReward == "Skill Level") {
                                    DrawSkillLevelMode(reward);
                                }
                            }
                            else if (
                                reward.typeReward ==
                                "NSM Resource") {
                                DrawNSMResourceField(
                                    reward,
                                    std::format(
                                        "INLOS.{}.NSMResource.{}.{}",
                                        rule.id,
                                        groupIndex,
                                        rewardIndex));
                            }
                            else if (reward.typeReward ==
                                     "Experience") {
                                ImGuiMCP::TextUnformatted(
                                    "INLOS Experience");
                            }
                            else if (reward.typeReward ==
                                     "Perk Points") {
                                ImGuiMCP::TextUnformatted(
                                    "Configured Loot Receiver");
                            }
                            else {
                                const auto& list =
                                    Manager::GetSingleton()
                                        ->GetList(
                                            reward.typeReward);
                                const auto found =
                                    std::ranges::find_if(
                                        list,
                                        [&](const InternalFormInfo&
                                                a_info) {
                                            return MatchesForm(
                                                reward, a_info);
                                        });
                                ImGuiMCP::TextUnformatted(
                                    found != list.end() ?
                                        found->GetDisplayName().c_str() :
                                        (!reward.editorID.empty() ?
                                            reward.editorID.c_str() :
                                            reward.formIDStr.c_str()));
                            }
                            ImGuiMCP::TableSetColumnIndex(2);
                            auto amount =
                                static_cast<int>(reward.amount);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            if (ImGuiMCP::InputInt(
                                    "##Amount",
                                    &amount,
                                    0,
                                    0)) {
                                reward.amount =
                                    static_cast<std::uint32_t>(
                                        std::max(1, amount));
                            }
                            ImGuiMCP::TableSetColumnIndex(3);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            ImGuiMCP::InputFloat(
                                "##Chance",
                                &reward.chanceReward,
                                0.0f,
                                0.0f,
                                "%.1f");
                            reward.chanceReward =
                                std::clamp(
                                    reward.chanceReward,
                                    0.0f,
                                    100.0f);
                            ImGuiMCP::TableSetColumnIndex(4);
                            if (ImGuiMCP::Button("X")) {
                                group.rewards.erase(
                                    group.rewards.begin() +
                                    rewardIndex);
                                ImGuiMCP::PopID();
                                removedReward = true;
                                break;
                            }
                            ImGuiMCP::PopID();
                          }
                        }
                        ImGuiMCP::EndTable();
                    }
                    ImGuiMCP::Unindent();
                    ImGuiMCP::Separator();
                }
                ImGuiMCP::PopID();
                if (removeGroup) {
                    rule.rewardGroups.erase(
                        rule.rewardGroups.begin() +
                        groupIndex);
                }
                else {
                    ++groupIndex;
                }
            }
            }
            ImGuiMCP::EndChild();
        }

        void DrawReward(Reward& a_reward, bool& a_remove)
        {
            const auto options =
                DistributionCore::RewardRegistry().AvailableFor(
                    DistributionCore::Domain::kINLOS);
            if (ImGuiMCP::BeginCombo(
                    "Reward Type",
                    a_reward.typeReward.empty() ?
                        "Select..." :
                        a_reward.typeReward.c_str())) {
                for (const auto& option : options) {
                    const auto selected =
                        option.id == a_reward.typeReward;
                    if (ImGuiMCP::Selectable(
                            option.displayName.c_str(),
                            selected)) {
                        a_reward.typeReward = option.id;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            const auto* descriptor =
                DistributionCore::RewardRegistry().Find(
                    a_reward.typeReward);
            if (descriptor &&
                (descriptor->capabilities &
                    DistributionCore::ToMask(
                        DistributionCore::TypeCapability::kRequiresForm)) != 0 &&
                a_reward.typeReward != "Gold") {
                InputString("EditorID", a_reward.editorID);
                InputString(
                    "Plugin|FormID",
                    a_reward.formIDStr);
            }
            if (a_reward.typeReward == "Skill XP" ||
                a_reward.typeReward == "Skill Level") {
                DrawSkillField(a_reward, "INLOS.Legacy.Skill");
                if (a_reward.typeReward == "Skill Level") {
                    DrawSkillLevelMode(a_reward);
                }
            }
            auto amount = static_cast<int>(a_reward.amount);
            if (ImGuiMCP::InputInt("Amount", &amount)) {
                a_reward.amount = static_cast<std::uint32_t>(
                    std::max(1, amount));
            }
            ImGuiMCP::InputFloat(
                "Chance (%)",
                &a_reward.chanceReward,
                1.0f,
                10.0f,
                "%.2f");
            a_reward.chanceReward = std::clamp(
                a_reward.chanceReward, 0.0f, 100.0f);
            if (ImGuiMCP::Button("Remove Reward")) {
                a_remove = true;
            }
        }

        void DrawRewards(std::vector<RewardGroup>& a_groups)
        {
            if (!ImGuiMCP::CollapsingHeader("Rewards")) {
                return;
            }
            if (ImGuiMCP::Button("Add Group")) {
                a_groups.push_back(
                    RewardGroup{ .name = "Loot" });
            }
            for (std::size_t groupIndex = 0;
                 groupIndex < a_groups.size();) {
                auto& group = a_groups[groupIndex];
                ImGuiMCP::PushID(static_cast<int>(groupIndex));
                ImGuiMCP::Separator();
                InputString("Group Name", group.name);
                ImGuiMCP::Checkbox("Exclusive", &group.isExclusive);
                ImGuiMCP::InputFloat(
                    "Group Chance (%)",
                    &group.chanceGroup,
                    1.0f,
                    10.0f,
                    "%.2f");
                group.chanceGroup = std::clamp(
                    group.chanceGroup, 0.0f, 100.0f);
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Add Reward")) {
                    Reward reward;
                    reward.typeReward = "Gold";
                    group.rewards.push_back(std::move(reward));
                }
                bool removeGroup =
                    ImGuiMCP::Button("Remove Group");
                for (std::size_t rewardIndex = 0;
                     rewardIndex < group.rewards.size();) {
                    ImGuiMCP::PushID(
                        static_cast<int>(rewardIndex));
                    bool removeReward = false;
                    DrawReward(
                        group.rewards[rewardIndex],
                        removeReward);
                    ImGuiMCP::PopID();
                    if (removeReward) {
                        group.rewards.erase(
                            group.rewards.begin() + rewardIndex);
                    }
                    else {
                        ++rewardIndex;
                    }
                }
                ImGuiMCP::PopID();
                if (removeGroup) {
                    a_groups.erase(a_groups.begin() + groupIndex);
                }
                else {
                    ++groupIndex;
                }
            }
        }

        void DrawRule(LootRule& a_lootRule)
        {
            auto& rule = a_lootRule.criteria;
            ImGuiMCP::Checkbox("Enabled", &rule.isEnabled);
            InputString("Rule Name", rule.name);
            EnumCombo(
                "Trigger",
                a_lootRule.trigger,
                {
                    { Trigger::kDeath, "Death" },
                    { Trigger::kDefeat, "Defeat" },
                    { Trigger::kBoth, "Death or Defeat" }
                });
            EnumCombo(
                "Destination",
                a_lootRule.destination,
                {
                    { Destination::kVictim, "Victim / Corpse" },
                    { Destination::kPlayer, "Configured Loot Receiver" }
                });
            ImGuiMCP::Checkbox(
                "Require Player Killer",
                &a_lootRule.requirePlayerKiller);

            ImGuiMCP::SetNextItemWidth(170.0f);
            EnumCombo(
                "Level Operator",
                rule.levelComparison,
                {
                    { NumericComparison::kGreaterOrEqual, ">=" },
                    { NumericComparison::kLessOrEqual, "<=" },
                    { NumericComparison::kEqual, "=" },
                    { NumericComparison::kBetween, "Between" }
                });
            ImGuiMCP::InputInt("Level", &rule.level);
            rule.level = std::max(1, rule.level);
            if (rule.levelComparison == NumericComparison::kBetween) {
                ImGuiMCP::InputInt(
                    "Maximum Level",
                    &rule.maximumLevel);
                rule.maximumLevel =
                    std::max(1, rule.maximumLevel);
            }

            EnumCombo(
                "Gender",
                rule.targetGender,
                {
                    { 0, "All" },
                    { 1, "Male" },
                    { 2, "Female" }
                });
            EnumCombo(
                "Body",
                rule.targetHumanoid,
                {
                    { 0, "Both" },
                    { 1, "Humanoid Only" },
                    { 2, "Non-humanoid Only" }
                });
            EnumCombo(
                "Age",
                rule.targetChild,
                {
                    { 0, "Both" },
                    { 1, "Children Only" },
                    { 2, "Exclude Children" }
                });
            EnumCombo(
                "Summoned Status",
                rule.summonedState,
                {
                    { RuleSummonedState::kAny, "Any" },
                    { RuleSummonedState::kSummonedOnly, "Summoned Only" },
                    { RuleSummonedState::kExcludeSummoned, "Exclude Summoned" }
                });
            EnumCombo(
                "Hostility",
                rule.hostilityState,
                {
                    { RuleHostilityState::kAny, "Any" },
                    { RuleHostilityState::kHostileToPlayer, "Hostile" },
                    { RuleHostilityState::kFriendlyOrAlly, "Friendly / Ally" }
                });
            EnumCombo(
                "Follower Status",
                rule.followerState,
                {
                    { RuleFollowerState::kAny, "Any" },
                    { RuleFollowerState::kActiveOnly, "Active Followers Only" },
                    { RuleFollowerState::kExcludeActive, "Exclude Active Followers" }
                });

            if (ImGuiMCP::Button("Manage Targets")) {
                g_activeRule = rule.id;
                g_targetsOpen = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Manage Blacklist")) {
                g_activeRule = rule.id;
                g_blacklistOpen = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Manage Rewards")) {
                g_activeRule = rule.id;
                g_rewardsOpen = true;
            }
            ImGuiMCP::TextDisabled(
                "Targets: %d | Blacklist: %d | Groups: %d | Rewards: %d",
                static_cast<int>(rule.targetFilters.size()),
                static_cast<int>(rule.blacklistFilters.size()),
                static_cast<int>(rule.rewardGroups.size()),
                static_cast<int>(std::accumulate(
                    rule.rewardGroups.begin(),
                    rule.rewardGroups.end(),
                    std::size_t{ 0 },
                    [](const std::size_t a_total,
                       const RewardGroup& a_group) {
                        return a_total + a_group.rewards.size();
                    })));
        }

        void DrawEDFStyleRule(LootRule& a_lootRule)
        {
            auto& rule = a_lootRule.criteria;
            ImGuiMCP::Checkbox(
                "Rule Enabled",
                &rule.isEnabled);
            ImGuiMCP::SameLine();
            InputString("Rule Name", rule.name);

            ImGuiMCP::TextUnformatted("Trigger:");
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(160.0f);
            EnumCombo(
                "##Trigger",
                a_lootRule.trigger,
                {
                    { Trigger::kDeath, "Death" },
                    { Trigger::kDefeat, "Defeat" },
                    { Trigger::kBoth, "Death or Defeat" }
                });
            ImGuiMCP::SameLine();
            ImGuiMCP::TextUnformatted("Destination:");
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(220.0f);
            EnumCombo(
                "##Destination",
                a_lootRule.destination,
                {
                    { Destination::kVictim, "Victim / Corpse" },
                    { Destination::kPlayer, "Configured Loot Receiver" }
                });
            ImGuiMCP::SameLine();
            ImGuiMCP::Checkbox(
                "Require Player Killer",
                &a_lootRule.requirePlayerKiller);

            ImGuiMCP::TextUnformatted("Actor Level:");
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(170.0f);
            EnumCombo(
                "##LevelComparison",
                rule.levelComparison,
                {
                    { NumericComparison::kGreaterOrEqual, ">=" },
                    { NumericComparison::kLessOrEqual, "<=" },
                    { NumericComparison::kEqual, "=" },
                    { NumericComparison::kBetween, "Between" }
                });
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(90.0f);
            if (ImGuiMCP::InputInt(
                    "##Level",
                    &rule.level,
                    0,
                    0)) {
                rule.level = std::max(1, rule.level);
                if (rule.levelComparison ==
                        NumericComparison::kBetween &&
                    rule.maximumLevel < rule.level) {
                    rule.maximumLevel = rule.level;
                }
                else if (rule.levelComparison !=
                         NumericComparison::kBetween) {
                    rule.maximumLevel = rule.level;
                }
            }
            if (rule.levelComparison ==
                NumericComparison::kBetween) {
                ImGuiMCP::SameLine();
                ImGuiMCP::TextUnformatted("-");
                ImGuiMCP::SameLine();
                ImGuiMCP::SetNextItemWidth(90.0f);
                if (ImGuiMCP::InputInt(
                        "##MaximumLevel",
                        &rule.maximumLevel,
                        0,
                        0)) {
                    rule.maximumLevel = std::max(
                        rule.level,
                        rule.maximumLevel);
                }
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Manage Targets")) {
                ResetSelectionState();
                g_filterPickerOpen = false;
                g_activeRule = rule.id;
                g_targetsOpen = true;
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Manage Blacklist")) {
                ResetSelectionState();
                g_filterPickerOpen = false;
                g_activeRule = rule.id;
                g_blacklistOpen = true;
            }
            ImGuiMCP::Separator();
            ImGuiMCP::Separator();

            const auto totalRewards = std::accumulate(
                rule.rewardGroups.begin(),
                rule.rewardGroups.end(),
                std::size_t{ 0 },
                [](const std::size_t a_total,
                   const RewardGroup& a_group) {
                    return a_total + a_group.rewards.size();
                });
            const auto totalChance = std::accumulate(
                rule.rewardGroups.begin(),
                rule.rewardGroups.end(),
                0.0f,
                [](const float a_total,
                   const RewardGroup& a_group) {
                    return a_total + a_group.chanceGroup;
                });
            ImGuiMCP::Text(
                "Groups: %d | Total Items: %d",
                static_cast<int>(rule.rewardGroups.size()),
                static_cast<int>(totalRewards));
            ImGuiMCP::Checkbox(
                "Exclusive Groups (Pick only one group from this rule)",
                &rule.isExclusive);
            if (rule.isExclusive) {
                ImGuiMCP::SameLine();
                if (totalChance > 100.0f) {
                    ImGuiMCP::TextColored(
                        { 1.0f, 0.0f, 0.0f, 1.0f },
                        "(Sum: %.1f%% !)",
                        totalChance);
                }
                else {
                    ImGuiMCP::TextDisabled(
                        "(Sum: %.1f%%)",
                        totalChance);
                }
            }
            if (ImGuiMCP::Button("Manage Groups")) {
                g_activeRule = rule.id;
                g_activeRewardGroup = -1;
                g_rewardPickerOpen = false;
                g_rewardsOpen = true;
            }
        }
    }

    void RenderRules()
    {
        auto* store = Store::GetSingleton();
        const auto& packages = store->Packages();
        const auto findPackage =
            [&](const std::string_view a_id) -> const Package* {
                const auto found = std::ranges::find(
                    packages, a_id, &Package::id);
                return found != packages.end() ?
                    std::addressof(*found) :
                    nullptr;
            };
        if (!findPackage(g_activePackage) ||
            store->IsPackagePendingDeletion(g_activePackage)) {
            g_activePackage =
                std::string(Store::kLocalPackageID);
        }
        if (!g_packageFilter.empty() &&
            (!findPackage(g_packageFilter) ||
                store->IsPackagePendingDeletion(
                    g_packageFilter))) {
            g_packageFilter.clear();
        }

        if (ImGuiMCP::CollapsingHeader(
                "Package Workspace",
                ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto* active =
                findPackage(g_activePackage);
            std::vector<
                DistributionCore::UI::SearchableComboOption>
                packageOptions;
            packageOptions.reserve(packages.size());
            for (const auto& package : packages) {
                if (!store->IsPackagePendingDeletion(
                        package.id)) {
                    packageOptions.push_back({
                        package.id,
                        package.displayName
                    });
                }
            }
            const auto packageRevision =
                (static_cast<std::uint64_t>(
                    packages.size()) << 32) |
                store->PackagesPendingDeletion().size();
            ImGuiMCP::SetNextItemWidth(260.0f);
            DistributionCore::UI::DrawSearchableCombo(
                    "Active Package",
                    active ?
                        active->displayName.c_str() :
                        "Local Rules",
                    "INLOS.ActivePackage",
                    packageOptions,
                    g_activePackage,
                    packageRevision);
            if (active &&
                active->id != Store::kLocalPackageID) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button(
                        "Delete Active Package")) {
                    if (store->MarkPackageForDeletion(
                            active->id)) {
                        if (g_packageFilter == active->id) {
                            g_packageFilter.clear();
                        }
                        g_activePackage =
                            std::string(
                                Store::kLocalPackageID);
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
                g_newPackage,
                sizeof(g_newPackage));
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Create Package") &&
                g_newPackage[0] != '\0') {
                if (const auto created =
                        store->CreatePackage(g_newPackage)) {
                    g_activePackage = *created;
                    g_newPackage[0] = '\0';
                }
            }

            const auto* filtered =
                findPackage(g_packageFilter);
            auto packageFilterOptions = packageOptions;
            packageFilterOptions.insert(
                packageFilterOptions.begin(),
                { "", "All Packages" });
            ImGuiMCP::SetNextItemWidth(260.0f);
            DistributionCore::UI::DrawSearchableCombo(
                    "Package Filter",
                    filtered ?
                        filtered->displayName.c_str() :
                        "All Packages",
                    "INLOS.PackageFilter",
                    packageFilterOptions,
                    g_packageFilter,
                    packageRevision);

            if (!store->PackagesPendingDeletion().empty()) {
                ImGuiMCP::TextUnformatted(
                    "Pending Package Deletions");
                ImGuiMCP::SameLine();
                ImGuiMCP::TextDisabled(
                    "The package and its rules will only be deleted when Save is pressed.");
                for (const auto& packageID :
                     store->PackagesPendingDeletion()) {
                    const auto* package =
                        findPackage(packageID);
                    ImGuiMCP::PushID(packageID.c_str());
                    ImGuiMCP::TextUnformatted(
                        package ?
                            package->displayName.c_str() :
                            packageID.c_str());
                    ImGuiMCP::SameLine();
                    if (ImGuiMCP::Button("Undo")) {
                        store->CancelPackageDeletion(
                            packageID);
                    }
                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::Separator();
        }

        if (ImGuiMCP::Button(" + New Rule ")) {
            ImGuiMCP::OpenPopup("NewRule");
        }
        if (ImGuiMCP::BeginPopup("NewRule")) {
            ImGuiMCP::InputText(
                "Name",
                g_newRuleName,
                sizeof(g_newRuleName));
            if (ImGuiMCP::Button("Create")) {
                auto& created =
                    store->CreateRule(g_activePackage);
                if (g_newRuleName[0] != '\0') {
                    created.criteria.name =
                        g_newRuleName;
                }
                g_activeRule = created.criteria.id;
                g_newRuleName[0] = '\0';
                ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::EndPopup();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Save")) {
            store->SaveAll();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Export Active Package")) {
            if (store->SaveAll()) {
                store->ExportPackage(
                    g_activePackage, {});
            }
        }
        ImGuiMCP::Separator();
        ImGuiMCP::TextUnformatted("Active Filters:");
        if (g_activeTypeFilters.empty()) {
            ImGuiMCP::TextDisabled(
                "None (Showing all)");
        }
        for (auto iterator =
                 g_activeTypeFilters.begin();
             iterator != g_activeTypeFilters.end();) {
            if (ImGuiMCP::Button(
                    (*iterator + " x").c_str())) {
                iterator =
                    g_activeTypeFilters.erase(iterator);
            }
            else {
                ImGuiMCP::SameLine();
                ++iterator;
            }
        }
        ImGuiMCP::NewLine();
        if (ImGuiMCP::BeginCombo(
                "Add Type Filter",
                "Select...")) {
            for (const auto& descriptor :
                 DistributionCore::FilterRegistry().
                     AvailableFor(
                         DistributionCore::Domain::kINLOS)) {
                if (descriptor.id == "Actor Value") {
                    continue;
                }
                if (ImGuiMCP::Selectable(
                        descriptor.displayName.c_str(),
                        g_activeTypeFilters.contains(
                            descriptor.id))) {
                    g_activeTypeFilters.emplace(
                        descriptor.id);
                }
            }
            ImGuiMCP::EndCombo();
        }

        for (const auto& package : packages) {
            if (store->IsPackagePendingDeletion(package.id) ||
                (!g_packageFilter.empty() &&
                    package.id != g_packageFilter)) {
                continue;
            }
            std::vector<LootRule*> packageRules;
            for (auto& rule : store->Rules()) {
                if (rule.criteria.packageID != package.id) {
                    continue;
                }
                if (!g_activeTypeFilters.empty() &&
                    std::ranges::none_of(
                        rule.criteria.targetFilters,
                        [](const BlacklistFilter& a_filter) {
                            return g_activeTypeFilters.contains(
                                a_filter.type);
                        })) {
                    continue;
                }
                packageRules.push_back(
                    std::addressof(rule));
            }
            const auto packageHeader = std::format(
                "{} ({})###INLOSPackage{}",
                package.displayName,
                packageRules.size(),
                package.id);
            if (!ImGuiMCP::CollapsingHeader(
                    packageHeader.c_str())) {
                continue;
            }
            ImGuiMCP::Indent();
            for (auto* lootRule : packageRules) {
                ImGuiMCP::PushID(
                    lootRule->criteria.id.c_str());
                const auto modified =
                    store->IsRuleModified(*lootRule);
                auto displayName =
                    lootRule->criteria.name;
                if (!lootRule->criteria.isEnabled) {
                    displayName = "[OFF] " + displayName;
                }
                else if (modified) {
                    displayName += " (Need save)";
                }
                const auto title = std::format(
                    "{} [V:{}]###rule",
                    displayName,
                    lootRule->criteria.version);
                auto styleColors = 0;
                if (!lootRule->criteria.isEnabled) {
                    ImGuiMCP::PushStyleColor(
                        ImGuiMCP::ImGuiCol_Text,
                        { 0.5f, 0.5f, 0.5f, 1.0f });
                    ImGuiMCP::PushStyleColor(
                        ImGuiMCP::ImGuiCol_Header,
                        { 0.1f, 0.1f, 0.1f, 1.0f });
                    styleColors = 2;
                }
                else if (modified) {
                    ImGuiMCP::PushStyleColor(
                        ImGuiMCP::ImGuiCol_Header,
                        { 0.4f, 0.3f, 0.1f, 1.0f });
                    styleColors = 1;
                }
                if (ImGuiMCP::CollapsingHeader(
                        title.c_str())) {
                    if (styleColors > 0) {
                        ImGuiMCP::PopStyleColor(
                            styleColors);
                    }
                    DrawEDFStyleRule(*lootRule);
                    if (ImGuiMCP::Button("Delete Rule")) {
                        const auto id =
                            lootRule->criteria.id;
                        ImGuiMCP::PopID();
                        store->DeleteRule(id);
                        return;
                    }
                }
                else if (styleColors > 0) {
                    ImGuiMCP::PopStyleColor(styleColors);
                }
                ImGuiMCP::PopID();
            }
            ImGuiMCP::Unindent();
        }

        auto* activeRule =
            g_activeRule.empty() ?
            nullptr :
            store->FindRule(g_activeRule);
        const auto* viewport =
            ImGuiMCP::GetMainViewport();
        if (g_targetsOpen) {
            ImGuiMCP::SetNextWindowSize({
                viewport->Size.x / 1.2f,
                viewport->Size.y / 1.2f
            });
            ImGuiMCP::OpenPopup("Manage Targets");
        }
        if (ImGuiMCP::BeginPopupModal(
                "Manage Targets",
                &g_targetsOpen)) {
            if (activeRule) {
                ImGuiMCP::PushID(
                    activeRule->criteria.id.c_str());
                ImGuiMCP::TextColored(
                    { 0.78f, 0.86f, 0.98f, 1.0f },
                    "Rule: %s",
                    activeRule->criteria.name.c_str());
                ImGuiMCP::Separator();
                DrawEDFStyleFilterWorkspace(
                    *activeRule, false);
                ImGuiMCP::PopID();
            }
            else {
                g_targetsOpen = false;
            }
            ImGuiMCP::EndPopup();
        }
        if (g_blacklistOpen) {
            ImGuiMCP::SetNextWindowSize({
                viewport->Size.x / 1.2f,
                viewport->Size.y / 1.2f
            });
            ImGuiMCP::OpenPopup("Manage Blacklist");
        }
        if (ImGuiMCP::BeginPopupModal(
                "Manage Blacklist",
                &g_blacklistOpen)) {
            if (activeRule) {
                ImGuiMCP::PushID(
                    activeRule->criteria.id.c_str());
                ImGuiMCP::TextColored(
                    { 0.78f, 0.86f, 0.98f, 1.0f },
                    "Rule: %s",
                    activeRule->criteria.name.c_str());
                ImGuiMCP::Separator();
                DrawEDFStyleFilterWorkspace(
                    *activeRule, true);
                ImGuiMCP::PopID();
            }
            else {
                g_blacklistOpen = false;
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
            if (activeRule) {
                ImGuiMCP::PushID(
                    activeRule->criteria.id.c_str());
                ImGuiMCP::TextColored(
                    { 0.78f, 0.86f, 0.98f, 1.0f },
                    "Rule: %s",
                    activeRule->criteria.name.c_str());
                ImGuiMCP::Separator();
                DrawEDFStyleRewardWorkspace(
                    *activeRule);
                ImGuiMCP::PopID();
            }
            else {
                g_rewardsOpen = false;
            }
            ImGuiMCP::EndPopup();
        }
    }

    void RenderSettings()
    {
        auto* settings = Settings::GetSingleton();
        ImGuiMCP::Checkbox(
            "Enable Death Events",
            &settings->enableDeath);
        ImGuiMCP::Checkbox(
            "Enable Defeat Events",
            &settings->enableDefeat);
        ImGuiMCP::InputFloat(
            "Experience Multiplier",
            &settings->experienceMultiplier,
            0.1f,
            1.0f,
            "%.2f");
        settings->experienceMultiplier = std::clamp(
            settings->experienceMultiplier, 0.0f, 1000.0f);

        EnumCombo(
            "Vanilla Loot",
            settings->vanillaLootMode,
            {
                { VanillaLootMode::kDoNothing, "Do Nothing" },
                { VanillaLootMode::kAutoLoot, "Auto Loot" },
                { VanillaLootMode::kDiscard, "Discard Vanilla Loot" }
            });
        if (settings->vanillaLootMode ==
            VanillaLootMode::kAutoLoot) {
            ImGuiMCP::Checkbox(
                "Follower / Companion Loot Goes to Player",
                &settings->followerVanillaLootToPlayer);
            ImGuiMCP::TextWrapped(
                "The killer receives the victim's existing inventory. "
                "A summon routes it to its commanding actor.");
        }
        else if (settings->vanillaLootMode ==
                 VanillaLootMode::kDiscard) {
            ImGuiMCP::Checkbox(
                "Preserve Quest Items",
                &settings->preserveQuestItemsWhenDiscarding);
        }

        EnumCombo(
            "INLOS Loot Receivers",
            settings->lootRecipientMode,
            {
                { LootRecipientMode::kPlayerOnly, "Player Only" },
                { LootRecipientMode::kAnyActor, "Any Actor" },
                {
                    LootRecipientMode::kPlayerAndFollowers,
                    "Player / Active Followers"
                }
            });
        ImGuiMCP::TextWrapped(
            "This setting controls rules whose destination is "
            "'Configured Loot Receiver'. Summons resolve to their owner.");
        ImGuiMCP::Separator();
        ImGuiMCP::TextUnformatted("Duplicate Spell and Perk Rewards");
        ImGuiMCP::Checkbox(
            "Give Spell Tome When Spell Is Already Known",
            &settings->giveSpellTomeWhenKnown);
        ImGuiMCP::Checkbox(
            "Give Book of Perks When Perk Is Already Owned",
            &settings->givePerkBookWhenOwned);
        ImGuiMCP::TextWrapped(
            "These options affect living loot receivers that already own "
            "the reward. Loot sent to a corpse remains physical and uses "
            "the corresponding book when one is available.");
        if (ImGuiMCP::Button("Save Settings")) {
            settings->Save();
        }
        ImGuiMCP::Separator();
        ImGuiMCP::TextWrapped(
            "Defeat integration contract:");
        ImGuiMCP::BulletText(
            "INLOSActorDefeated: sender is the defeated Actor; "
            "strArg may be VictimHex|InstigatorHex.");
        ImGuiMCP::BulletText(
            "INLOSActorRecovered: sender is the recovered Actor.");
        ImGuiMCP::BulletText(
            "INLOSExperienceGained: numArg is gained XP and strArg is total XP.");
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::error(
                "[INLOS] SKSEMenuFramework is not installed.");
            return;
        }
        LoadLanguage();
        SKSEMenuFramework::SetSection("Distribution System");
        SKSEMenuFramework::AddSectionItem(
            std::format(
                "INLOS/{}",
                GetLoc("menu.loot_rules", "Loot Rules")),
            RenderRules);
        SKSEMenuFramework::AddSectionItem(
            std::format(
                "INLOS/{}",
                GetLoc("menu.settings", "Settings")),
            RenderSettings);
        logger::info("[INLOS] UI registered.");
    }
}
