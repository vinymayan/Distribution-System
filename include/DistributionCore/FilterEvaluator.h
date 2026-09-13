#pragma once

#include <Rule.h>

#include <functional>

namespace DistributionCore
{
    enum class FilterEvaluation : std::uint8_t
    {
        kNotHandled = 0,
        kNoMatch = 1,
        kMatch = 2,
        kInvalid = 3
    };

    struct FilterEvaluationServices
    {
        std::function<RE::FormID(
            std::string_view,
            std::string_view,
            std::string_view)> resolveFormID;
        std::function<bool(RE::Actor*, RE::BGSKeyword*)>
            hasVirtualKeyword;
    };

    FilterEvaluation EvaluateFilter(
        RE::Actor* a_actor,
        RE::TESNPC* a_npc,
        const BlacklistFilter& a_filter,
        const FilterEvaluationServices& a_services);

    [[nodiscard]] bool IsCityLocation(RE::BGSLocation* a_location);
}
