#pragma once

#include "INLOS/Rule.h"

#include <mutex>

namespace INLOS
{
    class Store
    {
    public:
        static constexpr std::string_view kLocalPackageID =
            "inlos.local-rules";
        static constexpr int kSchemaVersion = 1;

        static Store* GetSingleton();

        bool Load();
        bool SaveAll();
        std::optional<std::string> CreatePackage(
            std::string_view a_displayName);
        LootRule& CreateRule(std::string_view a_packageID);
        bool DeleteRule(std::string_view a_ruleID);
        bool ExportRulesPackage(
            std::string_view a_archiveName,
            const std::set<std::string>& a_ruleIDs);
        bool MarkPackageForDeletion(std::string_view a_packageID);
        bool CancelPackageDeletion(std::string_view a_packageID);
        bool IsPackagePendingDeletion(
            std::string_view a_packageID) const;
        const std::set<std::string>& PackagesPendingDeletion() const
        {
            return _packagesToDelete;
        }

        std::vector<LootRule>& Rules() { return _rules; }
        const std::vector<LootRule>& Rules() const { return _rules; }
        const std::vector<Package>& Packages() const { return _packages; }
        LootRule* FindRule(std::string_view a_ruleID);
        bool IsRuleModified(const LootRule& a_rule) const;

    private:
        bool EnsurePackage(Package& a_package);
        bool LoadPackage(const Package& a_package);
        bool SaveRule(LootRule& a_rule);
        bool DeletePackageNow(std::string_view a_packageID);
        void RebuildIndices();

        std::mutex _lock;
        std::vector<Package> _packages;
        std::vector<LootRule> _rules;
        std::unordered_map<std::string, std::size_t> _ruleIndices;
        std::set<std::string> _packagesToDelete;
    };
}
