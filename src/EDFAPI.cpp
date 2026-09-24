#include "EDFAPI.h"
#include "EDFActorRuleAPI.h"
#include <unordered_map>

#include "Rule.h"
#include "RulePackageStore.h"
#include "SaveState.h"
#include "logger.h"
#include "ClibUtil/editorID.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>

namespace EDF::API
{
    namespace
    {
        struct CallbackTarget
        {
            Callback callback = nullptr;
            void* userData = nullptr;
        };

        std::atomic_bool g_ready{ false };
        std::atomic_uint64_t actorRuleEpoch{0};

        std::string EditorID(RE::TESForm* form) noexcept
        {
            if (!form) return {};
            try {
                return clib_util::editorID::get_editorID(form);
            }
            catch (...) {
                return {};
            }
        }

        std::string PortableFormKey(RE::TESForm* form)
        {
            if (!form || form->GetFormID() == 0) return {};
            if (auto* file = form->GetFile(0)) {
                const std::string pluginName(file->GetFilename());
                return std::format(
                    "{}|{}",
                    pluginName,
                    FormatLocalFormID(form->GetFormID(), pluginName));
            }
            // Dynamic/created forms (for example DFG forms) may not have a TESFile.
            // Keep a runtime fallback while EditorID remains the primary identity.
            return std::format("Dynamic|{:08X}", form->GetFormID());
        }

        std::string ActorRuleDisplayName(RE::Actor* actor, const std::string_view fallbackKey)
        {
            if (!actor) return std::string(fallbackKey);
            auto* base = actor->GetActorBase();
            if (!base) return std::string(fallbackKey);

            const auto editorID = EditorID(base);
            const auto formKey = PortableFormKey(base);
            std::string name;
            if (!editorID.empty()) name = editorID;
            if (!formKey.empty()) {
                if (!name.empty()) name += " | ";
                name += formKey;
            }
            if (name.empty()) return std::string(fallbackKey);

            // Keep the requester's stable reference key when it adds information
            // beyond the base form. This prevents collisions between two refs
            // that share the same NPC base while keeping the visible EditorID + form.
            if (!fallbackKey.empty() && fallbackKey.find(formKey) == std::string_view::npos) {
                name += " | ";
                name += fallbackKey;
            }
            return name;
        }

        void CopyText(char* destination, const std::size_t capacity,
            const std::string_view value)
        {
            if (!destination || capacity == 0) return;
            const auto count = std::min(capacity - 1, value.size());
            std::memcpy(destination, value.data(), count);
            destination[count] = '\0';
        }

        void Complete(
            const CallbackTarget target,
            Result result,
            const std::string& json = {})
        {
            if (!target.callback) return;
            result.ruleJson = json.empty() ? nullptr : json.c_str();
            result.ruleJsonLength =
                static_cast<std::uint32_t>(json.size());
            try {
                target.callback(std::addressof(result), target.userData);
            }
            catch (...) {
                logger::error("[EDF API] Consumer callback threw an exception.");
            }
        }

        Result MakeResult(const Operation operation)
        {
            Result result;
            result.operation = operation;
            return result;
        }

        bool ValidateRequester(
            const std::string_view requester,
            std::string& error)
        {
            if (requester.empty() || requester.size() > 64) {
                error = "requester must contain 1-64 characters";
                return false;
            }
            if (std::ranges::any_of(requester, [](const unsigned char ch) {
                    return ch < 0x20 || ch == '/' || ch == '\\';
                })) {
                error = "requester contains invalid characters";
                return false;
            }
            return true;
        }

        std::string PackageDisplayName(const std::string_view requester)
        {
            return std::format("EDF API - {}", requester);
        }

        std::string PackageID(const std::string_view requester)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto ch : requester) {
                hash ^= static_cast<unsigned char>(ch);
                hash *= 1099511628211ULL;
            }
            return std::format("edf.api.{:016x}", hash);
        }

        std::optional<std::string> FindRequesterPackage(
            const std::string_view requester)
        {
            const auto displayName = PackageDisplayName(requester);
            const auto packageID = PackageID(requester);
            for (const auto& package :
                 RuleManager::GetSingleton()->GetPackages()) {
                if (package.id == packageID &&
                    package.displayName == displayName) {
                    return package.id;
                }
            }
            return std::nullopt;
        }

        std::optional<std::string> EnsureRequesterPackage(
            const std::string_view requester)
        {
            if (auto package = FindRequesterPackage(requester)) {
                return package;
            }
            return RuleManager::GetSingleton()->CreatePackage(
                PackageDisplayName(requester), PackageID(requester));
        }

        bool OwnsRule(
            const std::string_view requester,
            const Rule& rule)
        {
            const auto package = FindRequesterPackage(requester);
            return package && *package == rule.packageID;
        }

        bool Queue(std::function<void()> work)
        {
            auto* tasks = SKSE::GetTaskInterface();
            if (!g_ready.load(std::memory_order_acquire) || !tasks) {
                return false;
            }
            tasks->AddTask(std::move(work));
            return true;
        }

        class Service final : public IEDFRuleAPI
        {
        public:
            std::uint32_t GetVersion() const noexcept override
            {
                return kInterfaceVersion;
            }

            bool IsReady() const noexcept override
            {
                return g_ready.load(std::memory_order_acquire);
            }

            bool QueueCreateRule(
                const CreateRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(CreateRuleRequest) ||
                    !request->requester || !request->ruleJson || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string json(request->ruleJson);
                return Queue([requester, json,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kCreate);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    Rule definition;
                    if (!ParseRuleDefinition(json, definition, error)) {
                        result.status = error.starts_with("invalid rule JSON") ?
                            Status::kInvalidJson : Status::kValidationFailed;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const auto package = EnsureRequesterPackage(requester);
                    if (!package) {
                        result.status = Status::kPersistenceFailed;
                        CopyText(result.error, sizeof(result.error),
                            "could not create requester package");
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    auto& created = manager->CreateRule(*package);
                    const auto id = created.id;
                    const auto packageID = created.packageID;
                    created = std::move(definition);
                    created.id = id;
                    created.packageID = packageID;
                    created.version = 0;
                    created.lastSavedHash.clear();
                    if (!manager->SaveRule(id)) {
                        manager->DeleteRule(id);
                        result.status = Status::kPersistenceFailed;
                        CopyText(result.error, sizeof(result.error),
                            "could not persist rule");
                        Complete(target, result);
                        return;
                    }
                    const auto* saved = manager->FindRule(id);
                    result.status = Status::kSuccess;
                    result.version = saved ? saved->version : 1;
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    const auto output = saved ?
                        SerializeRuleDefinition(*saved) : std::string{};
                    ScheduleAllLoadedRuleEvaluations();
                    Complete(target, result, output);
                });
            }

            bool QueueUpdateRule(
                const UpdateRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(UpdateRuleRequest) ||
                    !request->requester || !request->ruleID ||
                    !request->ruleJson || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                const std::string json(request->ruleJson);
                const auto expected = request->expectedVersion;
                return Queue([requester, id, json, expected,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kUpdate);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    auto* current = manager->FindRule(id);
                    if (!current) {
                        result.status = Status::kNotFound;
                        Complete(target, result);
                        return;
                    }
                    if (!OwnsRule(requester, *current)) {
                        result.status = Status::kNotOwner;
                        Complete(target, result);
                        return;
                    }
                    if (expected != kAnyVersion &&
                        expected != current->version) {
                        result.status = Status::kVersionConflict;
                        result.version = current->version;
                        Complete(target, result);
                        return;
                    }
                    Rule replacement;
                    if (!ParseRuleDefinition(json, replacement, error)) {
                        result.status = error.starts_with("invalid rule JSON") ?
                            Status::kInvalidJson : Status::kValidationFailed;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const Rule backup = *current;
                    replacement.id = backup.id;
                    replacement.packageID = backup.packageID;
                    replacement.version = backup.version;
                    replacement.lastSavedHash.clear();
                    *current = std::move(replacement);
                    if (!manager->SaveRule(id)) {
                        if (auto* restore = manager->FindRule(id)) {
                            *restore = backup;
                            manager->RebuildDependencyIndex();
                        }
                        result.status = Status::kPersistenceFailed;
                        Complete(target, result);
                        return;
                    }
                    current = manager->FindRule(id);
                    result.status = Status::kSuccess;
                    result.version = current ? current->version : 0;
                    const auto output = current ?
                        SerializeRuleDefinition(*current) : std::string{};
                    ScheduleAllLoadedRuleEvaluations();
                    Complete(target, result, output);
                });
            }

            bool QueueDeleteRule(
                const DeleteRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(DeleteRuleRequest) ||
                    !request->requester || !request->ruleID || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                const auto expected = request->expectedVersion;
                return Queue([requester, id, expected,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kDelete);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    const auto* current = manager->FindRule(id);
                    if (!current) {
                        result.status = Status::kNotFound;
                    }
                    else if (!OwnsRule(requester, *current)) {
                        result.status = Status::kNotOwner;
                    }
                    else if (expected != kAnyVersion &&
                        expected != current->version) {
                        result.status = Status::kVersionConflict;
                        result.version = current->version;
                    }
                    else if (!manager->DeleteRule(id)) {
                        result.status = Status::kPersistenceFailed;
                    }
                    else {
                        result.status = Status::kSuccess;
                    }
                    Complete(target, result);
                });
            }

            bool QueueLookupRule(
                const LookupRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(LookupRuleRequest) ||
                    !request->requester || !request->ruleID || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                return Queue([requester, id,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kLookup);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const auto* rule =
                        RuleManager::GetSingleton()->FindRule(id);
                    if (!rule) {
                        result.status = Status::kNotFound;
                        Complete(target, result);
                        return;
                    }
                    result.status = Status::kSuccess;
                    result.version = rule->version;
                    const auto output = SerializeRuleDefinition(*rule);
                    Complete(target, result, output);
                });
            }

            bool QueueReevaluateActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                return QueueActor(
                    request, callback, userData, false);
            }

            bool QueueResetActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                return QueueActor(
                    request, callback, userData, true);
            }

        private:
            bool QueueActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData,
                const bool reset) noexcept
            {
                if (!request ||
                    request->structSize < sizeof(ActorRequest) ||
                    !request->requester || request->actorFormID == 0 ||
                    !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string ruleID =
                    request->ruleID ? request->ruleID : "";
                const auto actorID = request->actorFormID;
                const auto epoch = actorRuleEpoch.load();
                return Queue([requester, ruleID, actorID, reset, epoch,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(reset ?
                        Operation::kResetActor :
                        Operation::kReevaluateActor);
                    result.actorFormID = actorID;
                    if (epoch != actorRuleEpoch.load()) {
                        result.status = Status::kNotReady;
                        Complete(target, result);
                        return;
                    }
                    CopyText(result.ruleID, sizeof(result.ruleID), ruleID);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* actor =
                        RE::TESForm::LookupByID<RE::Actor>(actorID);
                    if (!actor || actor->IsDead() ||
                        !actor->GetParentCell()) {
                        result.status = Status::kActorUnavailable;
                        Complete(target, result);
                        return;
                    }
                    if (reset) {
                        result.status = ResetRuleActivationForActor(
                            actor, ruleID) ? Status::kSuccess :
                            Status::kNotFound;
                    }
                    else {
                        const auto startedAt = std::chrono::steady_clock::now();

                        ApplyRulesToInstance(
                            actor, RuleEvaluationDelta::Full());

                        // If the caller supplied a ruleID, verify EDF's own
                        // activation ledger instead of asking Actor::HasPerk.
                        // This is especially important for runtime/distributed
                        // perks whose effective ownership may not be reflected
                        // by the console or by the caller's HasPerk probe.
                        if (!ruleID.empty()) {
                            const auto* rule =
                                RuleManager::GetSingleton()->FindRule(ruleID);
                            if (!rule) {
                                result.status = Status::kNotFound;
                                CopyText(
                                    result.error,
                                    sizeof(result.error),
                                    std::format(
                                        "rule '{}' was not found after reevaluation",
                                        ruleID));
                            } else {
                                auto* saves = SaveStateManager::GetSingleton();
                                const auto npcKey =
                                    SaveStateManager::BuildNPCKey(actor);
                                auto& session = saves->GetSessionData();
                                const auto npcIt =
                                    session.npcRuleVersions.find(npcKey);

                                const AppliedRuleState* state = nullptr;
                                if (npcIt != session.npcRuleVersions.end()) {
                                    const auto stateIt =
                                        npcIt->second.find(ruleID);
                                    if (stateIt != npcIt->second.end()) {
                                        state = std::addressof(stateIt->second);
                                    }
                                }

                                std::size_t perkRewards = 0;
                                for (const auto& group : rule->rewardGroups) {
                                    perkRewards += std::ranges::count_if(
                                        group.rewards,
                                        [](const Reward& reward) {
                                            return reward.typeReward == "Perk";
                                        });
                                }

                                const bool active =
                                    state &&
                                    state->activationStateKnown &&
                                    state->isActive;
                                const bool currentVersion =
                                    state && state->version == rule->version;
                                const bool rewardsSelected =
                                    state &&
                                    state->activeRewardKeys.size() >=
                                        perkRewards;

                                if (!active ||
                                    !currentVersion ||
                                    !rewardsSelected) {
                                    result.status =
                                        Status::kValidationFailed;
                                    CopyText(
                                        result.error,
                                        sizeof(result.error),
                                        std::format(
                                            "rule activation verification failed: active={} stateVersion={} ruleVersion={} activeRewards={} expectedPerkRewards={}",
                                            active,
                                            state ? state->version : -1,
                                            rule->version,
                                            state ?
                                                state->activeRewardKeys.size() :
                                                0,
                                            perkRewards));
                                } else {
                                    result.status = Status::kSuccess;
                                }

                                const auto elapsedMs =
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() -
                                            startedAt)
                                        .count();
                                logger::info(
                                    "[EDF ActorRule] reevaluate actor={:08X} rule='{}' status={} active={} stateVersion={} ruleVersion={} activeRewards={} perkRewards={} elapsedMs={}",
                                    actorID,
                                    ruleID,
                                    static_cast<std::uint32_t>(
                                        result.status),
                                    active,
                                    state ? state->version : -1,
                                    rule->version,
                                    state ?
                                        state->activeRewardKeys.size() : 0,
                                    perkRewards,
                                    elapsedMs);
                            }
                        } else {
                            result.status = Status::kSuccess;
                        }
                    }
                    Complete(target, result);
                });
            }
        };
    }

    void SetReady(const bool ready) noexcept
    {
        g_ready.store(ready, std::memory_order_release);
    }

    IEDFRuleAPI* GetService() noexcept
    {
        static Service service;
        return std::addressof(service);
    }
}

extern "C" __declspec(dllexport) void* GetEDFRuleAPI()
{
    return EDF::API::GetService();
}


namespace EDF::API
{
    namespace
    {
        std::unordered_map<std::string, RE::FormID> actorRuleTargets;
        constexpr std::string_view actorPackagePrefix = "edf.actor-api.";

        bool SyncActorRule(const ActorRules::Request* request,
            Callback callback, void* userData) noexcept
        {
            if (!request || request->structSize < sizeof(*request) ||
                !request->requester || !request->actorKey ||
                !request->actorFormID || request->perkCount > 100000 ||
                (request->perkCount && !request->perks) || !callback) return false;
            const std::string requester(request->requester), key(request->actorKey);
            const auto actorID = request->actorFormID;
            std::vector<std::uint32_t> perks;
            if (request->perkCount) perks.assign(request->perks, request->perks + request->perkCount);
            const auto epoch = actorRuleEpoch.load();
            return Queue([requester, key, actorID, perks = std::move(perks), epoch,
                          target = CallbackTarget{callback, userData}] {
                auto result = MakeResult(Operation::kUpdate);
                result.actorFormID = actorID;
                if (epoch != actorRuleEpoch.load()) {
                    result.status = Status::kNotReady;
                    Complete(target, result);
                    return;
                }
                std::string error;
                if (!ValidateRequester(requester, error) || key.empty() || key.size() > 1024) {
                    result.status = Status::kInvalidArgument;
                    Complete(target, result);
                    return;
                }
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorID);
                auto* actorBase = actor ? actor->GetActorBase() : nullptr;
                const auto ruleName = ActorRuleDisplayName(actor, key);

                Rule definition;
                definition.name = ruleName;
                definition.actorScope = RuleActorScope::kNPCOnly;
                definition.isEnabled = true;
                if (actorBase) {
                    BlacklistFilter actorFilter;
                    actorFilter.type = "NPC";
                    actorFilter.editorID = EditorID(actorBase);
                    actorFilter.formIDStr = PortableFormKey(actorBase);
                    if (!actorFilter.editorID.empty() || !actorFilter.formIDStr.empty()) {
                        definition.targetFilters.push_back(std::move(actorFilter));
                    }
                }
                RewardGroup group;
                group.name = "Purchased perks";
                for (auto id : perks) {
                    auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(id);
                    if (!perk) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error),
                            std::format("perk {:08X} is not loaded", id));
                        Complete(target, result);
                        return;
                    }
                    Reward reward;
                    reward.typeReward = "Perk";
                    reward.editorID = EditorID(perk);
                    reward.formIDStr = PortableFormKey(perk);
                    if (reward.editorID.empty() && reward.formIDStr.empty()) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error),
                            std::format("perk {:08X} has no resolvable identity", id));
                        Complete(target, result);
                        return;
                    }
                    reward.amount = 1;
                    reward.chanceReward = 100.0f;
                    reward.isPersistent = false;
                    group.rewards.push_back(std::move(reward));
                }
                definition.rewardGroups.push_back(std::move(group));
                auto* manager = RuleManager::GetSingleton();
                const auto packageID = std::string(actorPackagePrefix) + PackageID(requester);
                bool exists = false;
                for (const auto& package : manager->GetPackages()) exists |= package.id == packageID;
                if (!exists && !manager->CreatePackage(requester, packageID)) {
                    result.status = Status::kPersistenceFailed;
                    CopyText(result.error, sizeof(result.error),
                        std::format("could not create actor package '{}'", requester));
                    Complete(target, result);
                    return;
                }
                if (exists && !manager->RenamePackage(packageID, requester)) {
                    result.status = Status::kPersistenceFailed;
                    CopyText(result.error, sizeof(result.error),
                        std::format("could not rename actor package to '{}'", requester));
                    Complete(target, result);
                    return;
                }
                Rule* current = nullptr;
                for (auto& rule : manager->GetRules()) {
                    if (rule.packageID == packageID &&
                        (rule.name == ruleName || rule.name == key)) {
                        current = &rule;
                        break;
                    }
                }
                if (!current) current = &manager->CreateRule(packageID);
                const Rule backup = *current;
                definition.id = backup.id;
                definition.packageID = packageID;
                definition.version = backup.version;
                if (backup.version > 0 && backup.CalculateHash() == definition.CalculateHash()) {
                    actorRuleTargets[backup.id] = actorID;
                    result.status = Status::kSuccess;
                    CopyText(result.ruleID, sizeof(result.ruleID), backup.id);
                    Complete(target, result);
                    return;
                }
                *current = std::move(definition);
                const auto id = current->id;
                CopyText(result.ruleID, sizeof(result.ruleID), id);

                const auto syncStartedAt =
                    std::chrono::steady_clock::now();

                // Make the exact-reference binding visible before SaveRule()
                // rebuilds EDF's runtime indexes.
                const auto previousTarget =
                    actorRuleTargets.find(id);
                const bool hadPreviousTarget =
                    previousTarget != actorRuleTargets.end();
                const auto previousActorID =
                    hadPreviousTarget ? previousTarget->second : 0;
                actorRuleTargets[id] = actorID;

                if (!manager->SaveRule(id)) {
                    if (auto* restore = manager->FindRule(id)) {
                        *restore = backup;
                    }
                    if (hadPreviousTarget) {
                        actorRuleTargets[id] = previousActorID;
                    } else {
                        actorRuleTargets.erase(id);
                    }
                    manager->RebuildDependencyIndex();
                    result.status = Status::kPersistenceFailed;
                    CopyText(
                        result.error,
                        sizeof(result.error),
                        std::format(
                            "could not save actor rule '{}'",
                            current->name));
                } else {
                    // Verify what is actually present in the rule after
                    // SaveRule(). A successful callback now guarantees that
                    // every requested perk is represented as a reward.
                    const auto* saved =
                        manager->FindRule(id);
                    std::set<RE::FormID> savedPerks;

                    if (saved) {
                        for (const auto& savedGroup :
                             saved->rewardGroups) {
                            for (const auto& savedReward :
                                 savedGroup.rewards) {
                                if (savedReward.typeReward != "Perk") {
                                    continue;
                                }

                                const auto savedID =
                                    ResolveEDFFormID(
                                        "Perk",
                                        savedReward.editorID,
                                        savedReward.formIDStr);
                                if (savedID) {
                                    savedPerks.insert(savedID);
                                }

                                logger::info(
                                    "[EDF ActorRule] saved reward rule='{}' editorID='{}' form='{}' resolved={:08X}",
                                    id,
                                    savedReward.editorID,
                                    savedReward.formIDStr,
                                    savedID);
                            }
                        }
                    }

                    const std::set<RE::FormID> requestedPerks(
                        perks.begin(),
                        perks.end());
                    const bool rewardsComplete =
                        saved &&
                        requestedPerks == savedPerks;

                    const auto elapsedMs =
                        std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() -
                                syncStartedAt)
                            .count();

                    if (!rewardsComplete) {
                        result.status = Status::kPersistenceFailed;
                        CopyText(
                            result.error,
                            sizeof(result.error),
                            std::format(
                                "saved actor rule reward mismatch: requested={} saved={}",
                                requestedPerks.size(),
                                savedPerks.size()));
                        logger::error(
                            "[EDF ActorRule] rule save verification FAILED actor={:08X} rule='{}' requestedPerks={} savedPerks={} elapsedMs={}",
                            actorID,
                            id,
                            requestedPerks.size(),
                            savedPerks.size(),
                            elapsedMs);
                    } else {
                        result.status = Status::kSuccess;
                        logger::info(
                            "[EDF ActorRule] rule save verified actor={:08X} rule='{}' version={} requestedPerks={} savedPerks={} elapsedMs={}",
                            actorID,
                            id,
                            saved ? saved->version : 0,
                            requestedPerks.size(),
                            savedPerks.size(),
                            elapsedMs);
                    }
                }
                Complete(target, result);
            });
        }
    }

    void ClearActorRuleSession()
    {
        ++actorRuleEpoch;
        actorRuleTargets.clear();
    }

    bool MatchesActorRuleSession(const Rule& rule, RE::Actor* actor)
    {
        if (!rule.packageID.starts_with(actorPackagePrefix)) return true;
        const auto found = actorRuleTargets.find(rule.id);
        return actor && found != actorRuleTargets.end() && found->second == actor->GetFormID();
    }
}

extern "C" __declspec(dllexport) EDF::ActorRules::Interface* GetEDFActorRuleAPI()
{
    static EDF::ActorRules::Interface api{1, EDF::API::SyncActorRule};
    return &api;
}
