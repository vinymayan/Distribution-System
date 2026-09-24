#pragma once

#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <optional>
#include <cstdint>
#include <unordered_map>
#include <rapidjson/document.h>
#include "Manager.h"

std::vector<std::string> split(const std::string& s, char delimiter);

enum class RuleCombatState : std::uint8_t {
    kAny = 0,
    kInCombat = 1,
    kOutOfCombat = 2
};

enum class RuleFollowerState : std::uint8_t {
    kAny = 0,
    kActiveOnly = 1,
    kExcludeActive = 2
};

enum class RuleActorScope : std::uint8_t {
    kBoth = 0,
    kPlayerOnly = 1,
    kNPCOnly = 2
};

enum class RuleSummonedState : std::uint8_t {
    kAny = 0,
    kSummonedOnly = 1,
    kExcludeSummoned = 2
};

enum class RuleHostilityState : std::uint8_t {
    kAny = 0,
    kHostileToPlayer = 1,
    kFriendlyOrAlly = 2
};

enum class ActorValueMode : std::uint8_t {
    kCurrent = 0,
    kPermanent = 1,
    kMaximum = 2
};

enum class NumericComparison : std::uint8_t {
    kGreaterOrEqual = 0,
    kLessOrEqual = 1,
    kEqual = 2,
    kBetween = 3
};

enum class NumericRewardOperation : std::uint8_t {
    kFlat = 0,
    kPercent = 1
};

enum class NumericRewardSource : std::uint8_t {
    kFixed = 0,
    kGlobal = 1,
    kActorValue = 2
};

enum class NumericRewardBinding : std::uint8_t {
    kSnapshot = 0,
    kLive = 1
};

constexpr float CalculateNumericRewardDelta(
    const float a_baseline,
    const float a_sourceValue,
    const NumericRewardOperation a_operation)
{
    return a_operation == NumericRewardOperation::kPercent ?
        a_baseline * a_sourceValue / 100.0f :
        a_sourceValue;
}

constexpr float CalculateNumericRewardAdjustment(
    const float a_desiredDelta,
    const float a_previousDelta)
{
    return a_desiredDelta - a_previousDelta;
}

static_assert(CalculateNumericRewardDelta(
                  200.0f, 10.0f,
                  NumericRewardOperation::kPercent) == 20.0f);
static_assert(CalculateNumericRewardAdjustment(-15.0f, 5.0f) == -20.0f);

enum class EquipmentContext : std::uint8_t {
    kNormal = 1u << 0,
    kSleep = 1u << 1,
    kCombat = 1u << 2
};

using EquipmentContextMask = std::uint8_t;

constexpr EquipmentContextMask ToMask(EquipmentContext context)
{
    return static_cast<EquipmentContextMask>(context);
}

constexpr EquipmentContextMask kAllEquipmentContexts =
    ToMask(EquipmentContext::kNormal) |
    ToMask(EquipmentContext::kSleep) |
    ToMask(EquipmentContext::kCombat);

bool IsEquipmentRewardType(std::string_view type);

struct Reward {
    std::string typeReward; // "Spell", "Perk", "Weapon", "Keyword"
    std::string formIDStr;  // e.g. "Skyrim.esm|D8D4E" (Plugin|FormID) or "D8D4E" (FormID only - unsafe without plugin)
    std::string editorID;
    uint32_t amount = 1;
    float chanceReward = 100.0f;
    int functionOnType = 0;
    EquipmentContextMask equipContexts = ToMask(EquipmentContext::kNormal);
    bool isPersistent = true;
    // Numeric payload used by form-less EDF rewards such as Actor Value and
    // Actor Scale.
    // Kept separate from amount because physical rewards require an unsigned
    // item count while Actor Value bonuses and penalties are signed floats.
    std::string actorValueName;
    float actorValueAmount = 0.0f;
    NumericRewardOperation numericOperation =
        NumericRewardOperation::kFlat;
    NumericRewardSource numericSource =
        NumericRewardSource::kFixed;
    NumericRewardBinding numericBinding =
        NumericRewardBinding::kSnapshot;
    ActorValueMode percentBaseMode = ActorValueMode::kPermanent;
    std::string sourceActorValueName;
    ActorValueMode sourceActorValueMode = ActorValueMode::kCurrent;
    std::string sourceGlobalFormID;
    std::string sourceGlobalEditorID;
    float sourceMultiplier = 1.0f;
    //bool lootable = true;
    // Helper to separate Plugin | FormID
    std::pair<std::string, RE::FormID> ParseFormID() const;
};

struct RewardGroup {
    std::string name = "New Group";
    bool isExclusive = false; // Modo Rolagem: Independente vs Exclusivo
    float chanceGroup = 100.0f;
    std::vector<Reward> rewards;
};

struct BlacklistFilter {
    std::string type;      // "NPC", "Faction", "Race", "Keyword"
    std::string formIDStr; // Plugin|FormID
    std::string editorID;
    std::string actorValueName;
    // Type-specific data for filters that are not represented by a single form.
    // Keeping these fields generic lets schema v1 remain forwards-compatible.
    int optionMode = 0;
    int optionValue = 0;
    std::string optionText;
    ActorValueMode actorValueMode = ActorValueMode::kCurrent;
    NumericComparison comparison = NumericComparison::kGreaterOrEqual;
    float minimumValue = 0.0f;
    float maximumValue = 0.0f;
    bool isNot = false;
};

[[nodiscard]] constexpr bool ApplyFilterOperator(
    const bool a_matches,
    const bool a_isNot) noexcept
{
    return a_isNot ? !a_matches : a_matches;
}

static_assert(ApplyFilterOperator(true, false));
static_assert(!ApplyFilterOperator(true, true));
static_assert(ApplyFilterOperator(false, true));

enum class NPCTraitFilter : int {
    kUnique = 0,
    kEssential = 1,
    kProtected = 2
};

enum class QuestFilterMode : int {
    kRunning = 0,
    kCompleted = 1,
    kStopped = 2,
    kNotStarted = 3,
    kStage = 4,
    kSpecificAlias = 5,
    kAnyAlias = 6
};

enum class CellTypeFilter : int {
    kInterior = 0,
    kExterior = 1
};

enum class CityStatusFilter : int {
    kInsideCity = 0,
    kOutsideCity = 1
};

enum class EquippedCategoryFilter : int {
    kUnarmed = 0,
    kAnyWeapon = 1,
    kOneHanded = 2,
    kTwoHanded = 3,
    kBow = 4,
    kCrossbow = 5,
    kStaff = 6,
    kShield = 7,
    kHeavyArmor = 8,
    kLightArmor = 9,
    kClothing = 10
};

RE::ActorValue ResolveActorValue(std::string_view a_name);
bool IsMaximumActorValueSupported(RE::ActorValue a_actorValue);
bool IsActorValueFilterValid(const BlacklistFilter& a_filter);
bool IsActorValueRewardValid(const Reward& a_reward);
bool IsActorScaleRewardValid(const Reward& a_reward);
bool IsNumericValueFilterType(std::string_view a_type);
void NormalizeNumericValueFilter(BlacklistFilter& a_filter);


struct Rule {
    std::string id;
    std::string packageID = "edf.local-rules";
    std::string name;
    bool isEnabled = true;
    std::string type = "NPC";
    int level = 1;
    NumericComparison levelComparison =
        NumericComparison::kGreaterOrEqual;
    int maximumLevel = 1;
    int version = 0;
    // Novos campos de Alvos (Substituem type e filterFormIDs)
    int targetGender = 0;
    int targetHumanoid = 0;  // 0: Both, 1: Only humanoids, 2: Only non-humanoids
    int targetChild = 0;     // 0: Both, 1: Only children, 2: Only non-children
    RuleCombatState combatState = RuleCombatState::kAny;
    RuleFollowerState followerState = RuleFollowerState::kAny;
    RuleActorScope actorScope = RuleActorScope::kBoth;
    RuleSummonedState summonedState = RuleSummonedState::kAny;
    RuleHostilityState hostilityState = RuleHostilityState::kAny;
    bool targetRequiresAll = false;
    bool isExclusive = false;
    std::vector<BlacklistFilter> targetFilters; // Usando a mesma struct de filtro
    std::vector<RewardGroup> rewardGroups;

    int blacklistedGender = 0;       // 0: Nenhum, 1: Male, 2: Female
    int blacklistedHumanoid = 0;     // 0: Nenhum, 1: Humanoid, 2: Non-humanoid
    int blacklistedChild = 0;        // 0: Nenhum, 1: Child, 2: Non-child
    bool blacklistRequiresAll = false;
    std::vector<BlacklistFilter> blacklistFilters;

    mutable std::string lastSavedHash;
    // Calcula um hash baseado no conteúdo estrutural da regra
    std::string CalculateHash() const;

    bool IsModified() const {
        return lastSavedHash != CalculateHash();
    }
};

bool MatchesRuleLevel(int actorLevel, const Rule& rule);

struct RulePackage {
    std::string id;
    std::string displayName;
    bool enabled = true;
    std::filesystem::path path;
    int schemaVersion = 1;
};

enum class RuleDependency : std::uint32_t {
    kNone = 0,
    kStatic = 1u << 0,
    kTag = 1u << 1,
    kFactionRank = 1u << 2,
    kAbility = 1u << 3,
    kInventory = 1u << 4,
    kEnvironment = 1u << 5,
    kLevel = 1u << 6,
    kSleep = 1u << 7,
    kCombat = 1u << 8,
    kActorValue = 1u << 9,
    kFollower = 1u << 10,
    kQuest = 1u << 11,
    kRelationship = 1u << 12,
    kEquipment = 1u << 13,
    kAll = (1u << 14) - 1u
};

using RuleDependencyMask = std::uint32_t;

constexpr RuleDependencyMask ToMask(RuleDependency a_dependency)
{
    return static_cast<RuleDependencyMask>(a_dependency);
}

struct RuleEvaluationDelta {
    RuleDependencyMask mask = ToMask(RuleDependency::kAll);
    std::set<RE::FormID> changedForms;
    std::set<RE::ActorValue> changedActorValues;
    bool allowEquipmentReconciliation = false;

    static RuleEvaluationDelta Full()
    {
        RuleEvaluationDelta delta;
        delta.allowEquipmentReconciliation = true;
        return delta;
    }

    static RuleEvaluationDelta For(
        RuleDependency a_dependency,
        RE::FormID a_changedForm = 0)
    {
        RuleEvaluationDelta delta;
        delta.mask = ToMask(a_dependency);
        if (a_changedForm != 0) {
            delta.changedForms.insert(a_changedForm);
        }
        return delta;
    }

    bool IsFull() const
    {
        return mask == ToMask(RuleDependency::kAll);
    }

    void Merge(const RuleEvaluationDelta& a_other)
    {
        mask |= a_other.mask;
        changedForms.insert(a_other.changedForms.begin(), a_other.changedForms.end());
        changedActorValues.insert(
            a_other.changedActorValues.begin(),
            a_other.changedActorValues.end());
        allowEquipmentReconciliation |=
            a_other.allowEquipmentReconciliation;
    }
};

RuleDependencyMask GetFilterDependencyMask(std::string_view a_type);
RuleDependencyMask GetRewardDependencyMask(std::string_view a_type);

bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist, RE::Actor* actor = nullptr);
bool IsActivePlayerFollower(RE::Actor* actor);

struct AffectedNPC {
    RE::FormID npcFormID;
    std::string npcName;
    std::vector<std::string> ruleIDs; // IDs das regras que afetam este NPC
};

class RuleManager {
public:
    static RuleManager* GetSingleton() {
        static RuleManager singleton;
        return &singleton;
    }

    bool IsAffected(RE::Actor* actor);

    void LoadRules();
    bool SaveRules();
    bool SaveRule(std::string_view ruleID);
    bool ExportRule(const Rule& rule);
    bool ExportRulesPackage(const std::string& packageName, const std::set<std::string>& ruleIDs);
    std::vector<Rule>& GetRules() { return _rules; }
    Rule* FindRule(const std::string& ruleID);
    const Rule* FindRule(const std::string& ruleID) const;
    const std::vector<RulePackage>& GetPackages() const;
    std::optional<std::string> CreatePackage(
        std::string_view displayName,
        std::string_view requestedID = {});
    bool RenamePackage(std::string_view packageID, std::string_view displayName);
    bool MarkPackageForDeletion(std::string_view packageID);
    bool CancelPackageDeletion(std::string_view packageID);
    bool IsPackagePendingDeletion(std::string_view packageID) const;
    const std::set<std::string>& GetPackagesPendingDeletion() const {
        return _packagesToDelete;
    }

    // Create specific rule
    Rule& CreateRule(std::string_view packageID = "edf.local-rules");
    std::optional<std::string> DuplicateRule(
        std::string_view sourceRuleID,
        std::string_view destinationPackageID,
        std::string_view copyName = {});
    bool  DeleteRule(const std::string& id);
    bool IsRulePendingDeletion(std::string_view ruleID) const {
        return _deletedRuleIDs.contains(std::string(ruleID));
    }
    bool HasPendingRuleDeletions() const {
        return !_deletedRuleIDs.empty();
    }

    bool CreateRulesPackageSnapshot(
        const std::string& packageName,
        const std::vector<Rule>& rules,
        const std::filesystem::path& stagingRoot,
        RulePackage& outPackage);

    // Apply rules to an NPC (Validation logic)
    // Returns list of rewards to apply
    std::vector<Reward> GetRewardsForNPC(RE::TESNPC* npc);

    std::vector<RewardGroup> RollForGroups(RE::TESNPC* npc, const Rule& rule);
    // Adicione a declaração na classe RuleManager
    std::vector<Reward> GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule);

    void InitializeAffectedNPCsDatabase();
    void RebuildDependencyIndex(bool invalidateActorSnapshots = true);
    std::vector<std::string> GetCandidateRuleIDs(const RuleEvaluationDelta& a_delta) const;
    std::vector<std::string> GetCandidateRuleIDs(
        RE::Actor* a_actor,
        const RuleEvaluationDelta& a_delta) const;
    RuleDependencyMask GetRuleDependencyMask(std::string_view a_ruleID) const;
    bool IsRuleInUnstableCycle(std::string_view a_ruleID) const;
    RuleEvaluationDelta DetectBaseNPCChanges(RE::Actor* a_actor);
    RuleEvaluationDelta DetectActorValueChanges(RE::Actor* a_actor);
    RuleEvaluationDelta DetectFollowerStateChanges(RE::Actor* a_actor);
    void InvalidateBaseNPCState(RE::FormID a_npcFormID);
    void ForgetActorRuntimeState(RE::FormID a_actorID);
    void ResetRuntimeCaches();
    std::uint64_t GetDependencyRevision() const { return _dependencyRevision; }
    float GetRandomFloat(float a_min, float a_max) {
        static thread_local std::mt19937 gen{ std::random_device{}() };
        std::uniform_real_distribution<float> dis(a_min, a_max);
        return dis(gen);
    }
    // Getter para o banco de dados
    const std::map<RE::FormID, AffectedNPC>& GetAffectedNPCsDatabase();

    Rule* GetRuleVersion(const std::string& ruleID, int version);
private:
    std::vector<Rule> _rules;
    std::unordered_map<std::string, std::size_t> _ruleIndices;
    std::map<std::string, std::vector<Rule>> _ruleHistories;
    std::map<RE::FormID, AffectedNPC> _affectedNPCsDatabase;
    bool _affectedNPCsDatabaseValid = false;
    std::map<std::string, std::string> _ruleOwners;
    // Logically deleted rules retain their version history so rewards already
    // applied in any save can be reconciled when that actor is next loaded.
    std::set<std::string> _deletedRuleIDs;
    std::set<std::string> _packagesToDelete;
    std::map<RuleDependencyMask, std::set<std::string>> _rulesByDependency;
    std::map<RuleDependencyMask, std::map<RE::FormID, std::set<std::string>>> _rulesByExactDependency;
    std::map<RE::ActorValue, std::set<std::string>> _rulesByActorValue;
    std::map<std::string, std::set<std::string>> _rulesBySourcePlugin;
    std::map<int, std::set<std::string>> _rulesByNPCTrait;
    std::map<int, std::set<std::string>> _rulesByCellType;
    std::map<int, std::set<std::string>> _rulesByEquippedCategory;
    std::set<std::string> _rulesByRelationship;
    std::map<RuleDependencyMask, std::set<std::string>> _rulesWithUnresolvedDependency;
    std::set<std::string> _broadFullEvaluationRules;
    std::map<std::string, RuleDependencyMask> _ruleDependencyMasks;
    std::set<std::string> _unstableCycleRules;
    std::map<RE::FormID, std::pair<std::uint64_t, std::uint64_t>> _baseNPCFingerprints;
    std::set<std::pair<RE::ActorValue, ActorValueMode>> _watchedActorValues;
    std::map<
        RE::FormID,
        std::pair<
            std::uint64_t,
            std::map<std::pair<RE::ActorValue, ActorValueMode>, float>>>
        _actorValueSnapshots;
    std::map<RE::FormID, std::pair<std::uint64_t, bool>>
        _followerStateSnapshots;
    std::uint64_t _dependencyRevision = 0;
    bool _hasActorDependentRules = false;
    bool _hasFollowerDependentRules = false;
    const std::string _modDir = "Data/Viny Mods/EDF";
    const std::string _exportDir = "Data/Viny Mods/EDF/Export/";
};

bool ParseLegacyRuleFile(
    const std::filesystem::path& path,
    Rule& latest,
    std::vector<Rule>& history,
    std::string& error);

bool ParseRuleDefinition(
    std::string_view json,
    Rule& rule,
    std::string& error);
std::string SerializeRuleDefinition(const Rule& rule);
bool ValidateRuleDefinition(const Rule& rule, std::string& error);

std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName);
RE::TESForm* ResolveEDFForm(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr);
RE::FormID ResolveEDFFormID(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr);
