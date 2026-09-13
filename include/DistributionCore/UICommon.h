#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct BlacklistFilter;

namespace DistributionCore::UI
{
    struct SearchableComboOption
    {
        std::string value;
        std::string label;
    };

    bool DrawSearchableCombo(
        const char* a_label,
        const char* a_preview,
        std::string_view a_stateID,
        const std::vector<SearchableComboOption>& a_options,
        std::string& a_selectedValue,
        std::uint64_t a_optionsRevision,
        const char* a_searchHint = "Search...",
        const char* a_emptyText = "No items found.");

    bool DrawFilterOperator(
        const char* a_label,
        BlacklistFilter& a_filter);
}
