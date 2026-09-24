#pragma once

#include "Rule.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class RulePackageStore
{
public:
    static RulePackageStore* GetSingleton()
    {
        static RulePackageStore singleton;
        return &singleton;
    }

    static constexpr std::string_view LOCAL_PACKAGE_ID = "edf.local-rules";
    static constexpr std::string_view LOCAL_PACKAGE_NAME = "Local Rules";
    static constexpr std::string_view PACKAGES_DIR = "Data/Viny Mods/EDF/Packages";

    bool Load(
        std::vector<Rule>& rules,
        std::map<std::string, std::vector<Rule>>& histories,
        std::map<std::string, std::string>& owners,
        std::set<std::string>& deletedRuleIDs);

    bool SaveRule(Rule& rule, std::vector<Rule>& history);
    bool MarkRuleDeleted(std::string_view ruleID, std::string_view packageID);
    bool DeletePackage(std::string_view packageID);

    std::optional<std::string> CreatePackage(
        std::string_view displayName,
        std::string_view requestedID = {});
    bool RenamePackage(std::string_view packageID, std::string_view displayName);
    const std::vector<RulePackage>& GetPackages() const { return _packages; }

    bool CreateSnapshot(
        std::string_view displayName,
        const std::vector<Rule>& rules,
        const std::map<std::string, std::vector<Rule>>& histories,
        const std::filesystem::path& stagingRoot,
        RulePackage& outPackage);
    bool CreateSnapshot(
        const RulePackage& sourcePackage,
        const std::vector<Rule>& rules,
        const std::map<std::string, std::vector<Rule>>& histories,
        const std::filesystem::path& stagingRoot,
        RulePackage& outPackage);

private:
    bool WriteSnapshot(
        RulePackage package,
        const std::vector<Rule>& rules,
        const std::map<std::string, std::vector<Rule>>& histories,
        RulePackage& outPackage);

    std::vector<RulePackage> _packages;
};
