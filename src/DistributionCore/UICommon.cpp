#include "DistributionCore/UICommon.h"
#include "Rule.h"
#include "SKSEMCP/SKSEMenuFramework.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace DistributionCore::UI
{
    namespace
    {
        struct SearchableComboState
        {
            char search[128]{};
            std::string appliedSearch;
            std::uint64_t optionsRevision =
                static_cast<std::uint64_t>(-1);
            std::size_t optionsSize = 0;
            std::vector<std::size_t> filteredIndices;
        };

        std::string ToLowerASCII(const std::string_view a_value)
        {
            std::string result(a_value);
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character) {
                    return static_cast<char>(
                        std::tolower(a_character));
                });
            return result;
        }
    }

    bool DrawSearchableCombo(
        const char* a_label,
        const char* a_preview,
        const std::string_view a_stateID,
        const std::vector<SearchableComboOption>& a_options,
        std::string& a_selectedValue,
        const std::uint64_t a_optionsRevision,
        const char* a_searchHint,
        const char* a_emptyText)
    {
        static std::unordered_map<
            std::string,
            SearchableComboState> states;
        static auto clipper =
            ImGuiMCP::ImGuiListClipperManager::Create();

        const std::string stateKey(a_stateID);
        ImGuiMCP::PushID(stateKey.c_str());
        if (!ImGuiMCP::BeginCombo(a_label, a_preview)) {
            ImGuiMCP::PopID();
            return false;
        }
        auto& state = states[stateKey];
        const auto appearing = ImGuiMCP::IsWindowAppearing();
        if (appearing) {
            state.search[0] = '\0';
            state.appliedSearch.clear();
            state.optionsRevision =
                static_cast<std::uint64_t>(-1);
            ImGuiMCP::SetKeyboardFocusHere();
        }

        ImGuiMCP::SetNextItemWidth(-1.0f);
        const auto searchChanged =
            ImGuiMCP::InputTextWithHint(
                "##Search",
                a_searchHint,
                state.search,
                sizeof(state.search));
        const auto normalizedSearch =
            ToLowerASCII(state.search);
        if (searchChanged ||
            state.appliedSearch != normalizedSearch ||
            state.optionsRevision != a_optionsRevision ||
            state.optionsSize != a_options.size()) {
            state.filteredIndices.clear();
            state.filteredIndices.reserve(a_options.size());
            for (std::size_t index = 0;
                 index < a_options.size();
                 ++index) {
                if (normalizedSearch.empty() ||
                    ToLowerASCII(a_options[index].label).
                        contains(normalizedSearch)) {
                    state.filteredIndices.push_back(index);
                }
            }
            state.appliedSearch = normalizedSearch;
            state.optionsRevision = a_optionsRevision;
            state.optionsSize = a_options.size();
        }

        ImGuiMCP::Separator();
        bool changed = false;
        if (ImGuiMCP::BeginChild(
                "##Results",
                { 0.0f, 220.0f },
                0)) {
            if (state.filteredIndices.empty()) {
                ImGuiMCP::TextDisabled(a_emptyText);
            }
            else {
                ImGuiMCP::ImGuiListClipperManager::Begin(
                    clipper,
                    static_cast<int>(
                        state.filteredIndices.size()),
                    -1.0f);
                if (appearing) {
                    const auto selected = std::ranges::find_if(
                        state.filteredIndices,
                        [&](const std::size_t a_index) {
                            return a_options[a_index].value ==
                                a_selectedValue;
                        });
                    if (selected != state.filteredIndices.end()) {
                        ImGuiMCP::ImGuiListClipperManager::
                            IncludeItemByIndex(
                                clipper,
                                static_cast<int>(std::distance(
                                    state.filteredIndices.begin(),
                                    selected)));
                    }
                }
                while (ImGuiMCP::ImGuiListClipperManager::Step(
                    clipper)) {
                    for (auto visible = clipper->DisplayStart;
                         visible < clipper->DisplayEnd;
                         ++visible) {
                        const auto optionIndex =
                            state.filteredIndices[
                                static_cast<std::size_t>(visible)];
                        const auto& option =
                            a_options[optionIndex];
                        ImGuiMCP::PushID(option.value.c_str());
                        if (ImGuiMCP::Selectable(
                                option.label.c_str(),
                                option.value ==
                                    a_selectedValue)) {
                            a_selectedValue = option.value;
                            changed = true;
                        }
                        ImGuiMCP::PopID();
                    }
                }
            }
        }
        ImGuiMCP::EndChild();
        ImGuiMCP::EndCombo();
        ImGuiMCP::PopID();
        return changed;
    }

    bool DrawFilterOperator(
        const char* a_label,
        BlacklistFilter& a_filter)
    {
        const char* preview = a_filter.isNot ? "IS NOT" : "IS";
        ImGuiMCP::SetNextItemWidth(90.0f);
        const auto open = ImGuiMCP::BeginCombo(a_label, preview);
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip(
                "Inverts this filter before the list's AND/OR operation.");
        }
        if (!open) {
            return false;
        }
        bool changed = false;
        if (ImGuiMCP::Selectable("IS", !a_filter.isNot)) {
            a_filter.isNot = false;
            changed = true;
        }
        if (ImGuiMCP::Selectable("IS NOT", a_filter.isNot)) {
            a_filter.isNot = true;
            changed = true;
        }
        ImGuiMCP::EndCombo();
        return changed;
    }
}
