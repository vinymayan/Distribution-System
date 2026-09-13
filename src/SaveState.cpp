#include "SaveState.h"
#include "DistributionCore/Domain.h"
#include "DistributionCore/RewardSelector.h"
#include "DelayedDispatcher.h"
#include "WhoEditThatAPI.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// Conjunto estático para rastrear atores em processamento
static std::mutex g_processingMutex;
static std::set<RE::FormID> g_actorsInProcess;

namespace {
    std::mutex g_scheduledEvaluationsMutex;
    struct PendingRuleEvaluation {
        std::uint64_t token = 0;
        RE::ActorHandle actorHandle;
        RuleEvaluationDelta delta;
    };
    std::map<RE::FormID, PendingRuleEvaluation> g_scheduledEvaluations;
    std::atomic_uint64_t g_nextEvaluationToken{ 1 };
    std::atomic_uint64_t g_nextRuleActivationToken{ 1 };
    bool g_ruleEvaluationPumpScheduled = false;
    std::uint64_t g_ruleEvaluationPumpGeneration = 1;
    std::atomic_bool g_ruleEvaluationSuspended{ false };
    std::atomic_uint64_t g_followerRefreshToken{ 0 };
    void ProcessScheduledRuleEvaluations(std::uint64_t a_generation);
    thread_local RE::FormID g_activeEvaluationActor = 0;
    thread_local RuleEvaluationDelta* g_activeProducedDelta = nullptr;

    struct InventoryInteractionState {
        bool active = false;
        std::uint64_t token = 0;
        std::map<RE::FormID, RE::ActorHandle> actors;
        std::map<RE::FormID, RuleEvaluationDelta> pendingDeltas;
    };
    std::mutex g_inventoryInteractionMutex;
    InventoryInteractionState g_inventoryInteraction;
    std::atomic_uint64_t g_nextInventoryInteractionToken{ 1 };

    RuleEvaluationDelta MakeEmptyRuleEvaluationDelta()
    {
        RuleEvaluationDelta delta;
        delta.mask = ToMask(RuleDependency::kNone);
        return delta;
    }

    bool DeferInventoryInteractionEvaluation(
        RE::Actor* a_actor,
        const RuleEvaluationDelta& a_delta)
    {
        if (!a_actor) {
            return false;
        }

        const auto actorID = a_actor->GetFormID();
        std::lock_guard lock(g_inventoryInteractionMutex);
        if (!g_inventoryInteraction.active ||
            !g_inventoryInteraction.actors.contains(actorID)) {
            return false;
        }

        auto pendingIt =
            g_inventoryInteraction.pendingDeltas.try_emplace(
                actorID,
                MakeEmptyRuleEvaluationDelta()).first;
        pendingIt->second.Merge(a_delta);
        return true;
    }

    struct ActiveEquipmentReconciliation {
        RE::FormID actorID = 0;
        RuleEvaluationDelta delta;
    };
    thread_local ActiveEquipmentReconciliation* g_activeEquipmentReconciliation = nullptr;

    struct PendingSleepUpdate {
        std::uint64_t token = 0;
        bool isEntering = false;
    };

    std::mutex g_sleepUpdatesMutex;
    std::map<RE::FormID, PendingSleepUpdate> g_sleepUpdates;
    std::atomic_uint64_t g_nextSleepToken{ 1 };
    std::map<RE::FormID, bool> g_sleepStates;
    std::mutex g_combatStatesMutex;
    std::map<RE::FormID, RE::ACTOR_COMBAT_STATE> g_combatStates;

    constexpr char kRewardStateDelimiter = '\x1E';
    constexpr std::string_view kLedgerPrefix = "edf/r/";

    std::uint64_t StableHash(std::string_view value)
    {
        std::uint64_t result = 14695981039346656037ULL;
        for (const auto character : value) {
            result ^= static_cast<std::uint8_t>(character);
            result *= 1099511628211ULL;
        }
        return result;
    }

    std::string BuildLedgerRulePrefix(std::string_view ruleID)
    {
        return std::format(
            "{}{:016X}/", kLedgerPrefix, StableHash(ruleID));
    }

    std::string BuildLedgerMutationKey(
        std::string_view ruleID,
        std::string_view rewardKey)
    {
        return std::format(
            "{}{:016X}", BuildLedgerRulePrefix(ruleID),
            StableHash(rewardKey));
    }

    WhoEditThat::API::IWhoEditThatAPI* GetActorValueLedger()
    {
        static auto* api = WhoEditThat::API::GetAPI();
        static WhoEditThat::API::ClientHandle client =
            WhoEditThat::API::kInvalidClient;
        if (!api) {
            api = WhoEditThat::API::GetAPI();
        }
        if (!api || !api->IsReady()) {
            return nullptr;
        }
        if (client == WhoEditThat::API::kInvalidClient) {
            WhoEditThat::API::ClientRegistration registration;
            registration.clientID = "viny.edf";
            registration.displayName = "EDF";
            client = api->RegisterClient(std::addressof(registration));
        }
        return client == WhoEditThat::API::kInvalidClient ? nullptr : api;
    }

    WhoEditThat::API::ClientHandle GetActorValueLedgerClient()
    {
        auto* api = GetActorValueLedger();
        if (!api) {
            return WhoEditThat::API::kInvalidClient;
        }
        WhoEditThat::API::ClientRegistration registration;
        registration.clientID = "viny.edf";
        registration.displayName = "EDF";
        return api->RegisterClient(std::addressof(registration));
    }

    void LogLedgerResult(
        const WhoEditThat::API::Result* result,
        void*)
    {
        if (!result) {
            return;
        }
        if (result->status == WhoEditThat::API::Status::kSuccess ||
            result->status == WhoEditThat::API::Status::kNotFound) {
            logger::debug(
                "[ActorValueLedger] operation={} actor={:08X} key='{}' "
                "delta={:+.3f} affected={} status={}",
                std::to_underlying(result->operation),
                result->actorFormID,
                result->mutationKey,
                result->appliedDelta,
                result->affectedCount,
                std::to_underlying(result->status));
            return;
        }
        logger::warn(
            "[ActorValueLedger] operation={} actor={:08X} status={} message='{}'",
            std::to_underlying(result->operation),
            result->actorFormID,
            std::to_underlying(result->status),
            result->message);
    }

    std::string BuildRewardStateKey(
        const std::string& a_groupName,
        const Reward& a_reward,
        const bool a_includeEquipContexts = true)
    {
        std::string key;
        const auto append = [&](std::string_view a_value) {
            if (!key.empty()) {
                key.push_back(kRewardStateDelimiter);
            }
            key.append(a_value);
        };

        append(a_groupName);
        append(a_reward.typeReward);
        append(a_reward.formIDStr);
        append(a_reward.editorID);
        append(std::to_string(a_reward.amount));
        append(std::to_string(a_reward.functionOnType));
        if (a_includeEquipContexts) {
            append(std::to_string(a_reward.equipContexts));
        }
        append(a_reward.isPersistent ? "1" : "0");
        if (a_reward.typeReward == "Actor Value" ||
            a_reward.typeReward == "Actor Scale") {
            append(a_reward.actorValueName);
            append(std::format(
                "{:.9g}", a_reward.actorValueAmount));
            const bool hasDynamicNumericPayload =
                a_reward.numericOperation != NumericRewardOperation::kFlat ||
                a_reward.numericSource != NumericRewardSource::kFixed ||
                a_reward.numericBinding != NumericRewardBinding::kSnapshot ||
                a_reward.percentBaseMode != ActorValueMode::kPermanent ||
                !a_reward.sourceActorValueName.empty() ||
                a_reward.sourceActorValueMode != ActorValueMode::kCurrent ||
                !a_reward.sourceGlobalFormID.empty() ||
                !a_reward.sourceGlobalEditorID.empty() ||
                a_reward.sourceMultiplier != 1.0f;
            if (hasDynamicNumericPayload) {
                append(std::to_string(
                    static_cast<int>(a_reward.numericOperation)));
                append(std::to_string(
                    static_cast<int>(a_reward.numericSource)));
                append(std::to_string(
                    static_cast<int>(a_reward.numericBinding)));
                append(std::to_string(
                    static_cast<int>(a_reward.percentBaseMode)));
                append(a_reward.sourceActorValueName);
                append(std::to_string(
                    static_cast<int>(a_reward.sourceActorValueMode)));
                append(a_reward.sourceGlobalFormID);
                append(a_reward.sourceGlobalEditorID);
                append(std::format(
                    "{:.9g}", a_reward.sourceMultiplier));
            }
        }
        return key;
    }

    bool QueueActorValueReward(
        RE::Actor* actor,
        const Reward& reward,
        std::string_view ruleID,
        std::string_view rewardKey)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return false;
        }

        const auto mutationKey = BuildLedgerMutationKey(ruleID, rewardKey);
        const auto targetValue = ResolveActorValue(reward.actorValueName);
        const auto* targetName =
            targetValue != RE::ActorValue::kNone ?
                RE::ActorValueList::GetActorValueName(targetValue) :
                nullptr;
        if (!targetName || !*targetName) {
            logger::error(
                "[WhoEditThat] Cannot queue Actor Value reward '{}': "
                "target '{}' is invalid",
                mutationKey,
                reward.actorValueName);
            return false;
        }

        std::string sourceActorValueName;
        if (reward.numericSource == NumericRewardSource::kActorValue) {
            const auto sourceValue = ResolveActorValue(
                reward.sourceActorValueName);
            const auto* sourceName =
                sourceValue != RE::ActorValue::kNone ?
                    RE::ActorValueList::GetActorValueName(sourceValue) :
                    nullptr;
            if (!sourceName || !*sourceName) {
                logger::error(
                    "[WhoEditThat] Cannot queue Actor Value reward '{}': "
                    "source '{}' is invalid",
                    mutationKey,
                    reward.sourceActorValueName);
                return false;
            }
            sourceActorValueName = sourceName;
        }

        WhoEditThat::API::ActorValueContributionRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKey = mutationKey.c_str();
        request.targetActorValue = targetName;
        request.operation = static_cast<WhoEditThat::API::NumericOperation>(
            reward.numericOperation);
        request.source = static_cast<WhoEditThat::API::NumericSource>(
            reward.numericSource);
        request.channel = reward.isPersistent ?
            WhoEditThat::API::ModifierChannel::kPermanent :
            WhoEditThat::API::ModifierChannel::kTemporary;
        request.fixedValue = reward.actorValueAmount;
        request.sourceActorValue = sourceActorValueName.c_str();
        request.sourceMultiplier = reward.sourceMultiplier;
        if (reward.numericSource == NumericRewardSource::kGlobal) {
            const auto* global = ResolveEDFForm(
                "Global",
                reward.sourceGlobalEditorID,
                reward.sourceGlobalFormID);
            if (!global || !global->As<RE::TESGlobal>()) {
                return false;
            }
            request.sourceGlobalFormID = global->GetFormID();
        }
        return api->QueueUpsertActorValue(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    bool QueueActorScaleReward(
        RE::Actor* actor,
        const Reward& reward,
        std::string_view ruleID,
        std::string_view rewardKey)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient ||
            !IsActorScaleRewardValid(reward)) {
            return false;
        }

        const auto mutationKey = BuildLedgerMutationKey(ruleID, rewardKey);
        std::string sourceActorValueName;
        if (reward.numericSource == NumericRewardSource::kActorValue) {
            const auto sourceValue = ResolveActorValue(
                reward.sourceActorValueName);
            const auto* sourceName = sourceValue != RE::ActorValue::kNone ?
                RE::ActorValueList::GetActorValueName(sourceValue) : nullptr;
            if (!sourceName || !*sourceName) {
                return false;
            }
            sourceActorValueName = sourceName;
        }

        WhoEditThat::API::ActorScaleContributionRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKey = mutationKey.c_str();
        request.operation = static_cast<WhoEditThat::API::NumericOperation>(
            reward.numericOperation);
        request.source = static_cast<WhoEditThat::API::NumericSource>(
            reward.numericSource);
        request.fixedValue = reward.actorValueAmount;
        request.sourceActorValue = sourceActorValueName.c_str();
        request.sourceMultiplier = reward.sourceMultiplier;
        if (reward.numericSource == NumericRewardSource::kGlobal) {
            const auto* global = ResolveEDFForm(
                "Global",
                reward.sourceGlobalEditorID,
                reward.sourceGlobalFormID);
            if (!global || !global->As<RE::TESGlobal>()) {
                return false;
            }
            request.sourceGlobalFormID = global->GetFormID();
        }
        return api->QueueUpsertActorScale(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    bool QueueRemoveActorValueReward(
        RE::Actor* actor,
        std::string_view ruleID,
        std::string_view rewardKey)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return false;
        }
        const auto mutationKey = BuildLedgerMutationKey(ruleID, rewardKey);
        WhoEditThat::API::ContributionRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKey = mutationKey.c_str();
        return api->QueueRemoveActorValue(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    bool QueueRemoveActorScaleReward(
        RE::Actor* actor,
        std::string_view ruleID,
        std::string_view rewardKey)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return false;
        }
        const auto mutationKey = BuildLedgerMutationKey(ruleID, rewardKey);
        WhoEditThat::API::ContributionRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKey = mutationKey.c_str();
        return api->QueueRemoveActorScale(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    bool QueueRemoveActorValueRule(
        RE::Actor* actor,
        std::string_view ruleID)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return false;
        }
        const auto prefix = BuildLedgerRulePrefix(ruleID);
        WhoEditThat::API::ContributionScopeRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKeyPrefix = prefix.c_str();
        return api->QueueRemoveActorValuesByPrefix(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    bool QueueRemoveActorScaleRule(
        RE::Actor* actor,
        std::string_view ruleID)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return false;
        }
        const auto prefix = BuildLedgerRulePrefix(ruleID);
        WhoEditThat::API::ContributionScopeRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKeyPrefix = prefix.c_str();
        return api->QueueRemoveActorScalesByPrefix(
            std::addressof(request), LogLedgerResult, nullptr);
    }

    struct LedgerReconcileContext
    {
        std::set<std::string> desired;
    };

    void ReconcileActorValueLedgerCallback(
        const WhoEditThat::API::ActorValueListResult* result,
        void* userData)
    {
        std::unique_ptr<LedgerReconcileContext> context(
            static_cast<LedgerReconcileContext*>(userData));
        if (!result || result->status != WhoEditThat::API::Status::kSuccess) {
            return;
        }
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!api || client == WhoEditThat::API::kInvalidClient) {
            return;
        }
        for (std::uint32_t index = 0; index < result->entryCount; ++index) {
            const auto& entry = result->entries[index];
            if (context->desired.contains(entry.mutationKey)) {
                continue;
            }
            WhoEditThat::API::ContributionRequest request;
            request.client = client;
            request.actorFormID = result->actorFormID;
            request.mutationKey = entry.mutationKey;
            api->QueueRemoveActorValue(
                std::addressof(request), LogLedgerResult, nullptr);
        }
    }

    void ReconcileActorScaleLedgerCallback(
        const WhoEditThat::API::ActorScaleListResult* result,
        void* userData)
    {
        std::unique_ptr<LedgerReconcileContext> context(
            static_cast<LedgerReconcileContext*>(userData));
        if (!result || result->status != WhoEditThat::API::Status::kSuccess) {
            return;
        }
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!api || client == WhoEditThat::API::kInvalidClient) {
            return;
        }
        for (std::uint32_t index = 0; index < result->entryCount; ++index) {
            const auto& entry = result->entries[index];
            if (context->desired.contains(entry.mutationKey)) {
                continue;
            }
            WhoEditThat::API::ContributionRequest request;
            request.client = client;
            request.actorFormID = result->actorFormID;
            request.mutationKey = entry.mutationKey;
            api->QueueRemoveActorScale(
                std::addressof(request), LogLedgerResult, nullptr);
        }
    }

    void ReconcileActorValueLedger(
        RE::Actor* actor,
        const std::map<std::string, AppliedRuleState>& rules)
    {
        auto* api = GetActorValueLedger();
        const auto client = GetActorValueLedgerClient();
        if (!actor || !api || client == WhoEditThat::API::kInvalidClient) {
            return;
        }
        std::set<std::string> desired;
        for (const auto& [ruleID, state] : rules) {
            if (state.isActive) {
                for (const auto& rewardKey : state.activeRewardKeys) {
                    desired.insert(
                        BuildLedgerMutationKey(ruleID, rewardKey));
                }
            }
            for (const auto& rewardKey : state.persistentRewardKeys) {
                desired.insert(
                    BuildLedgerMutationKey(ruleID, rewardKey));
            }
        }
        auto context = std::make_unique<LedgerReconcileContext>();
        context->desired = desired;
        WhoEditThat::API::ContributionScopeRequest request;
        request.client = client;
        request.actorFormID = actor->GetFormID();
        request.mutationKeyPrefix = kLedgerPrefix.data();
        if (api->QueueListActorValues(
                std::addressof(request),
                ReconcileActorValueLedgerCallback,
                context.get())) {
            context.release();
        }
        auto scaleContext = std::make_unique<LedgerReconcileContext>();
        scaleContext->desired = std::move(desired);
        if (api->QueueListActorScales(
                std::addressof(request),
                ReconcileActorScaleLedgerCallback,
                scaleContext.get())) {
            scaleContext.release();
        }
    }

    float ReadActorValue(
        RE::Actor* a_actor,
        const RE::ActorValue a_actorValue,
        const ActorValueMode a_mode)
    {
        auto* owner = a_actor ? a_actor->AsActorValueOwner() : nullptr;
        if (!owner || a_actorValue == RE::ActorValue::kNone) {
            return 0.0f;
        }
        switch (a_mode) {
        case ActorValueMode::kPermanent:
            return owner->GetPermanentActorValue(a_actorValue);
        case ActorValueMode::kMaximum:
            return IsMaximumActorValueSupported(a_actorValue) ?
                a_actor->GetActorValueMax(a_actorValue) :
                0.0f;
        case ActorValueMode::kCurrent:
        default:
            return owner->GetActorValue(a_actorValue);
        }
    }

    bool ApplyOrReconcileNumericReward(
        RE::Actor* a_actor,
        const Reward& a_reward,
        const std::string& a_rewardKey,
        std::string_view a_ruleID,
        AppliedRuleState& a_state)
    {
        if (!a_actor || !IsActorValueRewardValid(a_reward) ||
            !IsActorScaleRewardValid(a_reward)) {
            return false;
        }
        if (a_reward.typeReward == "Actor Scale") {
            return QueueActorScaleReward(
                a_actor, a_reward, a_ruleID, a_rewardKey);
        }
        // One-time migration: old EDF saves own this delta directly. Remove
        // it before handing the same reward to WhoEditThat.
        if (const auto existing =
                a_state.numericRewardStates.find(a_rewardKey);
            existing != a_state.numericRewardStates.end()) {
            const auto target = ResolveActorValue(
                existing->second.targetActorValueName.empty() ?
                    a_reward.actorValueName :
                    existing->second.targetActorValueName);
            if (target != RE::ActorValue::kNone) {
                a_actor->AsActorValueOwner()->ModActorValue(
                    RE::ACTOR_VALUE_MODIFIERS::kPermanent,
                    target,
                    -existing->second.appliedDelta);
            }
            a_state.numericRewardStates.erase(existing);
        }
        return QueueActorValueReward(
            a_actor, a_reward, a_ruleID, a_rewardKey);
    }

    bool RemoveNumericReward(
        RE::Actor* a_actor,
        const Reward& a_reward,
        const std::string& a_rewardKey,
        std::string_view a_ruleID,
        AppliedRuleState& a_state)
    {
        if (!a_actor) {
            return false;
        }
        if (a_reward.typeReward == "Actor Scale") {
            return QueueRemoveActorScaleReward(
                a_actor, a_ruleID, a_rewardKey);
        }
        const auto found = a_state.numericRewardStates.find(a_rewardKey);
        bool removedLegacy = false;
        if (found != a_state.numericRewardStates.end()) {
            const auto target = ResolveActorValue(
                found->second.targetActorValueName.empty() ?
                    a_reward.actorValueName :
                    found->second.targetActorValueName);
            if (target != RE::ActorValue::kNone) {
                a_actor->AsActorValueOwner()->ModActorValue(
                    RE::ACTOR_VALUE_MODIFIERS::kPermanent,
                    target,
                    -found->second.appliedDelta);
                removedLegacy = true;
            }
            a_state.numericRewardStates.erase(found);
        }
        return QueueRemoveActorValueReward(
                   a_actor, a_ruleID, a_rewardKey) ||
            removedLegacy;
    }

    std::string BuildRewardOwnerKey(
        std::string_view a_ruleID,
        const std::string& a_groupName,
        const Reward& a_reward)
    {
        return std::string(a_ruleID) + kRewardStateDelimiter +
            BuildRewardStateKey(a_groupName, a_reward);
    }

    std::string BuildLeveledRewardLedgerGroup(
        const std::string& a_groupName,
        const Reward& a_reward)
    {
        return BuildRewardStateKey(a_groupName, a_reward);
    }

    bool ResolveRewardState(
        const Rule& a_rule,
        const std::string& a_rewardKey,
        const RewardGroup*& a_group,
        const Reward*& a_reward)
    {
        for (const auto& group : a_rule.rewardGroups) {
            for (const auto& reward : group.rewards) {
                if (BuildRewardStateKey(group.name, reward) == a_rewardKey ||
                    BuildRewardStateKey(group.name, reward, false) == a_rewardKey) {
                    a_group = std::addressof(group);
                    a_reward = std::addressof(reward);
                    return true;
                }
            }
        }

        a_group = nullptr;
        a_reward = nullptr;
        return false;
    }

    bool ContainsRewardStateKey(
        const std::set<std::string>& a_keys,
        const std::string& a_groupName,
        const Reward& a_reward)
    {
        return a_keys.contains(BuildRewardStateKey(a_groupName, a_reward)) ||
            a_keys.contains(BuildRewardStateKey(a_groupName, a_reward, false));
    }

    const RE::TESFile* GetSourceFileByFormID(RE::TESForm* a_form)
    {
        if (!a_form) return nullptr;
        if (auto file = a_form->GetFile(0)) return file;

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        const auto formID = a_form->GetFormID();
        const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
        if (modIndex == 0xFE) {
            const auto lightIndex = static_cast<std::uint16_t>((formID >> 12) & 0xFFF);
            return dataHandler->LookupLoadedLightModByIndex(lightIndex);
        }
        if (modIndex != 0xFF) {
            return dataHandler->LookupLoadedModByIndex(modIndex);
        }
        return nullptr;
    }

    const rapidjson::Value* FindMember(const rapidjson::Value& obj, const char* key) {
        if (!obj.IsObject()) return nullptr;
        auto it = obj.FindMember(key);
        return it != obj.MemberEnd() ? &it->value : nullptr;
    }

    uint32_t GetJsonUint(const rapidjson::Value& value, uint32_t fallback = 0) {
        if (value.IsUint()) return value.GetUint();
        if (value.IsInt() && value.GetInt() >= 0) return static_cast<uint32_t>(value.GetInt());
        return fallback;
    }

    std::vector<std::string> ReadStringArray(const rapidjson::Value* value) {
        std::vector<std::string> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsString()) result.emplace_back(item.GetString());
        }
        return result;
    }

    rapidjson::Value WriteStringArray(const std::vector<std::string>& values, rapidjson::Document::AllocatorType& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& value : values) {
            rapidjson::Value jsonValue;
            jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
            array.PushBack(jsonValue, alloc);
        }
        return array;
    }

    std::string SerializeJson(const rapidjson::Value& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

    constexpr char kManagedItemKeyDelimiter = '\x1F';

    std::string GetManagedItemBaseKey(const std::string& a_managedKey)
    {
        const auto delimiter = a_managedKey.find(kManagedItemKeyDelimiter);
        return delimiter == std::string::npos ? a_managedKey : a_managedKey.substr(0, delimiter);
    }

    RE::TESBoundObject* LookupBoundObjectByKey(const std::string& a_key) {
        auto tokens = split(GetManagedItemBaseKey(a_key), '|');
        if (tokens.size() != 2) return nullptr;

        try {
            auto localID = static_cast<RE::FormID>(std::stoul(tokens[1], nullptr, 16));
            if (tokens[0] == "Dynamic" || tokens[0] == "Created") {
                return RE::TESForm::LookupByID<RE::TESBoundObject>(localID);
            }

            auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) return nullptr;

            auto formID = dataHandler->LookupFormID(localID, tokens[0]);
            return RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
        }
        catch (...) {
            return nullptr;
        }
    }

    uint32_t GetInventoryCount(RE::Actor* a_actor, RE::TESBoundObject* a_item) {
        if (!a_actor || !a_item) return 0;
        const auto count = a_actor->GetInventoryCount(a_item);
        return count > 0 ? static_cast<uint32_t>(count) : 0;
    }

    std::string BuildManagedItemKey(const std::string& a_itemKey, const std::string& a_ruleID,
        const std::string& a_groupName, bool a_isPersistent)
    {
        return a_itemKey + kManagedItemKeyDelimiter + (a_isPersistent ? "P" : "T") +
            kManagedItemKeyDelimiter + a_ruleID + kManagedItemKeyDelimiter + a_groupName;
    }

    bool ManagedItemMatches(const std::string& a_managedKey, const std::string& a_itemKey,
        const PersistentItemState& a_state, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        return GetManagedItemBaseKey(a_managedKey) == a_itemKey &&
            a_state.ruleID == a_ruleID &&
            a_state.groupName == a_groupName &&
            a_state.isPersistent == a_isPersistent;
    }

    PersistentItemState* FindManagedItemState(std::map<std::string, PersistentItemState>& a_items,
        const std::string& a_itemKey, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        const auto managedKey = BuildManagedItemKey(a_itemKey, a_ruleID, a_groupName, a_isPersistent);
        if (auto it = a_items.find(managedKey); it != a_items.end()) {
            return std::addressof(it->second);
        }

        for (auto& [key, state] : a_items) {
            if (ManagedItemMatches(key, a_itemKey, state, a_ruleID, a_groupName, a_isPersistent)) {
                return std::addressof(state);
            }
        }

        return nullptr;
    }

    const PersistentItemState* FindManagedItemState(const std::map<std::string, PersistentItemState>& a_items,
        const std::string& a_itemKey, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        const auto managedKey = BuildManagedItemKey(a_itemKey, a_ruleID, a_groupName, a_isPersistent);
        if (auto it = a_items.find(managedKey); it != a_items.end()) {
            return std::addressof(it->second);
        }

        for (const auto& [key, state] : a_items) {
            if (ManagedItemMatches(key, a_itemKey, state, a_ruleID, a_groupName, a_isPersistent)) {
                return std::addressof(state);
            }
        }

        return nullptr;
    }
}

bool IsActorInCombatContext(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }
    {
        std::lock_guard lock(g_combatStatesMutex);
        if (const auto found = g_combatStates.find(actor->GetFormID());
            found != g_combatStates.end()) {
            return found->second != RE::ACTOR_COMBAT_STATE::kNone;
        }
    }
    return actor->IsInCombat();
}

void UpdateActorCombatContext(
    RE::Actor* actor,
    const RE::ACTOR_COMBAT_STATE state)
{
    if (!actor) {
        return;
    }
    std::lock_guard lock(g_combatStatesMutex);
    g_combatStates[actor->GetFormID()] = state;
}

void BeginInventoryInteraction(RE::Actor* a_targetActor)
{
    std::set<RE::FormID> actorIDs;
    {
        std::lock_guard lock(g_inventoryInteractionMutex);
        if (!g_inventoryInteraction.active) {
            g_inventoryInteraction.actors.clear();
            g_inventoryInteraction.pendingDeltas.clear();
        }
        g_inventoryInteraction.active = true;
        g_inventoryInteraction.token =
            g_nextInventoryInteractionToken.fetch_add(
                1, std::memory_order_relaxed);

        const auto addActor = [&](RE::Actor* a_actor) {
            if (!a_actor) {
                return;
            }
            const auto actorID = a_actor->GetFormID();
            const auto actorHandle = a_actor->GetHandle();
            if (!actorHandle) {
                return;
            }
            g_inventoryInteraction.actors[actorID] = actorHandle;
            actorIDs.insert(actorID);
        };
        addActor(a_targetActor);
        addActor(RE::PlayerCharacter::GetSingleton());
    }

    // An evaluation queued just before the menu opened must not run midway
    // through the menu transaction. Move it into the same final-state batch.
    std::lock_guard schedulerLock(g_scheduledEvaluationsMutex);
    std::lock_guard interactionLock(g_inventoryInteractionMutex);
    for (const auto actorID : actorIDs) {
        const auto scheduled = g_scheduledEvaluations.find(actorID);
        if (scheduled == g_scheduledEvaluations.end()) {
            continue;
        }
        auto pendingIt =
            g_inventoryInteraction.pendingDeltas.try_emplace(
                actorID,
                MakeEmptyRuleEvaluationDelta()).first;
        pendingIt->second.Merge(scheduled->second.delta);
        g_scheduledEvaluations.erase(scheduled);
    }

    logger::debug(
        "[InventoryInteraction] Deferred EDF evaluation for {} actor(s).",
        actorIDs.size());
}

bool IsInventoryInteractionActive(RE::Actor* a_actor)
{
    if (!a_actor) {
        return false;
    }
    std::lock_guard lock(g_inventoryInteractionMutex);
    return g_inventoryInteraction.active &&
        g_inventoryInteraction.actors.contains(a_actor->GetFormID());
}

void EndInventoryInteraction()
{
    std::uint64_t token = 0;
    {
        std::lock_guard lock(g_inventoryInteractionMutex);
        if (!g_inventoryInteraction.active) {
            return;
        }
        token = g_inventoryInteraction.token;
    }

    // ContainerMenu can emit its final container/equip events immediately
    // after the close notification. Keep collecting briefly, then evaluate
    // only the settled inventory state on the main thread.
    Utils::DelayedDispatcher::Get().PostDelayed(
        std::chrono::milliseconds(75),
        [token]() {
            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                return;
            }
            taskInterface->AddTask([token]() {
                std::map<RE::FormID, RE::ActorHandle> actors;
                std::map<RE::FormID, RuleEvaluationDelta> pendingDeltas;
                {
                    std::lock_guard lock(g_inventoryInteractionMutex);
                    if (!g_inventoryInteraction.active ||
                        g_inventoryInteraction.token != token) {
                        return;
                    }
                    actors = std::move(g_inventoryInteraction.actors);
                    pendingDeltas =
                        std::move(g_inventoryInteraction.pendingDeltas);
                    g_inventoryInteraction = {};
                }

                for (auto& [actorID, delta] : pendingDeltas) {
                    const auto actorIt = actors.find(actorID);
                    if (actorIt == actors.end()) {
                        continue;
                    }
                    auto actorPtr = actorIt->second.get();
                    auto* actor = actorPtr ? actorPtr.get() : nullptr;
                    if (!actor || actor->IsDead()) {
                        continue;
                    }
                    ScheduleRuleEvaluation(actor, std::move(delta));
                }
                logger::debug(
                    "[InventoryInteraction] Released {} settled actor delta(s).",
                    pendingDeltas.size());
            });
        });
}

void ScheduleRuleEvaluation(RE::Actor* a_actor, RuleEvaluationDelta a_delta)
{
    if (!a_actor ||
        g_ruleEvaluationSuspended.load(std::memory_order_acquire)) {
        return;
    }

    if (DeferInventoryInteractionEvaluation(a_actor, a_delta)) {
        return;
    }

    const auto actorID = a_actor->GetFormID();
    if (g_activeProducedDelta && g_activeEvaluationActor == actorID) {
        g_activeProducedDelta->Merge(a_delta);
        return;
    }

    auto taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::error("[RuleScheduler] TaskInterface indisponível; avaliação de {:08X} ignorada.",
            a_actor->GetFormID());
        return;
    }

    const auto actorHandle = a_actor->GetHandle();
    if (!actorHandle) {
        logger::debug("[RuleScheduler] Ator {:08X} não possui handle válido.", actorID);
        return;
    }

    const auto token = g_nextEvaluationToken.fetch_add(1, std::memory_order_relaxed);
    bool startPump = false;
    std::uint64_t pumpGeneration = 0;
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        if (auto found = g_scheduledEvaluations.find(actorID);
            found != g_scheduledEvaluations.end()) {
            found->second.delta.Merge(a_delta);
            return;
        }
        g_scheduledEvaluations.emplace(
            actorID,
            PendingRuleEvaluation{ token, actorHandle, std::move(a_delta) });
        if (!g_ruleEvaluationPumpScheduled) {
            g_ruleEvaluationPumpScheduled = true;
            startPump = true;
            pumpGeneration = g_ruleEvaluationPumpGeneration;
        }
    }

    if (startPump) {
        taskInterface->AddTask([pumpGeneration]() {
            ProcessScheduledRuleEvaluations(pumpGeneration);
        });
    }
}

namespace {
    void ProcessScheduledRuleEvaluations(std::uint64_t a_generation)
    {
        {
            std::lock_guard lock(g_scheduledEvaluationsMutex);
            if (a_generation != g_ruleEvaluationPumpGeneration ||
                g_ruleEvaluationSuspended.load(
                    std::memory_order_acquire)) {
                return;
            }
        }
        constexpr auto kFrameBudget = std::chrono::microseconds(1500);
        constexpr std::size_t kMaxActorsPerPump = 32;
        const auto startedAt = std::chrono::steady_clock::now();
        std::size_t processed = 0;

        while (processed < kMaxActorsPerPump &&
            std::chrono::steady_clock::now() - startedAt < kFrameBudget) {
            RE::FormID actorID = 0;
            PendingRuleEvaluation pending;
            {
                std::lock_guard lock(g_scheduledEvaluationsMutex);
                if (g_scheduledEvaluations.empty()) {
                    g_ruleEvaluationPumpScheduled = false;
                    return;
                }
                auto next = g_scheduledEvaluations.begin();
                actorID = next->first;
                pending = std::move(next->second);
                g_scheduledEvaluations.erase(next);
            }

            auto actorPtr = pending.actorHandle.get();
            auto actor = actorPtr ? actorPtr.get() : nullptr;
            if (actor && !actor->IsDead() &&
                !g_ruleEvaluationSuspended.load(
                    std::memory_order_acquire)) {
                const auto& context =
                    SaveStateManager::GetSingleton()->GetCurrentContext();
                auto* manager = RuleManager::GetSingleton();
                if (context.isValid &&
                    (!manager->GetRules().empty() ||
                        manager->HasPendingRuleDeletions())) {
                    pending.delta.Merge(
                        RuleManager::GetSingleton()->DetectBaseNPCChanges(actor));
                    pending.delta.Merge(
                        RuleManager::GetSingleton()->DetectActorValueChanges(actor));
                    pending.delta.Merge(
                        RuleManager::GetSingleton()->DetectFollowerStateChanges(actor));
                    logger::debug(
                        "[RuleScheduler] Reavaliando '{}' ({:08X}); mask={:X}, forms={}.",
                        actor->GetName(), actorID, pending.delta.mask,
                        pending.delta.changedForms.size());
                    ApplyRulesToInstance(actor, pending.delta);
                }
            }
            ++processed;
        }

        auto taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            std::lock_guard lock(g_scheduledEvaluationsMutex);
            g_scheduledEvaluations.clear();
            g_ruleEvaluationPumpScheduled = false;
            return;
        }

        bool hasMore = false;
        {
            std::lock_guard lock(g_scheduledEvaluationsMutex);
            hasMore = !g_scheduledEvaluations.empty();
            if (!hasMore) {
                g_ruleEvaluationPumpScheduled = false;
            }
        }
        if (hasMore) {
            taskInterface->AddTask([a_generation]() {
                ProcessScheduledRuleEvaluations(a_generation);
            });
        }
    }
}

void ScheduleAllLoadedRuleEvaluations()
{
    std::set<RE::FormID> scheduled;
    const auto schedule = [&](RE::Actor* actor) {
        if (!actor || actor->IsDead() ||
            !scheduled.insert(actor->GetFormID()).second) {
            return;
        }
        ScheduleRuleEvaluation(actor, RuleEvaluationDelta::Full());
    };
    schedule(RE::PlayerCharacter::GetSingleton());
    if (auto* processLists = RE::ProcessLists::GetSingleton()) {
        const auto scheduleHandles = [&](auto& handles) {
            for (auto& handle : handles) {
                auto actorPtr = handle.get();
                schedule(actorPtr ? actorPtr.get() : nullptr);
            }
        };
        scheduleHandles(processLists->highActorHandles);
        scheduleHandles(processLists->lowActorHandles);
    }
    logger::info(
        "[RuleScheduler] Full evaluation queued for {} loaded actor(s).",
        scheduled.size());
}

void ScheduleSleepOutfitUpdate(RE::Actor* a_actor, bool a_isEntering)
{
    if (!a_actor) {
        return;
    }

    auto taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::error("[SleepScheduler] TaskInterface indisponível para {:08X}.", a_actor->GetFormID());
        return;
    }

    const auto actorID = a_actor->GetFormID();
    const auto actorHandle = a_actor->GetHandle();
    if (!actorHandle) {
        return;
    }

    const auto token = g_nextSleepToken.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        if (auto it = g_sleepUpdates.find(actorID); it != g_sleepUpdates.end()) {
            it->second.isEntering = a_isEntering;
            return;
        }
        g_sleepUpdates.emplace(actorID, PendingSleepUpdate{ token, a_isEntering });
    }

    taskInterface->AddTask([actorHandle, actorID, token]() {
        bool isEntering = false;
        {
            std::lock_guard lock(g_sleepUpdatesMutex);
            const auto it = g_sleepUpdates.find(actorID);
            if (it == g_sleepUpdates.end() || it->second.token != token) {
                return;
            }
            isEntering = it->second.isEntering;
            g_sleepUpdates.erase(it);
        }

        auto actorPtr = actorHandle.get();
        auto actor = actorPtr ? actorPtr.get() : nullptr;
        if (!actor || actor->IsDead()) {
            return;
        }

        {
            std::lock_guard lock(g_sleepUpdatesMutex);
            g_sleepStates[actorID] = isEntering;
        }
        ScheduleRuleEvaluation(
            actor,
            RuleEvaluationDelta::For(RuleDependency::kSleep));
    });
}

void ForgetRuleEvaluationRuntimeState(RE::FormID a_actorID)
{
    RuleManager::GetSingleton()->ForgetActorRuntimeState(a_actorID);
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        g_scheduledEvaluations.erase(a_actorID);
    }
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        g_sleepUpdates.erase(a_actorID);
        g_sleepStates.erase(a_actorID);
    }
    {
        std::lock_guard lock(g_combatStatesMutex);
        g_combatStates.erase(a_actorID);
    }
    {
        std::lock_guard lock(g_inventoryInteractionMutex);
        g_inventoryInteraction.actors.erase(a_actorID);
        g_inventoryInteraction.pendingDeltas.erase(a_actorID);
    }
}

void ResetRuleEvaluationRuntimeState()
{
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        g_scheduledEvaluations.clear();
        g_ruleEvaluationPumpScheduled = false;
        ++g_ruleEvaluationPumpGeneration;
    }
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        g_sleepUpdates.clear();
        g_sleepStates.clear();
    }
    {
        std::lock_guard lock(g_combatStatesMutex);
        g_combatStates.clear();
    }
    {
        std::lock_guard lock(g_inventoryInteractionMutex);
        g_inventoryInteraction = {};
        g_nextInventoryInteractionToken.fetch_add(
            1, std::memory_order_relaxed);
    }
    RuleManager::GetSingleton()->ResetRuntimeCaches();
    g_followerRefreshToken.fetch_add(1, std::memory_order_relaxed);
}

void SuspendRuleEvaluationForLoad()
{
    g_ruleEvaluationSuspended.store(
        true, std::memory_order_release);
    ResetRuleEvaluationRuntimeState();
    logger::info(
        "[RuleScheduler] Evaluation suspended for save transition.");
}

void ResumeRuleEvaluationAfterLoad()
{
    g_ruleEvaluationSuspended.store(
        false, std::memory_order_release);
    logger::info(
        "[RuleScheduler] Evaluation resumed after save transition.");

    std::uint64_t generation = 0;
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        generation = g_ruleEvaluationPumpGeneration;
    }
    Utils::DelayedDispatcher::Get().PostDelayed(
        std::chrono::milliseconds(150),
        [generation]() {
            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                return;
            }
            taskInterface->AddTask([generation]() {
                if (g_ruleEvaluationSuspended.load(
                        std::memory_order_acquire)) {
                    return;
                }
                {
                    std::lock_guard lock(
                        g_scheduledEvaluationsMutex);
                    if (generation !=
                        g_ruleEvaluationPumpGeneration) {
                        return;
                    }
                }

                std::size_t scheduledActors = 0;
                if (auto* player =
                        RE::PlayerCharacter::GetSingleton();
                    player && player->GetParentCell()) {
                    ScheduleRuleEvaluation(player);
                    ++scheduledActors;
                }
                if (auto* processLists =
                        RE::ProcessLists::GetSingleton()) {
                    for (auto& actorHandle :
                        processLists->highActorHandles) {
                        auto actorPtr = actorHandle.get();
                        auto* actor =
                            actorPtr ? actorPtr.get() : nullptr;
                        if (!actor || actor->IsPlayerRef() ||
                            actor->IsDead()) {
                            continue;
                        }
                        ScheduleRuleEvaluation(actor);
                        ++scheduledActors;
                    }
                }
                logger::info(
                    "[RuleScheduler] Post-load refresh queued "
                    "{} loaded actor(s).",
                    scheduledActors);
            });
        });
}

void QueueFollowerStateRefreshAfterDialogue()
{
    if (g_ruleEvaluationSuspended.load(
            std::memory_order_acquire)) {
        return;
    }

    std::uint64_t generation = 0;
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        generation = g_ruleEvaluationPumpGeneration;
    }
    const auto token =
        g_followerRefreshToken.fetch_add(
            1, std::memory_order_relaxed) + 1;

    Utils::DelayedDispatcher::Get().PostDelayed(
        std::chrono::milliseconds(200),
        [generation, token]() {
            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                return;
            }
            taskInterface->AddTask([generation, token]() {
                if (g_ruleEvaluationSuspended.load(
                        std::memory_order_acquire) ||
                    token != g_followerRefreshToken.load(
                        std::memory_order_relaxed)) {
                    return;
                }
                {
                    std::lock_guard lock(
                        g_scheduledEvaluationsMutex);
                    if (generation !=
                        g_ruleEvaluationPumpGeneration) {
                        return;
                    }
                }

                auto* processLists =
                    RE::ProcessLists::GetSingleton();
                if (!processLists) {
                    return;
                }

                std::size_t changedActors = 0;
                auto* ruleManager =
                    RuleManager::GetSingleton();
                for (auto& actorHandle :
                    processLists->highActorHandles) {
                    auto actorPtr = actorHandle.get();
                    auto* actor =
                        actorPtr ? actorPtr.get() : nullptr;
                    if (!actor || actor->IsPlayerRef() ||
                        actor->IsDead()) {
                        continue;
                    }

                    auto delta =
                        ruleManager->DetectFollowerStateChanges(
                            actor);
                    delta.mask |=
                        ToMask(RuleDependency::kRelationship) |
                        ToMask(RuleDependency::kQuest);
                    ScheduleRuleEvaluation(
                        actor, std::move(delta));
                    ++changedActors;
                }
                logger::debug(
                    "[FollowerState] Dialogue refresh scheduled "
                    "{} changed actor(s).",
                    changedActors);
            });
        });
}

SaveStateManager* SaveStateManager::GetSingleton() {
    static SaveStateManager instance;
    return &instance;
}

int GetPoolIndex(std::vector<std::string>& pool, const std::string& value) {
    auto it = std::find(pool.begin(), pool.end(), value);
    if (it != pool.end()) return static_cast<int>(std::distance(pool.begin(), it));
    pool.push_back(value);
    return static_cast<int>(pool.size() - 1);
}

std::vector<SaveHistoryEntry>& SaveStateManager::GetCharacterHistory(uint32_t characterID) {
    // Retorna a referência do histórico ou cria uma entrada vazia caso não exista
    return _characterHistory[characterID];
}

void SaveStateManager::SetCurrentContext(
    uint32_t a_charID,
    uint32_t a_saveNum,
    std::string_view a_saveName) {
    logger::info("[SaveManager] Definindo contexto: CharID {:08X}, Save #{}", a_charID, a_saveNum);
    ResetRuleEvaluationRuntimeState();
    _virtualKeywordOwners.clear();
    _virtualKeywordOwnersReady.clear();
    _managedFactionOwners.clear();
    _managedFactionOwnersReady.clear();

    _currentContext.charID = a_charID;
    _currentContext.saveNumber = a_saveNum;
    _currentContext.saveName =
        std::filesystem::path(a_saveName).filename().string();
    _currentContext.isValid = true; // Só é válido se tivermos um ID de personagem

    _sessionData = SaveHistoryEntry{};
    _sessionData.saveNumber = a_saveNum;
    _sessionData.saveName = _currentContext.saveName;

    if (a_charID != 0) {
        if (!_characterHistory.contains(a_charID)) {
            LoadCharacterData(a_charID);
        }

        auto& history = _characterHistory[a_charID];
        SaveHistoryEntry* bestSnapshot = nullptr;

        const auto restore = [&](const SaveHistoryEntry& a_entry) {
            _sessionData = a_entry;
            _sessionData.saveNumber = a_saveNum;
            _sessionData.saveName = _currentContext.saveName;
        };
        if (!_currentContext.saveName.empty()) {
            const auto exact = std::ranges::find_if(
                history, [&](const SaveHistoryEntry& a_entry) {
                    return a_entry.saveNumber == a_saveNum &&
                        a_entry.saveName == _currentContext.saveName;
                });
            if (exact != history.end()) {
                restore(*exact);
                logger::info(
                    "[SaveManager] Contexto restaurado pela identidade '{}' / #{}.",
                    _currentContext.saveName, a_saveNum);
                return;
            }
        }
        for (auto& entry : history) {
            if (entry.saveNumber == a_saveNum &&
                (_currentContext.saveName.empty() || entry.saveName.empty())) {
                restore(entry);
                logger::info("[SaveManager] Contexto restaurado com sucesso do save #{}", a_saveNum);
                return;
            }
            if (entry.saveNumber < a_saveNum && (!bestSnapshot || entry.saveNumber > bestSnapshot->saveNumber)) {
                bestSnapshot = &entry;
            }
        }

        if (bestSnapshot) {
            restore(*bestSnapshot);
            logger::info("[SaveManager] Contexto herdado do save anterior #{}", bestSnapshot->saveNumber);
        }
    }
}

std::string SaveStateManager::BuildFormKey(RE::TESForm* a_form) {
    if (!a_form) return "";

    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(a_form)) {
        fileNameStr = file->GetFilename();
    }
    else if (a_form->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    return fileNameStr + "|" + FormatLocalFormID(a_form->GetFormID(), fileNameStr);
}

void SaveStateManager::PersistCurrentSave(const std::string& a_saveName) {

    if (_currentContext.isValid && _currentContext.charID == 0) {
        try {
            std::filesystem::path p(a_saveName);
            std::string fileName = p.filename().string();
            size_t first_u = fileName.find('_');
            size_t second_u = fileName.find('_', first_u + 1);
            if (first_u != std::string::npos && second_u != std::string::npos) {
                std::string idStr = fileName.substr(first_u + 1, second_u - first_u - 1);
                _currentContext.charID = static_cast<uint32_t>(std::stoul(idStr, nullptr, 16));
                logger::info("[SaveManager] Migrando contexto de New Game para Personagem: {:X}", _currentContext.charID);
            }
        }
        catch (...) {}
    }
    auto& context = _currentContext;
    // Log de debug para rastrear por que pode estar falhando
    if (!context.isValid) {
        logger::warn("[SaveManager] PersistCurrentSave abortado: Contexto inválido (CharID: {:08X})", context.charID);
        return;
    }

    uint32_t newSaveNumber = context.saveNumber;
    try {
        // Extrai o nome do arquivo (ex: quiksave.ess ou Save1_...)
        std::filesystem::path p(a_saveName);
        std::string fileName = p.filename().string();

        // Tenta extrair o número do save se for o formato padrão do Skyrim
        size_t savePos = fileName.find("Save");
        size_t underscorePos = fileName.find('_');
        if (savePos != std::string::npos && underscorePos != std::string::npos) {
            std::string numStr = fileName.substr(savePos + 4, underscorePos - (savePos + 4));
            newSaveNumber = std::stoul(numStr);
        }
    }
    catch (...) {
        logger::error("[SaveManager] Erro ao processar número do save do nome: {}", a_saveName);
    }

    _sessionData.saveNumber = newSaveNumber;
    _sessionData.saveName =
        std::filesystem::path(a_saveName).filename().string();
    logger::info("[SaveManager] Sincronizando dados para o Save #{}...", newSaveNumber);

    RefreshPersistentItemsForLoadedActors();
    UpdateSaveEntry(context.charID, _sessionData);
    context.saveNumber = newSaveNumber; // Atualiza o contexto para o novo número
    context.saveName = _sessionData.saveName;
}

void SaveStateManager::ClearContext()
{
	logger::info("[SaveManager] Limpando contexto atual para CharID {:08X}", _currentContext.charID);
    ResetRuleEvaluationRuntimeState();
    _currentContext.isValid = false;
    _currentContext.saveNumber = 0;
    _currentContext.charID = 0;
    _currentContext.saveName.clear();
    _sessionData = SaveHistoryEntry{};
    _virtualKeywordOwners.clear();
    _virtualKeywordOwnersReady.clear();
    _managedFactionOwners.clear();
    _managedFactionOwnersReady.clear();
}

std::string SaveStateManager::BuildNPCKey(RE::Actor* a_actor) {
    if (!a_actor) return "";
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return "";

    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(baseNPC)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    return fileNameStr + "|" + FormatLocalFormID(a_actor->GetFormID(), fileNameStr);
}

void SaveStateManager::TrackPersistentItemGrant(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_count,
    const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent)
{
    if (!a_actor || !a_item || a_count == 0) return;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto itemKey = BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return;

    auto managedKey = BuildManagedItemKey(itemKey, a_ruleID, a_groupName, a_isPersistent);
    auto& itemState = _sessionData.persistentItems[npcKey][managedKey];
    itemState.expectedCount += a_count;
    itemState.ruleID = itemState.ruleID.empty() ? a_ruleID : itemState.ruleID;
    itemState.groupName = itemState.groupName.empty() ? a_groupName : itemState.groupName;
    itemState.isPersistent = a_isPersistent;

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    itemState.missingCount = itemState.expectedCount > currentCount ? itemState.expectedCount - currentCount : 0;

    logger::debug("[PersistentLedger] Grant registrado: NPC '{}' Item '{}' Esperado {} Ausente {}",
        a_actor->GetName(), a_item->GetName(), itemState.expectedCount, itemState.missingCount);
}

void SaveStateManager::EnsurePersistentItemTracked(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_expectedCount,
    const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent)
{
    if (!a_actor || !a_item || a_expectedCount == 0) return;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto itemKey = BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return;

    auto& npcItems = _sessionData.persistentItems[npcKey];
    if (FindManagedItemState(npcItems, itemKey, a_ruleID, a_groupName, a_isPersistent)) return;

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    if (currentCount == 0 && a_isPersistent) return;

    auto managedKey = BuildManagedItemKey(itemKey, a_ruleID, a_groupName, a_isPersistent);
    auto& itemState = npcItems[managedKey];
    itemState.expectedCount = a_expectedCount;
    itemState.missingCount = a_isPersistent && itemState.expectedCount > currentCount ? itemState.expectedCount - currentCount : 0;
    itemState.ruleID = a_ruleID;
    itemState.groupName = a_groupName;
    itemState.isPersistent = a_isPersistent;

    logger::debug("[PersistentLedger] Item existente adotado: NPC '{}' Item '{}' Esperado {} Ausente {}",
        a_actor->GetName(), a_item->GetName(), itemState.expectedCount, itemState.missingCount);
}

void SaveStateManager::AuditPersistentItems(RE::Actor* a_actor)
{
    if (!a_actor) return;

    const auto npcKey = BuildNPCKey(a_actor);
    if (npcKey.empty()) return;

    auto npcIt = _sessionData.persistentItems.find(npcKey);
    if (npcIt == _sessionData.persistentItems.end()) return;

    for (auto& [itemKey, itemState] : npcIt->second) {
        auto item = LookupBoundObjectByKey(itemKey);
        if (!item || itemState.expectedCount == 0) continue;

        const auto currentCount = GetInventoryCount(a_actor, item);
        const auto protectedCount = itemState.expectedCount > itemState.missingCount ? itemState.expectedCount - itemState.missingCount : 0;

        if (currentCount < protectedCount) {
            logger::debug("[PersistentLedger] NPC '{}' Item '{}' Esperado {} Atual {} Retirado {} Restauravel {}",
                a_actor->GetName(), item->GetName(), itemState.expectedCount, currentCount, itemState.missingCount, protectedCount - currentCount);
        }
    }
}

void SaveStateManager::RefreshPersistentItemsForLoadedActors()
{
    auto processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) return;

    auto auditList = [&](auto& handles) {
        for (auto& handle : handles) {
            auto actorPtr = handle.get();
            if (actorPtr) {
                AuditPersistentItems(actorPtr.get());
            }
        }
        };

    if (auto player = RE::PlayerCharacter::GetSingleton()) {
        AuditPersistentItems(player);
    }

    auditList(processLists->highActorHandles);
    auditList(processLists->middleHighActorHandles);
    auditList(processLists->lowActorHandles);
}

void SaveStateManager::HandleContainerChanged(const RE::TESContainerChangedEvent* a_event)
{
    if (!a_event || !_currentContext.isValid || a_event->itemCount <= 0) return;

    auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event->baseObj);
    if (!item) return;

    auto applyTransfer = [&](RE::FormID a_containerID, bool a_enteringOwner) {
        if (a_containerID == 0) return;

        auto actor = RE::TESForm::LookupByID<RE::Actor>(a_containerID);
        if (!actor) return;

        const auto npcKey = BuildNPCKey(actor);
        const auto itemKey = BuildFormKey(item);
        auto npcIt = _sessionData.persistentItems.find(npcKey);
        if (npcIt == _sessionData.persistentItems.end()) return;

        auto remainingMovedCount = static_cast<uint32_t>(a_event->itemCount);
        for (auto& [managedKey, itemState] : npcIt->second) {
            if (remainingMovedCount == 0) break;
            if (GetManagedItemBaseKey(managedKey) != itemKey) continue;

            const auto oldMissing = itemState.missingCount;
            const auto movedCount = remainingMovedCount;

            if (a_enteringOwner) {
                const auto restoredCount = std::min(movedCount, itemState.missingCount);
                itemState.missingCount -= restoredCount;
                remainingMovedCount -= restoredCount;
            }
            else {
                const auto availableDebt = itemState.expectedCount > itemState.missingCount ? itemState.expectedCount - itemState.missingCount : 0;
                const auto claimedCount = std::min(movedCount, availableDebt);
                itemState.missingCount += claimedCount;
                remainingMovedCount -= claimedCount;
            }

            if (oldMissing != itemState.missingCount) {
                logger::debug("[PersistentLedger] Transferencia: Dono '{}' Item '{}' Esperado {} Ausente {} -> {}",
                    actor->GetName(), item->GetName(), itemState.expectedCount, oldMissing, itemState.missingCount);
            }
        }
        };

    applyTransfer(a_event->oldContainer, false);
    applyTransfer(a_event->newContainer, true);
}

void SaveStateManager::EnsureVirtualKeywordOwners(RE::Actor* a_actor)
{
    if (!a_actor) {
        return;
    }

    const auto npcKey = BuildNPCKey(a_actor);
    if (npcKey.empty() || !_virtualKeywordOwnersReady.insert(npcKey).second) {
        return;
    }

    auto& ownersByKeyword = _virtualKeywordOwners[npcKey];
    if (const auto npcRules = _sessionData.npcRuleVersions.find(npcKey);
        npcRules != _sessionData.npcRuleVersions.end()) {
        for (const auto& [ruleID, state] : npcRules->second) {
            if (!state.isActive) {
                continue;
            }

            const Rule* rule = RuleManager::GetSingleton()->GetRuleVersion(
                ruleID, state.version);
            if (!rule) {
                const auto& currentRules = RuleManager::GetSingleton()->GetRules();
                const auto current = std::ranges::find_if(
                    currentRules,
                    [&ruleID](const Rule& a_rule) { return a_rule.id == ruleID; });
                if (current != currentRules.end()) {
                    rule = std::addressof(*current);
                }
            }
            if (!rule) {
                continue;
            }

            for (const auto& rewardKey : state.activeRewardKeys) {
                const RewardGroup* group = nullptr;
                const Reward* reward = nullptr;
                if (!ResolveRewardState(*rule, rewardKey, group, reward) ||
                    !group || !reward || reward->typeReward != "Keyword") {
                    continue;
                }
                const auto [plugin, formID] = reward->ParseFormID();
                auto keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(formID);
                if (!keyword) {
                    continue;
                }
                ownersByKeyword[BuildFormKey(keyword)].insert(
                    BuildRewardOwnerKey(ruleID, group->name, *reward));
            }
        }
    }

    if (const auto effective = _sessionData.virtualKeywords.find(npcKey);
        effective != _sessionData.virtualKeywords.end()) {
        for (const auto& keywordKey : effective->second) {
            if (ownersByKeyword[keywordKey].empty()) {
                ownersByKeyword[keywordKey].insert("legacy");
            }
        }
    }
}

bool SaveStateManager::AddVirtualKeyword(
    RE::Actor* a_actor,
    RE::BGSKeyword* a_keyword,
    std::string_view a_owner)
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    if (npcKey.empty() || keywordKey.empty()) return false;

    EnsureVirtualKeywordOwners(a_actor);
    auto& owners = _virtualKeywordOwners[npcKey][keywordKey];
    const auto ownerInserted = owners.insert(std::string(a_owner)).second;
    auto& keywords = _sessionData.virtualKeywords[npcKey];
    const auto [it, inserted] = keywords.insert(keywordKey);
    if (inserted) {
        logger::info("[TagDelta] Virtual keyword '{}' adicionada para '{}'", a_keyword->GetFormEditorID(), a_actor->GetName());
        ScheduleRuleEvaluation(
            a_actor,
            RuleEvaluationDelta::For(RuleDependency::kTag, a_keyword->GetFormID()));
    }
    return inserted || ownerInserted;
}

bool SaveStateManager::HasVirtualKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword) const
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    if (npcKey.empty() || keywordKey.empty()) return false;

    auto npcIt = _sessionData.virtualKeywords.find(npcKey);
    return npcIt != _sessionData.virtualKeywords.end() && npcIt->second.contains(keywordKey);
}

bool SaveStateManager::RemoveVirtualKeyword(
    RE::Actor* a_actor,
    RE::BGSKeyword* a_keyword,
    std::string_view a_owner)
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    EnsureVirtualKeywordOwners(a_actor);
    auto actorOwners = _virtualKeywordOwners.find(npcKey);
    if (actorOwners != _virtualKeywordOwners.end()) {
        auto keywordOwners = actorOwners->second.find(keywordKey);
        if (keywordOwners != actorOwners->second.end()) {
            keywordOwners->second.erase(std::string(a_owner));
            if (!keywordOwners->second.empty()) {
                return false;
            }
            actorOwners->second.erase(keywordOwners);
        }
    }

    auto npcIt = _sessionData.virtualKeywords.find(npcKey);
    if (npcIt == _sessionData.virtualKeywords.end()) return false;

    const bool removed = npcIt->second.erase(keywordKey) > 0;
    if (removed) {
        logger::info("[TagDelta] Virtual keyword '{}' removida de '{}'",
            a_keyword->GetFormEditorID(), a_actor->GetName());
        ScheduleRuleEvaluation(
            a_actor,
            RuleEvaluationDelta::For(RuleDependency::kTag, a_keyword->GetFormID()));
    }
    return removed;
}

void SaveStateManager::EnsureManagedFactionOwners(RE::Actor* a_actor)
{
    if (!a_actor) {
        return;
    }

    const auto npcKey = BuildNPCKey(a_actor);
    if (npcKey.empty() || !_managedFactionOwnersReady.insert(npcKey).second) {
        return;
    }

    auto& ownersByFaction = _managedFactionOwners[npcKey];
    if (const auto npcRules = _sessionData.npcRuleVersions.find(npcKey);
        npcRules != _sessionData.npcRuleVersions.end()) {
        for (const auto& [ruleID, state] : npcRules->second) {
            if (!state.isActive) {
                continue;
            }
            const Rule* rule = RuleManager::GetSingleton()->GetRuleVersion(
                ruleID, state.version);
            if (!rule) {
                const auto& currentRules = RuleManager::GetSingleton()->GetRules();
                const auto current = std::ranges::find_if(
                    currentRules,
                    [&ruleID](const Rule& a_rule) { return a_rule.id == ruleID; });
                if (current != currentRules.end()) {
                    rule = std::addressof(*current);
                }
            }
            if (!rule) {
                continue;
            }

            for (const auto& rewardKey : state.activeRewardKeys) {
                const RewardGroup* group = nullptr;
                const Reward* reward = nullptr;
                if (!ResolveRewardState(*rule, rewardKey, group, reward) ||
                    !group || !reward || reward->typeReward != "Faction") {
                    continue;
                }
                const auto [plugin, formID] = reward->ParseFormID();
                auto faction = RE::TESForm::LookupByID<RE::TESFaction>(formID);
                if (!faction) {
                    continue;
                }
                ownersByFaction[BuildFormKey(faction)]
                    [BuildRewardOwnerKey(ruleID, group->name, *reward)] =
                    std::clamp(static_cast<int>(reward->amount), -128, 127);
            }
        }
    }

    if (const auto effective = _sessionData.addedFactions.find(npcKey);
        effective != _sessionData.addedFactions.end()) {
        for (const auto& [factionKey, rank] : effective->second) {
            if (ownersByFaction[factionKey].empty()) {
                ownersByFaction[factionKey]["legacy"] = rank;
            }
        }
    }
}

bool SaveStateManager::AddManagedFaction(
    RE::Actor* a_actor,
    RE::TESFaction* a_faction,
    int a_rank,
    std::string_view a_owner)
{
    if (!a_actor || !a_faction) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto factionKey = BuildFormKey(a_faction);
    if (npcKey.empty() || factionKey.empty()) return false;

    EnsureManagedFactionOwners(a_actor);
    auto& owners = _managedFactionOwners[npcKey][factionKey];
    owners[std::string(a_owner)] = std::clamp(a_rank, -128, 127);
    const auto rank = std::ranges::max(
        owners | std::views::values);
    auto& factions = _sessionData.addedFactions[npcKey];
    auto it = factions.find(factionKey);
    const bool changed = it == factions.end() || it->second != rank || !a_actor->IsInFaction(a_faction);

    if (changed) {
        a_actor->AddToFaction(a_faction, static_cast<std::int8_t>(rank));
        factions[factionKey] = rank;
        logger::info("[TagDelta] Faction '{}' rank {} adicionada para '{}'", a_faction->GetFormEditorID(), rank, a_actor->GetName());
        RuleEvaluationDelta delta;
        delta.mask = GetRewardDependencyMask("Faction");
        delta.changedForms.insert(a_faction->GetFormID());
        ScheduleRuleEvaluation(a_actor, std::move(delta));
    }

    return changed;
}

bool SaveStateManager::RemoveManagedFaction(
    RE::Actor* a_actor,
    RE::TESFaction* a_faction,
    std::string_view a_owner)
{
    if (!a_actor || !a_faction) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto factionKey = BuildFormKey(a_faction);
    EnsureManagedFactionOwners(a_actor);
    if (auto actorOwners = _managedFactionOwners.find(npcKey);
        actorOwners != _managedFactionOwners.end()) {
        if (auto factionOwners = actorOwners->second.find(factionKey);
            factionOwners != actorOwners->second.end()) {
            factionOwners->second.erase(std::string(a_owner));
            if (!factionOwners->second.empty()) {
                const auto rank = std::ranges::max(
                    factionOwners->second | std::views::values);
                a_actor->AddToFaction(
                    a_faction, static_cast<std::int8_t>(rank));
                _sessionData.addedFactions[npcKey][factionKey] = rank;
                return false;
            }
            actorOwners->second.erase(factionOwners);
        }
    }

    auto npcIt = _sessionData.addedFactions.find(npcKey);
    if (npcIt == _sessionData.addedFactions.end() || !npcIt->second.contains(factionKey)) return false;

    a_actor->RemoveFromFaction(a_faction);
    npcIt->second.erase(factionKey);
    RuleEvaluationDelta delta;
    delta.mask = GetRewardDependencyMask("Faction");
    delta.changedForms.insert(a_faction->GetFormID());
    ScheduleRuleEvaluation(a_actor, std::move(delta));
    return true;
}

std::string SaveStateManager::GetCharacterPath(uint32_t characterID) {
    char hexID[9];
    sprintf_s(hexID, "%08X", characterID);
    const std::string fileName = std::string(hexID) + ".json";
    const std::filesystem::path newDir = "Data/Viny Mods/EDF/Saves";
    const std::filesystem::path legacyDir = "Data/SKSE/Plugins/EDF/Saves";
    std::filesystem::create_directories(newDir);

    const auto newPath = newDir / fileName;
    const auto legacyPath = legacyDir / fileName;
    if (!std::filesystem::exists(newPath) && std::filesystem::exists(legacyPath)) {
        std::error_code ec;
        std::filesystem::copy_file(legacyPath, newPath, std::filesystem::copy_options::skip_existing, ec);
        if (ec) {
            logger::warn("[SaveManager] Falha ao migrar save '{}' para '{}': {}", legacyPath.string(), newPath.string(), ec.message());
            return legacyPath.string();
        }
        logger::info("[SaveManager] Save migrado de '{}' para '{}'", legacyPath.string(), newPath.string());
    }

    return newPath.string();
}

void SaveStateManager::LoadCharacterData(uint32_t characterID) {
    std::string path = GetCharacterPath(characterID);
    logger::info("[SaveManager] Tentando carregar arquivo: {}", path);

    try {
        rapidjson::Document doc;
        const auto parseFile = [&](const std::filesystem::path& a_path) {
            std::ifstream input(a_path, std::ios::binary);
            if (!input.is_open()) return false;
            rapidjson::IStreamWrapper stream(input);
            doc.ParseStream(stream);
            return !doc.HasParseError();
        };
        if (!parseFile(path)) {
            const auto backupPath = std::filesystem::path(path).concat(".bak");
            logger::warn(
                "[SaveManager] Save lateral principal ausente ou inválido; tentando '{}'.",
                backupPath.string());
            if (!parseFile(backupPath)) {
                logger::error(
                    "[SaveManager] Não foi possível carregar '{}' nem seu backup.",
                    path);
                return;
            }
            logger::info(
                "[SaveManager] Histórico recuperado do backup '{}'.",
                backupPath.string());
        }
        if (doc.HasParseError()) {
            return;
        }

        // --- DETECÇÃO E CONVERSÃO DE FORMATO ANTIGO ---
        if (doc.IsArray()) {
            logger::info("[SaveManager] Formato antigo detectado para {:08X}. Iniciando migração...", characterID);

            std::vector<SaveHistoryEntry> legacyHistory;
            for (const auto& oldEntryJson : doc.GetArray()) {
                if (!oldEntryJson.IsObject()) continue;

                SaveHistoryEntry entry;
                if (auto saveNumber = FindMember(oldEntryJson, "saveNumber")) {
                    entry.saveNumber = GetJsonUint(*saveNumber);
                }

                // No formato antigo, npcRuleVersions mapeava ruleID para um int (versão)
                auto oldNpcMap = FindMember(oldEntryJson, "npcRuleVersions");
                if (oldNpcMap && oldNpcMap->IsObject()) {
                    for (auto npcIt = oldNpcMap->MemberBegin(); npcIt != oldNpcMap->MemberEnd(); ++npcIt) {
                        if (!npcIt->name.IsString() || !npcIt->value.IsObject()) continue;

                        const std::string npcKey = npcIt->name.GetString();
                        for (auto ruleIt = npcIt->value.MemberBegin(); ruleIt != npcIt->value.MemberEnd(); ++ruleIt) {
                            if (!ruleIt->name.IsString()) continue;

                            AppliedRuleState state;
                            state.version = static_cast<int>(GetJsonUint(ruleIt->value));
                            state.appliedGroups = {}; // Histórico de grupos não existia no formato antigo
                            entry.npcRuleVersions[npcKey][ruleIt->name.GetString()] = state;
                        }
                    }
                }
                legacyHistory.push_back(entry);
            }

            _characterHistory[characterID] = legacyHistory;

            if (!legacyHistory.empty()) {
                UpdateSaveEntry(characterID, legacyHistory.back());
                logger::info("[SaveManager] Migração concluída com sucesso para o disco.");
            }
            return;
        }

        if (!doc.IsObject()) return;

        // --- CARREGAMENTO DO FORMATO NOVO (EXISTENTE) ---
        auto plugins = ReadStringArray(FindMember(doc, "p_plugins"));
        auto rules = ReadStringArray(FindMember(doc, "p_rules"));
        auto groups = ReadStringArray(FindMember(doc, "p_groups"));
        auto items = ReadStringArray(FindMember(doc, "p_items"));
        auto tags = ReadStringArray(FindMember(doc, "p_tags"));
        auto rewardKeys = ReadStringArray(FindMember(doc, "p_rewards"));
        auto historyArray = FindMember(doc, "history");
        if (!historyArray || !historyArray->IsArray()) return;

        std::vector<SaveHistoryEntry> decodedHistory;
        for (const auto& h : historyArray->GetArray()) {
            if (!h.IsObject()) continue;

            SaveHistoryEntry entry;
            if (auto saveNumber = FindMember(h, "sn")) {
                entry.saveNumber = GetJsonUint(*saveNumber);
            }
            if (const auto saveName = FindMember(h, "sk");
                saveName && saveName->IsString()) {
                entry.saveName = saveName->GetString();
            }

            auto npcsJson = FindMember(h, "npcs");
            if (!npcsJson || !npcsJson->IsArray()) continue;

            for (const auto& npcItem : npcsJson->GetArray()) {
                if (!npcItem.IsArray() || npcItem.Size() < 3) continue;

                int pIdx = npcItem[0].IsInt() ? npcItem[0].GetInt() : -1;
                uint32_t fID = GetJsonUint(npcItem[1]);
                if (pIdx < 0 || static_cast<size_t>(pIdx) >= plugins.size()) continue;

                std::string npcKey = plugins[pIdx] + "|" + FormatLocalFormID(fID, plugins[pIdx]);
                const auto& rulesMap = npcItem[2];
                if (!rulesMap.IsObject()) continue;

                for (auto ruleIt = rulesMap.MemberBegin(); ruleIt != rulesMap.MemberEnd(); ++ruleIt) {
                    if (!ruleIt->name.IsString() || !ruleIt->value.IsArray() || ruleIt->value.Size() < 2) continue;

                    int rIdx = std::stoi(ruleIt->name.GetString());
                    if (rIdx < 0 || static_cast<size_t>(rIdx) >= rules.size()) continue;

                    const auto& rData = ruleIt->value;
                    AppliedRuleState state;
                    state.version = rData[0].IsInt() ? rData[0].GetInt() : 0;
                    if (rData[1].IsArray()) {
                        for (const auto& groupIndex : rData[1].GetArray()) {
                            int gIdx = groupIndex.IsInt() ? groupIndex.GetInt() : -1;
                            if (gIdx >= 0 && static_cast<size_t>(gIdx) < groups.size()) state.appliedGroups.push_back(groups[gIdx]);
                        }
                    }
                    if (rData.Size() >= 7 && rData[2].IsBool() && rData[3].IsBool() &&
                        rData[4].IsBool() && rData[5].IsArray() && rData[6].IsArray()) {
                        state.activationStateKnown = rData[2].GetBool();
                        state.isActive = rData[3].GetBool();
                        state.canRerollOnNextActivation = rData[4].GetBool();

                        for (const auto& rewardIndex : rData[5].GetArray()) {
                            int rewardIdx = rewardIndex.IsInt() ? rewardIndex.GetInt() : -1;
                            if (rewardIdx >= 0 && static_cast<size_t>(rewardIdx) < rewardKeys.size()) {
                                state.activeRewardKeys.push_back(rewardKeys[rewardIdx]);
                            }
                        }
                        for (const auto& rewardIndex : rData[6].GetArray()) {
                            int rewardIdx = rewardIndex.IsInt() ? rewardIndex.GetInt() : -1;
                            if (rewardIdx >= 0 && static_cast<size_t>(rewardIdx) < rewardKeys.size()) {
                                state.persistentRewardKeys.insert(rewardKeys[rewardIdx]);
                            }
                        }
                    }
                    if (rData.Size() >= 8 && rData[7].IsObject()) {
                        for (auto numericIt = rData[7].MemberBegin();
                             numericIt != rData[7].MemberEnd();
                             ++numericIt) {
                            if (!numericIt->name.IsString() ||
                                !numericIt->value.IsArray() ||
                                numericIt->value.Size() < 4) {
                                continue;
                            }
                            const auto rewardIndex =
                                std::stoi(numericIt->name.GetString());
                            if (rewardIndex < 0 ||
                                static_cast<std::size_t>(rewardIndex) >=
                                    rewardKeys.size()) {
                                continue;
                            }
                            const auto& value = numericIt->value;
                            if (!value[0].IsString() ||
                                !value[1].IsNumber() ||
                                !value[2].IsNumber() ||
                                !value[3].IsNumber()) {
                                continue;
                            }
                            state.numericRewardStates[
                                rewardKeys[rewardIndex]] = {
                                value[0].GetString(),
                                value[1].GetFloat(),
                                value[2].GetFloat(),
                                value[3].GetFloat()
                            };
                        }
                    }
                    entry.npcRuleVersions[npcKey][rules[rIdx]] = state;
                }

                if (npcItem.Size() >= 4 && npcItem[3].IsObject()) {
                    const auto& itemsMap = npcItem[3];
                    for (auto itemIt = itemsMap.MemberBegin(); itemIt != itemsMap.MemberEnd(); ++itemIt) {
                        if (!itemIt->name.IsString() || !itemIt->value.IsArray() || itemIt->value.Size() < 4) continue;

                        int itemIdx = std::stoi(itemIt->name.GetString());
                        if (itemIdx < 0 || static_cast<size_t>(itemIdx) >= items.size()) continue;

                        const auto& itemData = itemIt->value;
                        PersistentItemState itemState;
                        itemState.expectedCount = GetJsonUint(itemData[0]);
                        itemState.missingCount = GetJsonUint(itemData[1]);

                        int rIdx = itemData[2].IsInt() ? itemData[2].GetInt() : -1;
                        int gIdx = itemData[3].IsInt() ? itemData[3].GetInt() : -1;
                        if (rIdx >= 0 && static_cast<size_t>(rIdx) < rules.size()) itemState.ruleID = rules[rIdx];
                        if (gIdx >= 0 && static_cast<size_t>(gIdx) < groups.size()) itemState.groupName = groups[gIdx];
                        itemState.isPersistent = itemData.Size() >= 5 && itemData[4].IsBool() ? itemData[4].GetBool() : true;

                        if (itemState.expectedCount > 0) {
                            entry.persistentItems[npcKey][items[itemIdx]] = itemState;
                        }
                    }
                }

                if (npcItem.Size() >= 5 && npcItem[4].IsArray()) {
                    for (const auto& tagIndex : npcItem[4].GetArray()) {
                        int tIdx = tagIndex.IsInt() ? tagIndex.GetInt() : -1;
                        if (tIdx >= 0 && static_cast<size_t>(tIdx) < tags.size()) {
                            entry.virtualKeywords[npcKey].insert(tags[tIdx]);
                        }
                    }
                }

                if (npcItem.Size() >= 6 && npcItem[5].IsObject()) {
                    const auto& factionsMap = npcItem[5];
                    for (auto factIt = factionsMap.MemberBegin(); factIt != factionsMap.MemberEnd(); ++factIt) {
                        if (!factIt->name.IsString() || !factIt->value.IsInt()) continue;

                        int fIdx = std::stoi(factIt->name.GetString());
                        if (fIdx >= 0 && static_cast<size_t>(fIdx) < tags.size()) {
                            entry.addedFactions[npcKey][tags[fIdx]] = factIt->value.GetInt();
                        }
                    }
                }
            }
            decodedHistory.push_back(entry);
        }
        _characterHistory[characterID] = decodedHistory;
        logger::info("[SaveManager] Carregamento concluído. {} snapshots em memória.", decodedHistory.size());
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Erro ao carregar/converter save: {}", e.what());
    }
}
void SaveStateManager::UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry) {
    auto& history = _characterHistory[characterID];

    // Atualiza cache em memória
    auto it = std::find_if(history.begin(), history.end(), [&](const SaveHistoryEntry& e) {
        return e.saveNumber == newEntry.saveNumber &&
            (newEntry.saveName.empty() || e.saveName.empty() ||
             e.saveName == newEntry.saveName);
        });
    if (it != history.end()) {
        *it = newEntry;
        logger::debug("[SaveManager] Atualizando snapshot existente #{} em memória.", newEntry.saveNumber);
    }
    else {
        history.push_back(newEntry);
        logger::debug("[SaveManager] Adicionando novo snapshot #{} ao histórico.", newEntry.saveNumber);
    }

    logger::info("[SaveManager] Iniciando compressão para escrita (Save #{})", newEntry.saveNumber);

    // --- COMPRESSÃO ---
    std::vector<std::string> p_plugins, p_rules, p_groups, p_items, p_tags, p_rewards;

    try {
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("schemaVersion", 2, alloc);

        rapidjson::Value jHistory(rapidjson::kArrayType);
        for (const auto& entry : history) {
            rapidjson::Value jEntry(rapidjson::kObjectType);
            jEntry.AddMember("sn", entry.saveNumber, alloc);
            if (!entry.saveName.empty()) {
                rapidjson::Value saveName;
                saveName.SetString(
                    entry.saveName.c_str(),
                    static_cast<rapidjson::SizeType>(entry.saveName.size()),
                    alloc);
                jEntry.AddMember("sk", saveName, alloc);
            }

            rapidjson::Value jNPCs(rapidjson::kArrayType);
            for (auto const& [npcKey, ruleMap] : entry.npcRuleVersions) {
                auto tokens = split(npcKey, '|');
                if (tokens.size() < 2) continue;

                rapidjson::Value jNPC(rapidjson::kArrayType);
                jNPC.PushBack(GetPoolIndex(p_plugins, tokens[0]), alloc);
                jNPC.PushBack(static_cast<uint32_t>(std::stoul(tokens[1], nullptr, 16)), alloc);

                rapidjson::Value jRules(rapidjson::kObjectType);
                for (auto const& [ruleID, state] : ruleMap) {
                    rapidjson::Value jState(rapidjson::kArrayType);
                    jState.PushBack(state.version, alloc);

                    rapidjson::Value jGrp(rapidjson::kArrayType);
                    for (const auto& gName : state.appliedGroups) {
                        jGrp.PushBack(GetPoolIndex(p_groups, gName), alloc);
                    }
                    jState.PushBack(jGrp, alloc);
                    jState.PushBack(state.activationStateKnown, alloc);
                    jState.PushBack(state.isActive, alloc);
                    jState.PushBack(state.canRerollOnNextActivation, alloc);

                    rapidjson::Value jActiveRewards(rapidjson::kArrayType);
                    for (const auto& rewardKey : state.activeRewardKeys) {
                        jActiveRewards.PushBack(GetPoolIndex(p_rewards, rewardKey), alloc);
                    }
                    jState.PushBack(jActiveRewards, alloc);

                    rapidjson::Value jPersistentRewards(rapidjson::kArrayType);
                    for (const auto& rewardKey : state.persistentRewardKeys) {
                        jPersistentRewards.PushBack(GetPoolIndex(p_rewards, rewardKey), alloc);
                    }
                    jState.PushBack(jPersistentRewards, alloc);

                    rapidjson::Value jNumericRewards(rapidjson::kObjectType);
                    for (const auto& [rewardKey, numeric] :
                         state.numericRewardStates) {
                        const auto rewardIndex = std::to_string(
                            GetPoolIndex(p_rewards, rewardKey));
                        rapidjson::Value rewardJsonKey;
                        rewardJsonKey.SetString(
                            rewardIndex.c_str(),
                            static_cast<rapidjson::SizeType>(rewardIndex.size()),
                            alloc);
                        rapidjson::Value numericData(rapidjson::kArrayType);
                        rapidjson::Value targetName;
                        targetName.SetString(
                            numeric.targetActorValueName.c_str(),
                            static_cast<rapidjson::SizeType>(
                                numeric.targetActorValueName.size()),
                            alloc);
                        numericData.PushBack(targetName, alloc);
                        numericData.PushBack(
                            numeric.baselineAtActivation, alloc);
                        numericData.PushBack(
                            numeric.resolvedSourceValue, alloc);
                        numericData.PushBack(numeric.appliedDelta, alloc);
                        jNumericRewards.AddMember(
                            rewardJsonKey, numericData, alloc);
                    }
                    jState.PushBack(jNumericRewards, alloc);

                    const auto ruleIndex = std::to_string(GetPoolIndex(p_rules, ruleID));
                    rapidjson::Value ruleKey;
                    ruleKey.SetString(ruleIndex.c_str(), static_cast<rapidjson::SizeType>(ruleIndex.size()), alloc);
                    jRules.AddMember(ruleKey, jState, alloc);
                }
                jNPC.PushBack(jRules, alloc);

                rapidjson::Value jItems(rapidjson::kObjectType);
                auto itemNpcIt = entry.persistentItems.find(npcKey);
                if (itemNpcIt != entry.persistentItems.end()) {
                    for (auto const& [itemKey, itemState] : itemNpcIt->second) {
                        rapidjson::Value jItemState(rapidjson::kArrayType);
                        jItemState.PushBack(itemState.expectedCount, alloc);
                        jItemState.PushBack(itemState.missingCount, alloc);
                        jItemState.PushBack(GetPoolIndex(p_rules, itemState.ruleID), alloc);
                        jItemState.PushBack(GetPoolIndex(p_groups, itemState.groupName), alloc);
                        jItemState.PushBack(itemState.isPersistent, alloc);

                        const auto itemIndex = std::to_string(GetPoolIndex(p_items, itemKey));
                        rapidjson::Value itemJsonKey;
                        itemJsonKey.SetString(itemIndex.c_str(), static_cast<rapidjson::SizeType>(itemIndex.size()), alloc);
                        jItems.AddMember(itemJsonKey, jItemState, alloc);
                    }
                }
                jNPC.PushBack(jItems, alloc);

                rapidjson::Value jVirtualKeywords(rapidjson::kArrayType);
                auto keywordNpcIt = entry.virtualKeywords.find(npcKey);
                if (keywordNpcIt != entry.virtualKeywords.end()) {
                    for (const auto& keywordKey : keywordNpcIt->second) {
                        jVirtualKeywords.PushBack(GetPoolIndex(p_tags, keywordKey), alloc);
                    }
                }
                jNPC.PushBack(jVirtualKeywords, alloc);

                rapidjson::Value jFactions(rapidjson::kObjectType);
                auto factionNpcIt = entry.addedFactions.find(npcKey);
                if (factionNpcIt != entry.addedFactions.end()) {
                    for (const auto& [factionKey, rank] : factionNpcIt->second) {
                        const auto factionIndex = std::to_string(GetPoolIndex(p_tags, factionKey));
                        rapidjson::Value factionJsonKey;
                        factionJsonKey.SetString(factionIndex.c_str(), static_cast<rapidjson::SizeType>(factionIndex.size()), alloc);
                        jFactions.AddMember(factionJsonKey, rank, alloc);
                    }
                }
                jNPC.PushBack(jFactions, alloc);

                jNPCs.PushBack(jNPC, alloc);
            }
            jEntry.AddMember("npcs", jNPCs, alloc);
            jHistory.PushBack(jEntry, alloc);
        }

        doc.AddMember("p_plugins", WriteStringArray(p_plugins, alloc), alloc);
        doc.AddMember("p_rules", WriteStringArray(p_rules, alloc), alloc);
        doc.AddMember("p_groups", WriteStringArray(p_groups, alloc), alloc);
        doc.AddMember("p_items", WriteStringArray(p_items, alloc), alloc);
        doc.AddMember("p_tags", WriteStringArray(p_tags, alloc), alloc);
        doc.AddMember("p_rewards", WriteStringArray(p_rewards, alloc), alloc);
        doc.AddMember("history", jHistory, alloc);

        const auto started = std::chrono::steady_clock::now();
        const auto serialized = SerializeJson(doc);
        rapidjson::Document verification;
        verification.Parse(serialized.c_str(), serialized.size());
        if (verification.HasParseError() || !verification.IsObject()) {
            throw std::runtime_error(
                "generated save JSON failed validation");
        }

        const auto path = std::filesystem::path(
            GetCharacterPath(characterID));
        const auto temporaryPath =
            std::filesystem::path(path).concat(".tmp");
        const auto backupPath =
            std::filesystem::path(path).concat(".bak");
        std::filesystem::create_directories(path.parent_path());

        {
            std::ofstream output(
                temporaryPath,
                std::ios::binary | std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error(
                    "could not open temporary save file");
            }
            output.write(
                serialized.data(),
                static_cast<std::streamsize>(serialized.size()));
            output.flush();
            if (!output.good()) {
                throw std::runtime_error(
                    "could not flush temporary save file");
            }
        }
        {
            std::ifstream input(temporaryPath, std::ios::binary);
            rapidjson::IStreamWrapper stream(input);
            rapidjson::Document temporaryVerification;
            temporaryVerification.ParseStream(stream);
            if (!input.is_open() ||
                temporaryVerification.HasParseError() ||
                !temporaryVerification.IsObject()) {
                throw std::runtime_error(
                    "temporary save file failed validation");
            }
        }

        std::error_code ec;
        if (std::filesystem::exists(path)) {
            std::filesystem::copy_file(
                path,
                backupPath,
                std::filesystem::copy_options::overwrite_existing,
                ec);
            if (ec) {
                throw std::runtime_error(std::format(
                    "could not create save backup: {}", ec.message()));
            }
        }
        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH)) {
            const auto replaceError = std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category());
            std::error_code restoreError;
            if (!std::filesystem::exists(path) &&
                std::filesystem::exists(backupPath)) {
                std::filesystem::copy_file(
                    backupPath,
                    path,
                    std::filesystem::copy_options::overwrite_existing,
                    restoreError);
            }
            throw std::runtime_error(std::format(
                "could not promote temporary save: {}",
                replaceError.message()));
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        logger::info(
            "[SaveManager] Histórico atualizado: {} bytes em {} ms ('{}').",
            serialized.size(), elapsed.count(), path.string());
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Exceção durante a compressão/escrita do JSON: {}", e.what());
    }
}
bool IsActorSleeping(RE::Actor* a_actor) {
    if (!a_actor) return false;
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        if (const auto found = g_sleepStates.find(a_actor->GetFormID());
            found != g_sleepStates.end()) {
            return found->second;
        }
    }
    auto actorState = a_actor->AsActorState();
    if (!actorState) return false;

    auto sitSleepState = actorState->GetSitSleepState();
    return (sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping);
}

EquipmentContextMask GetCurrentEquipmentContext(RE::Actor* a_actor)
{
    if (IsActorInCombatContext(a_actor)) {
        return ToMask(EquipmentContext::kCombat);
    }
    if (IsActorSleeping(a_actor)) {
        return ToMask(EquipmentContext::kSleep);
    }
    return ToMask(EquipmentContext::kNormal);
}

bool IsRewardActiveInCurrentContext(
    RE::Actor* a_actor,
    const Reward& a_reward)
{
    return !IsEquipmentRewardType(a_reward.typeReward) ||
        (a_reward.equipContexts & GetCurrentEquipmentContext(a_actor)) != 0;
}

namespace {
    constexpr int kInventoryArmorPriority = 0;
    constexpr int kRuleOutfitArmorPriority = 1;
    constexpr int kRuleArmorPriority = 2;

    RE::TESBoundObject* GetPhysicalItemRewardObject(
        RE::TESForm* a_form,
        const std::string_view a_type)
    {
        if (!a_form) {
            return nullptr;
        }

        // Do not use TESBoundObject alone here: SpellItem and other logical
        // reward forms may inherit from it even though they are not inventory
        // items managed by the persistent-item ledger.
        if (a_type == "Weapon") return a_form->As<RE::TESObjectWEAP>();
        if (a_type == "Armor") return a_form->As<RE::TESObjectARMO>();
        if (a_type == "Potion") return a_form->As<RE::AlchemyItem>();
        if (a_type == "Ingredient") return a_form->As<RE::IngredientItem>();
        if (a_type == "Scroll") return a_form->As<RE::ScrollItem>();
        if (a_type == "Book") return a_form->As<RE::TESObjectBOOK>();
        if (a_type == "Ammo") return a_form->As<RE::TESAmmo>();
        if (a_type == "Light") return a_form->As<RE::TESObjectLIGH>();
        if (a_type == "Gold") return a_form->As<RE::TESObjectMISC>();
        if (a_type == "Misc") return a_form->As<RE::TESObjectMISC>();
        if (a_type == "SoulGem") return a_form->As<RE::TESSoulGem>();
        if (a_type == "Key") return a_form->As<RE::TESKey>();
        return nullptr;
    }

    struct ArmorEquipCandidate {
        RE::TESObjectARMO* armor = nullptr;
        RE::FormID formID = 0;
        int priority = kInventoryArmorPriority;
        float armorRating = 0.0f;
        std::uint32_t slotMask = 0;
        bool wasWorn = false;
    };

    struct EquipmentSnapshotEntry {
        RE::TESBoundObject* object = nullptr;
        RE::TESObjectARMO* armor = nullptr;
        std::int32_t count = 0;
        bool wasWorn = false;
    };

    struct EquipmentPlan {
        std::vector<RE::TESBoundObject*> toUnequip;
        std::vector<RE::TESBoundObject*> toEquip;

        bool HasChanges() const
        {
            return !toUnequip.empty() || !toEquip.empty();
        }
    };

    struct ActiveRuleArmorSelection {
        std::map<RE::FormID, int> priorities;
        std::set<RE::FormID> inactiveContextItems;
    };

    bool HasCompatibleArmorModel(RE::Actor* a_actor, RE::TESObjectARMO* a_armor)
    {
        if (!a_actor || !a_armor) return false;

        auto race = a_actor->GetRace();
        auto baseNPC = a_actor->GetActorBase();
        if (!race || !baseNPC) return false;

        const auto sex = baseNPC->GetSex();
        if (sex != RE::SEXES::kMale && sex != RE::SEXES::kFemale) return false;

        const auto armorMask = a_armor->GetSlotMask().underlying();
        if (armorMask == 0) return false;

        std::set<RE::FormID> visitedTemplates;
        for (auto currentArmor = a_armor;
             currentArmor && visitedTemplates.insert(currentArmor->GetFormID()).second;
             currentArmor = currentArmor->templateArmor) {
            for (auto* addon : currentArmor->armorAddons) {
                if (!addon || !addon->IsValidRace(race) ||
                    (addon->GetSlotMask().underlying() & armorMask) == 0) {
                    continue;
                }

                const auto* model = addon->bipedModels[sex].GetModel();
                if (model && model[0] != '\0') {
                    return true;
                }
            }
        }

        return false;
    }

    bool CanActorEquipRuleItem(
        RE::Actor* a_actor,
        RE::TESBoundObject* a_item)
    {
        if (!a_actor || !a_item) {
            return false;
        }
        auto race = a_actor->GetRace();
        if (!race) {
            return false;
        }

        if (auto armor = a_item->As<RE::TESObjectARMO>()) {
            if (!HasCompatibleArmorModel(a_actor, armor)) {
                return false;
            }
            return !armor->IsShield() ||
                race->validEquipTypes.any(
                    RE::TESRace::EquipmentFlag::kShield);
        }
        if (auto ammo = a_item->As<RE::TESAmmo>()) {
            return race->validEquipTypes.any(
                ammo->IsBolt() ?
                    RE::TESRace::EquipmentFlag::kCrossbow :
                    RE::TESRace::EquipmentFlag::kBow);
        }
        if (a_item->As<RE::TESObjectLIGH>()) {
            return race->validEquipTypes.any(
                RE::TESRace::EquipmentFlag::kTorch);
        }

        auto weapon = a_item->As<RE::TESObjectWEAP>();
        if (!weapon) {
            return true;
        }

        using EquipmentFlag = RE::TESRace::EquipmentFlag;
        switch (weapon->GetWeaponType()) {
        case RE::WEAPON_TYPE::kHandToHandMelee:
            return race->validEquipTypes.any(
                EquipmentFlag::kHandToHandMelee);
        case RE::WEAPON_TYPE::kOneHandSword:
            return race->validEquipTypes.any(
                EquipmentFlag::kOneHandSword);
        case RE::WEAPON_TYPE::kOneHandDagger:
            return race->validEquipTypes.any(
                EquipmentFlag::kOneHandDagger);
        case RE::WEAPON_TYPE::kOneHandAxe:
            return race->validEquipTypes.any(
                EquipmentFlag::kOneHandAxe);
        case RE::WEAPON_TYPE::kOneHandMace:
            return race->validEquipTypes.any(
                EquipmentFlag::kOneHandMace);
        case RE::WEAPON_TYPE::kTwoHandSword:
            return race->validEquipTypes.any(
                EquipmentFlag::kTwoHandSword);
        case RE::WEAPON_TYPE::kTwoHandAxe:
            return race->validEquipTypes.any(
                EquipmentFlag::kTwoHandAxe);
        case RE::WEAPON_TYPE::kBow:
            return race->validEquipTypes.any(
                EquipmentFlag::kBow);
        case RE::WEAPON_TYPE::kStaff:
            return race->validEquipTypes.any(
                EquipmentFlag::kStaff);
        case RE::WEAPON_TYPE::kCrossbow:
            return race->validEquipTypes.any(
                EquipmentFlag::kCrossbow);
        default:
            return false;
        }
    }

    void MarkPreferredArmor(
        std::map<RE::FormID, int>& a_priorities,
        RE::TESForm* a_form,
        int a_priority)
    {
        auto armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
        if (!armor) return;

        auto& currentPriority = a_priorities[armor->GetFormID()];
        currentPriority = std::max(currentPriority, a_priority);
    }

    ActiveRuleArmorSelection GetActiveRuleArmorSelection(RE::Actor* a_actor)
    {
        ActiveRuleArmorSelection selection;
        if (!a_actor) return selection;

        const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
        auto& session = SaveStateManager::GetSingleton()->GetSessionData();
        const auto npcIt = session.npcRuleVersions.find(npcKey);
        if (npcIt == session.npcRuleVersions.end()) return selection;

        for (const auto& [ruleID, state] : npcIt->second) {
            if (!state.activationStateKnown) continue;

            auto rule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, state.version);
            if (!rule) continue;

            std::set<std::string> relevantRewardKeys{
                state.persistentRewardKeys.begin(), state.persistentRewardKeys.end()
            };
            if (state.isActive) {
                relevantRewardKeys.insert(
                    state.activeRewardKeys.begin(), state.activeRewardKeys.end());
            }

            for (const auto& rewardKey : relevantRewardKeys) {
                const RewardGroup* group = nullptr;
                const Reward* reward = nullptr;
                if (!ResolveRewardState(*rule, rewardKey, group, reward) || !reward) continue;

                auto [plugin, formID] = reward->ParseFormID();
                auto rewardForm = RE::TESForm::LookupByID(formID);
                if (!rewardForm) continue;
                const bool contextActive =
                    IsRewardActiveInCurrentContext(a_actor, *reward);

                if (reward->typeReward == "Armor") {
                    if (contextActive) {
                        MarkPreferredArmor(selection.priorities, rewardForm, kRuleArmorPriority);
                    }
                    else if (auto armor = rewardForm->As<RE::TESObjectARMO>()) {
                        selection.inactiveContextItems.insert(armor->GetFormID());
                    }
                }
                else if (reward->typeReward == "Outfit") {
                    if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                        for (auto* outfitItem : outfit->outfitItems) {
                            auto armor = outfitItem ? outfitItem->As<RE::TESObjectARMO>() : nullptr;
                            if (!armor) continue;

                            if (contextActive) {
                                MarkPreferredArmor(
                                    selection.priorities, armor, kRuleOutfitArmorPriority);
                            }
                            else {
                                selection.inactiveContextItems.insert(armor->GetFormID());
                            }
                        }
                    }
                }
            }
        }

        for (const auto& preferred : selection.priorities) {
            selection.inactiveContextItems.erase(preferred.first);
        }
        return selection;
    }

    bool RuleHasEquipmentRewards(const Rule& a_rule)
    {
        for (const auto& group : a_rule.rewardGroups) {
            if (std::ranges::any_of(
                    group.rewards,
                    [](const Reward& a_reward) {
                        return IsEquipmentRewardType(a_reward.typeReward);
                    })) {
                return true;
            }
        }
        return false;
    }

    bool ActorHasManagedEquipmentRewards(RE::Actor* a_actor)
    {
        if (!a_actor) {
            return false;
        }

        const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
        const auto& session =
            SaveStateManager::GetSingleton()->GetSessionData();
        const auto npcIt = session.npcRuleVersions.find(npcKey);
        if (npcIt == session.npcRuleVersions.end()) {
            return false;
        }

        for (const auto& [ruleID, state] : npcIt->second) {
            if (!state.activationStateKnown ||
                (!state.isActive &&
                 state.persistentRewardKeys.empty())) {
                continue;
            }
            const auto* rule =
                RuleManager::GetSingleton()->GetRuleVersion(
                    ruleID, state.version);
            if (rule && RuleHasEquipmentRewards(*rule)) {
                return true;
            }
        }
        return false;
    }

    bool ShouldReconcileEquipmentForDelta(
        RE::Actor* a_actor,
        const RuleEvaluationDelta& a_delta)
    {
        if (!a_actor || a_actor->IsPlayer() ||
            !ActorHasManagedEquipmentRewards(a_actor)) {
            return false;
        }

        constexpr auto relevantMask =
            ToMask(RuleDependency::kSleep) |
            ToMask(RuleDependency::kCombat);
        return a_delta.IsFull() ||
            a_delta.allowEquipmentReconciliation ||
            (a_delta.mask & relevantMask) != 0;
    }

    std::vector<EquipmentSnapshotEntry> CaptureEquipmentSnapshot(
        RE::Actor* a_actor)
    {
        std::vector<EquipmentSnapshotEntry> snapshot;
        if (!a_actor) {
            return snapshot;
        }

        auto inventory = a_actor->GetInventory();
        snapshot.reserve(inventory.size());
        for (const auto& [item, inventoryData] : inventory) {
            if (!item || inventoryData.first <= 0) {
                continue;
            }

            // InventoryEntryData owns shallow ExtraDataList pointers. Read the
            // worn state now and never retain either pointer beyond this loop.
            const bool wasWorn =
                inventoryData.second &&
                inventoryData.second->IsWorn();
            snapshot.push_back({
                item,
                item->As<RE::TESObjectARMO>(),
                inventoryData.first,
                wasWorn
            });
        }
        return snapshot;
    }

    void AppendUniqueEquipmentObject(
        std::vector<RE::TESBoundObject*>& a_objects,
        RE::TESBoundObject* a_object)
    {
        if (!a_object) {
            return;
        }
        const auto formID = a_object->GetFormID();
        if (std::ranges::none_of(
                a_objects,
                [formID](const RE::TESBoundObject* a_existing) {
                    return a_existing &&
                        a_existing->GetFormID() == formID;
                })) {
            a_objects.push_back(a_object);
        }
    }

    EquipmentPlan BuildEquipmentPlan(
        RE::Actor* a_actor,
        const bool a_includeContextItems)
    {
        EquipmentPlan plan;
        if (!a_actor || a_actor->IsDead() || a_actor->IsPlayer()) {
            return plan;
        }

        struct ContextEquipCandidate {
            RE::TESBoundObject* object = nullptr;
            int priority = kInventoryArmorPriority;
        };

        std::map<RE::FormID, ContextEquipCandidate> desiredContextItems;
        std::set<RE::FormID> inactiveContextItems;
        if (a_includeContextItems) {
            const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
            const auto& session =
                SaveStateManager::GetSingleton()->GetSessionData();
            const auto npcIt =
                session.npcRuleVersions.find(npcKey);

            const auto registerItem = [&](
                RE::TESBoundObject* a_item,
                const bool a_contextActive,
                const int a_priority) {
                if (!a_item) {
                    return;
                }

                // Non-shield armor is resolved below as one compatible slot
                // set together with the actor's regular inventory armor.
                if (auto* armor =
                        a_item->As<RE::TESObjectARMO>();
                    armor && !armor->IsShield()) {
                    return;
                }

                const auto formID = a_item->GetFormID();
                if (!a_contextActive) {
                    inactiveContextItems.insert(formID);
                    return;
                }
                if (!CanActorEquipRuleItem(a_actor, a_item)) {
                    logger::debug(
                        "[EquipmentContext] Ignorando '{}' para '{}': "
                        "tipo de equipamento, raça ou modelo incompatível.",
                        a_item->GetName(),
                        a_actor->GetName());
                    return;
                }

                auto& candidate = desiredContextItems[formID];
                candidate.object = a_item;
                candidate.priority =
                    std::max(candidate.priority, a_priority);
            };

            if (npcIt != session.npcRuleVersions.end()) {
                for (const auto& [ruleID, state] : npcIt->second) {
                    if (!state.activationStateKnown) {
                        continue;
                    }

                    const auto* rule =
                        RuleManager::GetSingleton()->GetRuleVersion(
                            ruleID, state.version);
                    if (!rule) {
                        continue;
                    }

                    std::set<std::string> rewardKeys{
                        state.persistentRewardKeys.begin(),
                        state.persistentRewardKeys.end()
                    };
                    if (state.isActive) {
                        rewardKeys.insert(
                            state.activeRewardKeys.begin(),
                            state.activeRewardKeys.end());
                    }

                    for (const auto& rewardKey : rewardKeys) {
                        const RewardGroup* group = nullptr;
                        const Reward* reward = nullptr;
                        if (!ResolveRewardState(
                                *rule,
                                rewardKey,
                                group,
                                reward) ||
                            !reward ||
                            !IsEquipmentRewardType(
                                reward->typeReward)) {
                            continue;
                        }

                        const bool contextActive =
                            IsRewardActiveInCurrentContext(
                                a_actor, *reward);
                        const auto [plugin, formID] =
                            reward->ParseFormID();
                        auto* rewardForm =
                            RE::TESForm::LookupByID(formID);
                        if (!rewardForm) {
                            continue;
                        }

                        if (reward->typeReward == "Outfit") {
                            auto* outfit =
                                rewardForm->As<RE::BGSOutfit>();
                            if (!outfit) {
                                continue;
                            }
                            for (auto* form : outfit->outfitItems) {
                                registerItem(
                                    form ?
                                        form->As<RE::TESBoundObject>() :
                                        nullptr,
                                    contextActive,
                                    kRuleOutfitArmorPriority);
                            }
                        }
                        else {
                            registerItem(
                                rewardForm->As<RE::TESBoundObject>(),
                                contextActive,
                                kRuleArmorPriority);
                        }
                    }
                }
            }

            for (const auto& [formID, candidate] :
                 desiredContextItems) {
                inactiveContextItems.erase(formID);
            }
        }

        const auto activeRuleSelection =
            GetActiveRuleArmorSelection(a_actor);
        auto snapshot = CaptureEquipmentSnapshot(a_actor);

        std::vector<ArmorEquipCandidate> armorCandidates;
        armorCandidates.reserve(snapshot.size());
        for (const auto& entry : snapshot) {
            auto* armor = entry.armor;
            if (entry.count <= 0 || !armor ||
                armor->IsShield() ||
                activeRuleSelection.inactiveContextItems.contains(
                    armor->GetFormID())) {
                continue;
            }

            const auto slotMask =
                armor->GetSlotMask().underlying();
            if (slotMask == 0 ||
                !HasCompatibleArmorModel(a_actor, armor)) {
                logger::debug(
                    "[EquipBest] Ignorando '{}' para '{}': "
                    "raça, sexo, modelo ou slots incompatíveis.",
                    armor->GetName(),
                    a_actor->GetName());
                continue;
            }

            const auto priorityIt =
                activeRuleSelection.priorities.find(
                    armor->GetFormID());
            const auto priority =
                priorityIt !=
                    activeRuleSelection.priorities.end() ?
                    priorityIt->second :
                    kInventoryArmorPriority;
            armorCandidates.push_back({
                armor,
                armor->GetFormID(),
                priority,
                armor->GetArmorRating(),
                slotMask,
                entry.wasWorn
            });
        }

        std::ranges::sort(
            armorCandidates,
            [](const ArmorEquipCandidate& a_lhs,
               const ArmorEquipCandidate& a_rhs) {
                if (a_lhs.priority != a_rhs.priority) {
                    return a_lhs.priority > a_rhs.priority;
                }
                if (a_lhs.armorRating != a_rhs.armorRating) {
                    return a_lhs.armorRating >
                        a_rhs.armorRating;
                }
                return a_lhs.formID < a_rhs.formID;
            });

        std::set<RE::FormID> selectedArmorIDs;
        std::uint32_t occupiedSlots = 0;
        for (const auto& candidate : armorCandidates) {
            if ((occupiedSlots & candidate.slotMask) != 0) {
                continue;
            }
            selectedArmorIDs.insert(candidate.formID);
            occupiedSlots |= candidate.slotMask;
            if (!candidate.wasWorn) {
                AppendUniqueEquipmentObject(
                    plan.toEquip, candidate.armor);
            }
        }

        for (const auto& entry : snapshot) {
            if (!entry.object || !entry.wasWorn) {
                continue;
            }

            if (entry.armor && !entry.armor->IsShield()) {
                const auto formID = entry.armor->GetFormID();
                if (!selectedArmorIDs.contains(formID) &&
                    (activeRuleSelection.inactiveContextItems.contains(
                         formID) ||
                     (entry.armor->GetSlotMask().underlying() &
                      occupiedSlots) != 0)) {
                    AppendUniqueEquipmentObject(
                        plan.toUnequip, entry.object);
                }
                continue;
            }

            if (a_includeContextItems &&
                inactiveContextItems.contains(
                    entry.object->GetFormID())) {
                AppendUniqueEquipmentObject(
                    plan.toUnequip, entry.object);
            }
        }

        if (a_includeContextItems) {
            std::vector<ContextEquipCandidate> contextCandidates;
            contextCandidates.reserve(desiredContextItems.size());
            for (const auto& [formID, candidate] :
                 desiredContextItems) {
                contextCandidates.push_back(candidate);
            }
            std::ranges::sort(
                contextCandidates,
                [](const ContextEquipCandidate& a_lhs,
                   const ContextEquipCandidate& a_rhs) {
                    if (a_lhs.priority != a_rhs.priority) {
                        return a_lhs.priority <
                            a_rhs.priority;
                    }
                    return a_lhs.object->GetFormID() <
                        a_rhs.object->GetFormID();
                });

            for (const auto& candidate : contextCandidates) {
                const auto formID =
                    candidate.object->GetFormID();
                const auto found = std::ranges::find_if(
                    snapshot,
                    [formID](const EquipmentSnapshotEntry& a_entry) {
                        return a_entry.object &&
                            a_entry.object->GetFormID() == formID;
                    });
                if (candidate.object &&
                    found != snapshot.end() &&
                    found->count > 0 &&
                    !found->wasWorn) {
                    AppendUniqueEquipmentObject(
                        plan.toEquip, candidate.object);
                }
            }
        }

        std::ranges::sort(
            plan.toUnequip,
            [](const RE::TESBoundObject* a_lhs,
               const RE::TESBoundObject* a_rhs) {
                const auto lhsID =
                    a_lhs ? a_lhs->GetFormID() : 0;
                const auto rhsID =
                    a_rhs ? a_rhs->GetFormID() : 0;
                return lhsID < rhsID;
            });
        return plan;
    }

    bool ApplyEquipmentPlan(
        RE::Actor* a_actor,
        const EquipmentPlan& a_plan)
    {
        if (!a_actor || !a_plan.HasChanges()) {
            return false;
        }
        if (IsInventoryInteractionActive(a_actor)) {
            logger::debug(
                "[EquipmentPlan] Physical reconciliation deferred while "
                "'{}' is in an inventory interaction.",
                a_actor->GetName());
            return false;
        }

        auto* equipManager =
            RE::ActorEquipManager::GetSingleton();
        if (!equipManager) {
            return false;
        }

        ActiveEquipmentReconciliation transaction;
        transaction.actorID = a_actor->GetFormID();
        transaction.delta.mask =
            ToMask(RuleDependency::kEquipment) |
            ToMask(RuleDependency::kInventory);
        transaction.delta.allowEquipmentReconciliation = true;
        for (auto* object : a_plan.toUnequip) {
            if (object) {
                transaction.delta.changedForms.insert(
                    object->GetFormID());
            }
        }
        for (auto* object : a_plan.toEquip) {
            if (object) {
                transaction.delta.changedForms.insert(
                    object->GetFormID());
            }
        }

        {
            struct TransactionScope {
                ActiveEquipmentReconciliation* previous = nullptr;

                ~TransactionScope()
                {
                    g_activeEquipmentReconciliation = previous;
                }
            } transactionScope{
                g_activeEquipmentReconciliation
            };
            g_activeEquipmentReconciliation =
                std::addressof(transaction);

            // The inventory snapshot has already been discarded. From this
            // point onward no InventoryEntryData or ExtraDataList pointer is
            // consulted.
            for (auto* object : a_plan.toUnequip) {
                if (object) {
                    equipManager->UnequipObject(a_actor, object);
                }
            }
            for (auto* object : a_plan.toEquip) {
                if (!object) {
                    continue;
                }
                equipManager->EquipObject(
                    a_actor,
                    object,
                    nullptr,
                    1,
                    nullptr,
                    false,
                    false,
                    false,
                    true);
            }
        }
        ScheduleRuleEvaluation(
            a_actor, std::move(transaction.delta));
        return true;
    }

}

void EquipBestInventoryItems(RE::Actor* a_actor)
{
    ApplyEquipmentPlan(
        a_actor,
        BuildEquipmentPlan(a_actor, false));
}

void ReconcileEquipmentContext(RE::Actor* a_actor)
{
    ApplyEquipmentPlan(
        a_actor,
        BuildEquipmentPlan(a_actor, true));
}

bool CollectActiveEquipmentReconciliationEvent(
    RE::Actor* a_actor,
    const RE::FormID a_formID)
{
    if (!a_actor || !g_activeEquipmentReconciliation ||
        g_activeEquipmentReconciliation->actorID !=
            a_actor->GetFormID()) {
        return false;
    }

    g_activeEquipmentReconciliation->delta.mask |=
        ToMask(RuleDependency::kEquipment) |
        ToMask(RuleDependency::kInventory);
    if (a_formID != 0) {
        g_activeEquipmentReconciliation->delta.changedForms.insert(
            a_formID);
    }
    return true;
}

void ManageSleepOutfitState(RE::Actor* a_actor, bool)
{
    ReconcileEquipmentContext(a_actor);
}

struct PendingEquip {
    RE::TESBoundObject* object;
    int priority; // 0 para Outfit (Baixa), 1 para Recompensas Individuais (Alta)
};

struct ManagedRewardItemSnapshot {
    RE::TESBoundObject* item = nullptr;
    std::uint32_t expectedCount = 0;
    std::uint32_t missingCount = 0;
};

std::vector<ManagedRewardItemSnapshot>
GetManagedLeveledRewardItems(
    RE::Actor* a_actor,
    const Reward& a_reward,
    const std::string& a_ruleID,
    const std::string& a_groupName)
{
    std::vector<ManagedRewardItemSnapshot> result;
    if (!a_actor || a_reward.typeReward != "Leveled Item") {
        return result;
    }

    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    const auto ledgerGroup =
        BuildLeveledRewardLedgerGroup(a_groupName, a_reward);
    const auto& session =
        SaveStateManager::GetSingleton()->GetSessionData();
    const auto npcIt = session.persistentItems.find(npcKey);
    if (npcIt == session.persistentItems.end()) {
        return result;
    }

    for (const auto& [managedKey, state] : npcIt->second) {
        if (state.ruleID != a_ruleID ||
            state.groupName != ledgerGroup ||
            state.isPersistent != a_reward.isPersistent) {
            continue;
        }
        if (auto* item = LookupBoundObjectByKey(managedKey)) {
            result.push_back({
                item,
                state.expectedCount,
                state.missingCount
            });
        }
    }
    return result;
}

void RemoveManagedTemporaryItemOnInvalidation(RE::Actor* a_actor, RE::TESBoundObject* a_item,
    uint32_t a_fallbackCount, const std::string& a_ruleID, const std::string& a_groupName)
{
    if (!a_actor || !a_item || a_fallbackCount == 0) return;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    const auto itemKey = SaveStateManager::BuildFormKey(a_item);
    auto& session = saveManager->GetSessionData();
    auto npcIt = session.persistentItems.find(npcKey);

    uint32_t removeCount = a_fallbackCount;
    bool eraseManagedState = false;
    std::string managedKeyToErase;

    if (npcIt != session.persistentItems.end()) {
        for (auto& [managedKey, state] : npcIt->second) {
            if (!ManagedItemMatches(managedKey, itemKey, state, a_ruleID, a_groupName, false)) continue;

            const auto protectedCount = state.expectedCount > state.missingCount ? state.expectedCount - state.missingCount : 0;
            removeCount = protectedCount;
            eraseManagedState = state.missingCount == 0;
            managedKeyToErase = managedKey;
            break;
        }
    }

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    removeCount = std::min(removeCount, currentCount);
    if (removeCount > 0) {
        a_actor->RemoveItem(a_item, static_cast<std::int32_t>(removeCount), RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    }

    if (eraseManagedState && npcIt != session.persistentItems.end()) {
        npcIt->second.erase(managedKeyToErase);
        if (npcIt->second.empty()) {
            session.persistentItems.erase(npcIt);
        }
    }
}

void RemoveRuleRewards(RE::Actor* a_actor, const Rule& a_rule) {
    if (!a_actor) return;

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return;
    bool isPlayer = a_actor->IsPlayer();
    for (const auto& group : a_rule.rewardGroups) {
        for (const auto& reward : group.rewards) {
            if (reward.isPersistent) continue;
            if (reward.typeReward == "Actor Value" ||
                reward.typeReward == "Actor Scale") {
                const auto rewardKey =
                    BuildRewardStateKey(group.name, reward);
                if (reward.typeReward == "Actor Scale") {
                    QueueRemoveActorScaleReward(
                        a_actor, a_rule.id, rewardKey);
                } else {
                    QueueRemoveActorValueReward(
                        a_actor, a_rule.id, rewardKey);
                }
                continue;
            }
            auto [plugin, fID] = reward.ParseFormID();
            auto rewardForm = RE::TESForm::LookupByID(fID);
            if (!rewardForm) continue;

            if (reward.typeReward == "Spell") {
                if (auto spell = rewardForm->As<RE::SpellItem>()) {

                    // 1. Dispel (Para functionOnType 1 ou 2) - SUGERIDO POR VOCÊ
                    if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                        if (auto activeEffectList = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                            for (auto& effect : *activeEffectList) {
                                if (effect && effect->spell == spell) {
                                    effect->Dispel(true);
                                    logger::debug("[Dispel] Efeito de '{}' removido de {}.", spell->GetName(), a_actor->GetName());
                                }
                            }
                        }
                    }

                    // 2. RemoveSpell (Para functionOnType 0 ou 2)
                    if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                        // Se for NPC: Remove apenas se não for nativo do ActorBase
                        if (!isPlayer) {
                            if (!baseNPC->GetSpellList()->GetIndex(spell).has_value()) {
                                a_actor->RemoveSpell(spell);
                            }
                        }

                    }
                }
            }
            else if (reward.typeReward == "Shout") {
                if (auto shout = rewardForm->As<RE::TESShout>()) {
                    if (!isPlayer) {
                        if (auto spellList = baseNPC->GetSpellList(); spellList && spellList->GetIndex(shout).has_value()) {
                            spellList->RemoveShout(shout);
                        }
                    }
                }
            }
            else if (reward.typeReward == "Keyword") {
                if (auto keyword = rewardForm->As<RE::BGSKeyword>()) {
                    SaveStateManager::GetSingleton()->RemoveVirtualKeyword(
                        a_actor,
                        keyword,
                        BuildRewardOwnerKey(a_rule.id, group.name, reward));
                }
            }
            else if (reward.typeReward == "Faction") {
                if (auto faction = rewardForm->As<RE::TESFaction>()) {
                    SaveStateManager::GetSingleton()->RemoveManagedFaction(
                        a_actor,
                        faction,
                        BuildRewardOwnerKey(a_rule.id, group.name, reward));
                }
            }
            //else if (reward.typeReward == "Package") {
            //    if (auto pkg = rewardForm->As<RE::TESPackage>()) {
            //        auto& packageList = baseNPC->aiPackages.packages;

            //        // Verifica se o pacote já está na lista (conforme visto no Distribute.cpp)
            //        if (std::ranges::find(packageList, pkg) == packageList.end()) {
            //            // Adicionamos ao início (push_front) para que ele tenha prioridade sobre pacotes padrão
            //            packageList.push_front(pkg);

            //            // Força o NPC a reavaliar sua pilha de pacotes imediatamente
            //            a_actor->EvaluatePackage(false, false);
            //            logger::debug("[ApplyRules] Package {} adicionado ao BaseNPC {}", reward.formIDStr, baseNPC->GetName());
            //        }
            //    }
            //}
            else if (reward.typeReward == "Perk") {
                if (auto perk = rewardForm->As<RE::BGSPerk>()) {
                    if (!isPlayer) { // Proteção extra para o Player
                        bool isNative = false;
                        for (uint32_t i = 0; i < baseNPC->perkCount; i++) {
                            if (baseNPC->perks[i].perk == perk) { isNative = true; break; }
                        }
                        if (!isNative) a_actor->RemovePerk(perk);
                    }
                }
            }
            else if (reward.typeReward == "Outfit") {
                if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                    // Todos os modos adicionam os itens ao inventário. Na
                    // invalidação, removemos apenas a quantidade ainda
                    // atribuída a esta regra pelo ledger.
                    for (auto* form : outfit->outfitItems) {
                        if (auto* bound = form->As<RE::TESBoundObject>()) {
                            RemoveManagedTemporaryItemOnInvalidation(a_actor, bound, 1, a_rule.id, group.name);
                        }
                    }
                }
            }
            else if (reward.typeReward == "Leveled Item") {
                const auto ledgerGroup =
                    BuildLeveledRewardLedgerGroup(
                        group.name, reward);
                for (const auto& item :
                     GetManagedLeveledRewardItems(
                         a_actor,
                         reward,
                         a_rule.id,
                         group.name)) {
                    RemoveManagedTemporaryItemOnInvalidation(
                        a_actor,
                        item.item,
                        item.expectedCount,
                        a_rule.id,
                        ledgerGroup);
                    ScheduleRuleEvaluation(
                        a_actor,
                        RuleEvaluationDelta::For(
                            RuleDependency::kInventory,
                            item.item->GetFormID()));
                }
            }
            else {
                // Itens genéricos (Weapon, Armor, Ammo, etc)
                if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                    // Itens são mais difíceis de rastrear se são "nativos",
                    // mas removemos a quantidade estipulada pela regra.
                    RemoveManagedTemporaryItemOnInvalidation(a_actor, bound, reward.amount, a_rule.id, group.name);
                }
            }

            const auto dependencyMask =
                GetRewardDependencyMask(reward.typeReward);
            if (dependencyMask != ToMask(RuleDependency::kNone)) {
                RuleEvaluationDelta delta;
                delta.mask = dependencyMask;
                delta.changedForms.insert(fID);
                ScheduleRuleEvaluation(a_actor, std::move(delta));
            }
        }
    }
    logger::debug("[LocationUpdate] Regra '{}' desaplicada de {}. Atributos nativos preservados.", a_rule.name, a_actor->GetName());
}

namespace
{
    struct PendingSpellRewardCast
    {
        RE::ActorHandle actorHandle;
        RE::FormID actorID = 0;
        RE::FormID spellID = 0;
        std::string ruleID;
        std::string rewardKey;
        int ruleVersion = 0;
        std::uint64_t activationToken = 0;
        std::uint64_t schedulerGeneration = 0;
        std::uint8_t attempt = 0;
    };

    constexpr std::uint8_t kMaxSpellCastAttempts = 30;
    constexpr auto kInitialSpellCastDelay =
        std::chrono::milliseconds(100);
    constexpr auto kSpellCastRetryDelay =
        std::chrono::milliseconds(100);

    bool IsCurrentSchedulerGeneration(
        const std::uint64_t a_generation)
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        return a_generation == g_ruleEvaluationPumpGeneration;
    }

    bool IsSpellRewardStillActive(
        RE::Actor* a_actor,
        const PendingSpellRewardCast& a_request)
    {
        if (!a_actor) {
            return false;
        }

        const auto npcKey =
            SaveStateManager::BuildNPCKey(a_actor);
        const auto& session =
            SaveStateManager::GetSingleton()->GetSessionData();
        const auto npcIt =
            session.npcRuleVersions.find(npcKey);
        if (npcIt == session.npcRuleVersions.end()) {
            return false;
        }
        const auto stateIt =
            npcIt->second.find(a_request.ruleID);
        if (stateIt == npcIt->second.end()) {
            return false;
        }

        const auto& state = stateIt->second;
        return state.activationStateKnown &&
            state.isActive &&
            state.version == a_request.ruleVersion &&
            state.runtimeActivationToken ==
                a_request.activationToken &&
            std::ranges::find(
                state.activeRewardKeys,
                a_request.rewardKey) !=
                state.activeRewardKeys.end();
    }

    bool IsActorReadyForSpellRewardCast(RE::Actor* a_actor)
    {
        if (!a_actor || a_actor->IsDead() ||
            a_actor->IsDisabled() ||
            a_actor->IsMarkedForDeletion()) {
            return false;
        }

        auto* cell = a_actor->GetParentCell();
        if (!cell || !cell->IsAttached() ||
            !a_actor->Is3DLoaded() || !a_actor->Get3D()) {
            return false;
        }

        const auto& runtimeData =
            a_actor->GetActorRuntimeData();
        if (!runtimeData.currentProcess ||
            a_actor->GetProcessLevel() !=
                RE::PROCESS_TYPE::kHigh ||
            !a_actor->GetHighProcess()) {
            return false;
        }

        if (auto* ui = RE::UI::GetSingleton();
            ui && ui->GameIsPaused()) {
            return false;
        }
        return true;
    }

    void QueueSafeSpellRewardCast(
        PendingSpellRewardCast a_request,
        std::chrono::milliseconds a_delay);

    void ProcessSafeSpellRewardCast(
        PendingSpellRewardCast a_request)
    {
        if (g_ruleEvaluationSuspended.load(
                std::memory_order_acquire) ||
            !IsCurrentSchedulerGeneration(
                a_request.schedulerGeneration)) {
            return;
        }

        auto actorPtr = a_request.actorHandle.get();
        auto* actor =
            actorPtr ? actorPtr.get() : nullptr;
        if (!actor) {
            actor =
                RE::TESForm::LookupByID<RE::Actor>(
                    a_request.actorID);
            if (actor) {
                a_request.actorHandle = actor->GetHandle();
            }
        }
        if (!actor ||
            actor->GetFormID() != a_request.actorID ||
            actor->IsDead() ||
            actor->IsDisabled() ||
            actor->IsMarkedForDeletion() ||
            !IsSpellRewardStillActive(actor, a_request)) {
            return;
        }

        // Do not query actor-dependent rule state (especially inventory)
        // until the reference is fully attached and safe to inspect.
        if (!IsActorReadyForSpellRewardCast(actor)) {
            if (++a_request.attempt <
                kMaxSpellCastAttempts) {
                QueueSafeSpellRewardCast(
                    std::move(a_request),
                    kSpellCastRetryDelay);
            }
            else {
                logger::warn(
                    "[SpellReward] Cast skipped for actor "
                    "{:08X}: no safe attached cell/3D/high "
                    "process after {} attempts.",
                    a_request.actorID,
                    kMaxSpellCastAttempts);
            }
            return;
        }

        auto* rule =
            RuleManager::GetSingleton()->FindRule(
                a_request.ruleID);
        auto* baseNPC = actor->GetActorBase();
        if (!rule || !baseNPC || !rule->isEnabled ||
            rule->version != a_request.ruleVersion ||
            !MatchesRuleLevel(actor->GetLevel(), *rule) ||
            !IsNPCMatchingTargets(baseNPC, *rule, false, actor) ||
            IsNPCMatchingTargets(baseNPC, *rule, true, actor)) {
            return;
        }

        auto* spell =
            RE::TESForm::LookupByID<RE::SpellItem>(
                a_request.spellID);
        auto* caster = actor->GetMagicCaster(
            RE::MagicSystem::CastingSource::kInstant);
        if (!spell || !caster) {
            logger::warn(
                "[SpellReward] Cast skipped for '{}' "
                "({:08X}): spell {:08X} or instant caster "
                "is unavailable.",
                actor->GetName(),
                a_request.actorID,
                a_request.spellID);
            return;
        }

        caster->CastSpellImmediate(
            spell,
            false,
            actor,
            1.0f,
            false,
            0.0f,
            actor);
        logger::debug(
            "[SpellReward] Safely cast '{}' ({:08X}) for "
            "'{}' ({:08X}) after {} attempt(s).",
            spell->GetName(),
            a_request.spellID,
            actor->GetName(),
            a_request.actorID,
            static_cast<unsigned>(a_request.attempt) + 1);
    }

    void QueueSafeSpellRewardCast(
        PendingSpellRewardCast a_request,
        const std::chrono::milliseconds a_delay)
    {
        Utils::DelayedDispatcher::Get().PostDelayed(
            a_delay,
            [request = std::move(a_request)]() mutable {
                if (g_ruleEvaluationSuspended.load(
                        std::memory_order_acquire) ||
                    !IsCurrentSchedulerGeneration(
                        request.schedulerGeneration)) {
                    return;
                }

                auto* taskInterface =
                    SKSE::GetTaskInterface();
                if (!taskInterface) {
                    return;
                }
                taskInterface->AddTask(
                    [request = std::move(request)]() mutable {
                        ProcessSafeSpellRewardCast(
                            std::move(request));
                    });
            });
    }

    void ScheduleSafeSpellRewardCast(
        RE::Actor* a_actor,
        RE::SpellItem* a_spell,
        const Reward& a_reward,
        std::string_view a_ruleID,
        const int a_ruleVersion,
        const std::uint64_t a_activationToken,
        const std::string& a_groupName)
    {
        if (!a_actor || !a_spell ||
            a_activationToken == 0) {
            return;
        }

        PendingSpellRewardCast request;
        request.actorHandle = a_actor->GetHandle();
        request.actorID = a_actor->GetFormID();
        request.spellID = a_spell->GetFormID();
        request.ruleID = a_ruleID;
        request.rewardKey =
            BuildRewardStateKey(a_groupName, a_reward);
        request.ruleVersion = a_ruleVersion;
        request.activationToken = a_activationToken;
        {
            std::lock_guard lock(
                g_scheduledEvaluationsMutex);
            request.schedulerGeneration =
                g_ruleEvaluationPumpGeneration;
        }

        QueueSafeSpellRewardCast(
            std::move(request),
            kInitialSpellCastDelay);
    }
}

std::vector<std::pair<RE::TESBoundObject*, std::uint32_t>>
ResolveLeveledItemReward(
    RE::Actor* a_actor,
    RE::TESLevItem* a_list,
    const std::uint32_t a_count)
{
    std::vector<std::pair<RE::TESBoundObject*, std::uint32_t>> result;
    for (const auto& resolved :
         DistributionCore::ResolveLeveledItems(
             a_actor, a_list, a_count)) {
        result.emplace_back(resolved.item, resolved.count);
    }
    return result;
}

void ApplyRewardPhysical(
    RE::Actor* a_actor,
    const Reward& reward,
    bool isPlayer,
    std::vector<PendingEquip>& equipQueue,
    std::string_view a_ruleID,
    int a_ruleVersion,
    std::uint64_t a_activationToken,
    const std::string& a_groupName) {
    if (reward.typeReward == "Actor Value" ||
        reward.typeReward == "Actor Scale") {
        const auto rewardKey =
            BuildRewardStateKey(a_groupName, reward);
        if (reward.typeReward == "Actor Scale") {
            QueueActorScaleReward(
                a_actor, reward, a_ruleID, rewardKey);
        } else {
            QueueActorValueReward(
                a_actor, reward, a_ruleID, rewardKey);
        }
        return;
    }
    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    if (reward.typeReward == "Spell") {
        if (auto spell = rewardForm->As<RE::SpellItem>()) {
            bool changed = false;
            if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                if (!a_actor->HasSpell(spell)) {
                    a_actor->AddSpell(spell);
                    changed = true;
                }
            }
            if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                ScheduleSafeSpellRewardCast(
                    a_actor,
                    spell,
                    reward,
                    a_ruleID,
                    a_ruleVersion,
                    a_activationToken,
                    a_groupName);
            }
            if (changed) {
                ScheduleRuleEvaluation(
                    a_actor,
                    RuleEvaluationDelta::For(
                        RuleDependency::kAbility, spell->GetFormID()));
            }
        }
    }
    else if (reward.typeReward == "Shout") {
        if (auto shout = rewardForm->As<RE::TESShout>()) {
            if (!a_actor->HasShout(shout)) {
                a_actor->AddShout(shout);
                ScheduleRuleEvaluation(
                    a_actor,
                    RuleEvaluationDelta::For(
                        RuleDependency::kAbility, shout->GetFormID()));
            }
        }
    }
    else if (reward.typeReward == "Keyword") {
        if (auto keyword = rewardForm->As<RE::BGSKeyword>()) {
            SaveStateManager::GetSingleton()->AddVirtualKeyword(
                a_actor,
                keyword,
                BuildRewardOwnerKey(a_ruleID, a_groupName, reward));
        }
    }
    else if (reward.typeReward == "Faction") {
        if (auto faction = rewardForm->As<RE::TESFaction>()) {
            SaveStateManager::GetSingleton()->AddManagedFaction(
                a_actor,
                faction,
                static_cast<int>(reward.amount),
                BuildRewardOwnerKey(a_ruleID, a_groupName, reward));
        }
    }
    else if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            // Sempre adicionamos os itens ao inventário do NPC
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    a_actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
                    ScheduleRuleEvaluation(
                        a_actor,
                        RuleEvaluationDelta::For(
                            RuleDependency::kInventory, bound->GetFormID()));
                    // Só adicionamos à fila de equipagem imediata se o traje permitir
                    // uso padrão (Standard ou Both)
                    if (!isPlayer &&
                        IsRewardActiveInCurrentContext(a_actor, reward)) {
                        equipQueue.push_back({ bound, 0 });
                    }
                }
            }
        }
    }
    else if (reward.typeReward == "Perk") {
        if (auto perk = rewardForm->As<RE::BGSPerk>()) {
            if (!a_actor->HasPerk(perk)) {
                a_actor->AddPerk(perk, 1);
                ScheduleRuleEvaluation(
                    a_actor,
                    RuleEvaluationDelta::For(
                        RuleDependency::kAbility, perk->GetFormID()));
            }
        }
    }
    else if (reward.typeReward == "Leveled Item") {
        // Leveled rewards are resolved and tracked by
        // ApplyRewardPhysicalTracked. Never add the abstract LVLI form to an
        // actor inventory because the persistent ledger could not identify
        // the concrete result selected by the game.
        return;
    }
    else {
        // Itens físicos (Weapon, Armor, etc)
        if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
            a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);
            ScheduleRuleEvaluation(
                a_actor,
                RuleEvaluationDelta::For(
                    RuleDependency::kInventory, bound->GetFormID()));
            if (!isPlayer &&
                IsEquipmentRewardType(reward.typeReward) &&
                IsRewardActiveInCurrentContext(a_actor, reward)) {
                equipQueue.push_back({ bound, 1 });
            }
        }
    }
}

void TrackPersistentRewardItems(RE::Actor* a_actor, const Reward& reward, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    auto saveManager = SaveStateManager::GetSingleton();
    if (reward.typeReward == "Leveled Item") {
        return;
    }
    if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    saveManager->TrackPersistentItemGrant(a_actor, bound, 1, ruleID, groupName, reward.isPersistent);
                }
            }
        }
    }
    else if (auto bound =
                 GetPhysicalItemRewardObject(
                     rewardForm, reward.typeReward)) {
        saveManager->TrackPersistentItemGrant(a_actor, bound, reward.amount, ruleID, groupName, reward.isPersistent);
    }
}

void EnsurePersistentRewardTracked(RE::Actor* a_actor, const Reward& reward, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    auto saveManager = SaveStateManager::GetSingleton();
    if (reward.typeReward == "Leveled Item") {
        return;
    }
    if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    saveManager->EnsurePersistentItemTracked(a_actor, bound, 1, ruleID, groupName, reward.isPersistent);
                }
            }
        }
    }
    else if (auto bound =
                 GetPhysicalItemRewardObject(
                     rewardForm, reward.typeReward)) {
        saveManager->EnsurePersistentItemTracked(a_actor, bound, reward.amount, ruleID, groupName, reward.isPersistent);
    }
}

uint32_t GetPersistentRestoreCount(RE::Actor* a_actor, RE::TESBoundObject* a_item,
    const std::string& ruleID, const std::string& groupName, bool isPersistent)
{
    if (!a_actor || !a_item) return 0;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    const auto itemKey = SaveStateManager::BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return 0;

    auto& session = saveManager->GetSessionData();
    auto npcIt = session.persistentItems.find(npcKey);
    if (npcIt == session.persistentItems.end()) return 0;

    auto itemState = FindManagedItemState(npcIt->second, itemKey, ruleID, groupName, isPersistent);
    if (!itemState) return 0;

    const auto protectedCount = itemState->expectedCount > itemState->missingCount ? itemState->expectedCount - itemState->missingCount : 0;
    const auto currentCount = GetInventoryCount(a_actor, a_item);
    return protectedCount > currentCount ? protectedCount - currentCount : 0;
}

void RestorePersistentBoundObject(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_count,
    bool isPlayer, bool a_shouldEquip, std::vector<PendingEquip>& equipQueue)
{
    if (!a_actor || !a_item || a_count == 0) return;

    a_actor->AddObjectToContainer(a_item, nullptr, static_cast<std::int32_t>(a_count), nullptr);
    if (!isPlayer && a_shouldEquip &&
        (a_item->IsWeapon() || a_item->IsArmor() || a_item->IsAmmo() ||
            a_item->As<RE::TESObjectLIGH>())) {
        equipQueue.push_back({ a_item, 1 });
    }

    logger::info("[PersistentLedger] Restaurado '{}' x{} para '{}'", a_item->GetName(), a_count, a_actor->GetName());
}

void RestorePersistentRewardIfNeeded(RE::Actor* a_actor, const Reward& reward, bool isPlayer,
    std::vector<PendingEquip>& equipQueue, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    EnsurePersistentRewardTracked(a_actor, reward, ruleID, groupName);

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;
    const bool shouldEquip =
        IsRewardActiveInCurrentContext(a_actor, reward);

    if (reward.typeReward == "Leveled Item") {
        const auto ledgerGroup =
            BuildLeveledRewardLedgerGroup(groupName, reward);
        for (const auto& item :
             GetManagedLeveledRewardItems(
                 a_actor, reward, ruleID, groupName)) {
            RestorePersistentBoundObject(
                a_actor,
                item.item,
                GetPersistentRestoreCount(
                    a_actor,
                    item.item,
                    ruleID,
                    ledgerGroup,
                    reward.isPersistent),
                isPlayer,
                false,
                equipQueue);
        }
    }
    else if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    RestorePersistentBoundObject(a_actor, bound,
                        GetPersistentRestoreCount(a_actor, bound, ruleID, groupName, reward.isPersistent),
                        isPlayer, shouldEquip, equipQueue);
                }
            }
        }
    }
    else if (auto bound =
                 GetPhysicalItemRewardObject(
                     rewardForm, reward.typeReward)) {
        RestorePersistentBoundObject(a_actor, bound,
            GetPersistentRestoreCount(a_actor, bound, ruleID, groupName, reward.isPersistent),
            isPlayer, shouldEquip, equipQueue);
    }
}

void ApplyRewardPhysicalTracked(RE::Actor* a_actor, const Reward& reward, bool isPlayer, std::vector<PendingEquip>& equipQueue,
    const std::string& ruleID, int ruleVersion,
    std::uint64_t activationToken, const std::string& groupName)
{
    if (reward.typeReward == "Leveled Item") {
        const auto [plugin, formID] = reward.ParseFormID();
        auto* list =
            RE::TESForm::LookupByID<RE::TESLevItem>(formID);
        if (!list) {
            return;
        }
        const auto ledgerGroup =
            BuildLeveledRewardLedgerGroup(groupName, reward);
        for (const auto& [item, count] :
             ResolveLeveledItemReward(
                 a_actor, list, reward.amount)) {
            a_actor->AddObjectToContainer(
                item,
                nullptr,
                static_cast<std::int32_t>(count),
                nullptr);
            SaveStateManager::GetSingleton()->
                TrackPersistentItemGrant(
                    a_actor,
                    item,
                    count,
                    ruleID,
                    ledgerGroup,
                    reward.isPersistent);
            ScheduleRuleEvaluation(
                a_actor,
                RuleEvaluationDelta::For(
                    RuleDependency::kInventory,
                    item->GetFormID()));
        }
        return;
    }

    ApplyRewardPhysical(
        a_actor, reward, isPlayer, equipQueue, ruleID,
        ruleVersion, activationToken, groupName);
    TrackPersistentRewardItems(a_actor, reward, ruleID, groupName);
}

bool IsRewardRepresentedInCurrentState(
    RE::Actor* a_actor,
    const Reward& a_reward,
    const std::string& a_ruleID,
    const std::string& a_groupName)
{
    if (!a_actor) return false;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    auto& session = saveManager->GetSessionData();

    auto [plugin, fID] = a_reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return false;

    const auto hasTrackedBoundObject = [&](RE::TESBoundObject* a_item) {
        if (!a_item) return false;
        const auto npcIt = session.persistentItems.find(npcKey);
        if (npcIt == session.persistentItems.end()) return false;
        const auto itemKey = SaveStateManager::BuildFormKey(a_item);
        return FindManagedItemState(
            npcIt->second, itemKey, a_ruleID, a_groupName, a_reward.isPersistent) != nullptr;
    };

    if (a_reward.typeReward == "Leveled Item") {
        return !GetManagedLeveledRewardItems(
            a_actor,
            a_reward,
            a_ruleID,
            a_groupName).empty();
    }
    if (a_reward.typeReward == "Outfit") {
        auto outfit = rewardForm->As<RE::BGSOutfit>();
        if (!outfit) return false;
        return std::ranges::any_of(outfit->outfitItems, [&](RE::TESForm* a_form) {
            return hasTrackedBoundObject(a_form ? a_form->As<RE::TESBoundObject>() : nullptr);
        });
    }
    if (auto bound =
            GetPhysicalItemRewardObject(
                rewardForm, a_reward.typeReward)) {
        return hasTrackedBoundObject(bound);
    }
    if (a_reward.typeReward == "Keyword") {
        auto keyword = rewardForm->As<RE::BGSKeyword>();
        return keyword && saveManager->HasVirtualKeyword(a_actor, keyword);
    }
    if (a_reward.typeReward == "Faction") {
        auto faction = rewardForm->As<RE::TESFaction>();
        const auto factionKey = SaveStateManager::BuildFormKey(faction);
        const auto npcIt = session.addedFactions.find(npcKey);
        return faction && npcIt != session.addedFactions.end() && npcIt->second.contains(factionKey);
    }
    if (a_reward.typeReward == "Spell") {
        auto spell = rewardForm->As<RE::SpellItem>();
        if (!spell) return false;
        if ((a_reward.functionOnType == 0 || a_reward.functionOnType == 2) && a_actor->HasSpell(spell)) {
            return true;
        }
        if (a_reward.functionOnType == 1 || a_reward.functionOnType == 2) {
            if (auto effects = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                return std::ranges::any_of(*effects, [&](const auto& a_effect) {
                    return a_effect && a_effect->spell == spell;
                });
            }
        }
        return false;
    }
    if (a_reward.typeReward == "Shout") {
        auto shout = rewardForm->As<RE::TESShout>();
        return shout && a_actor->HasShout(shout);
    }
    if (a_reward.typeReward == "Perk") {
        auto perk = rewardForm->As<RE::BGSPerk>();
        return perk && a_actor->HasPerk(perk);
    }

    return false;
}

bool HasOutstandingNonPersistentReward(
    RE::Actor* a_actor,
    const Reward& a_reward,
    const std::string& a_ruleID,
    const std::string& a_groupName)
{
    if (!a_actor || a_reward.isPersistent) return false;

    auto [plugin, fID] = a_reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return false;

    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    const auto npcIt = session.persistentItems.find(npcKey);

    const auto hasMissingManagedCopies = [&](RE::TESBoundObject* a_item) {
        if (!a_item || npcIt == session.persistentItems.end()) return false;
        const auto itemKey = SaveStateManager::BuildFormKey(a_item);
        const auto itemState = FindManagedItemState(
            npcIt->second, itemKey, a_ruleID, a_groupName, false);
        return itemState && itemState->missingCount > 0;
    };

    if (a_reward.typeReward == "Leveled Item") {
        return std::ranges::any_of(
            GetManagedLeveledRewardItems(
                a_actor,
                a_reward,
                a_ruleID,
                a_groupName),
            [](const ManagedRewardItemSnapshot& a_item) {
                return a_item.missingCount > 0;
            });
    }
    if (a_reward.typeReward == "Outfit") {
        auto outfit = rewardForm->As<RE::BGSOutfit>();
        return outfit && std::ranges::any_of(outfit->outfitItems, [&](RE::TESForm* a_form) {
            return hasMissingManagedCopies(a_form ? a_form->As<RE::TESBoundObject>() : nullptr);
        });
    }
    if (auto bound =
            GetPhysicalItemRewardObject(
                rewardForm, a_reward.typeReward)) {
        return hasMissingManagedCopies(bound);
    }

    // Para rewards não físicos, permanecer no ator após a tentativa de
    // remoção significa que o resultado anterior ainda está presente.
    return IsRewardRepresentedInCurrentState(
        a_actor, a_reward, a_ruleID, a_groupName);
}

void MigrateLegacyActivationState(
    RE::Actor* a_actor,
    const Rule& a_rule,
    AppliedRuleState& a_state)
{
    a_state.activeRewardKeys.clear();
    a_state.persistentRewardKeys.clear();

    for (const auto& groupName : a_state.appliedGroups) {
        const auto groupIt = std::ranges::find_if(a_rule.rewardGroups, [&](const RewardGroup& a_group) {
            return a_group.name == groupName;
        });
        if (groupIt == a_rule.rewardGroups.end()) continue;

        for (const auto& reward : groupIt->rewards) {
            if (reward.typeReward != "Actor Value" &&
                reward.typeReward != "Actor Scale" &&
                !IsRewardRepresentedInCurrentState(
                    a_actor, reward, a_rule.id, groupIt->name)) {
                continue;
            }
            if (!IsActorValueRewardValid(reward) ||
                !IsActorScaleRewardValid(reward)) {
                continue;
            }

            const auto rewardKey = BuildRewardStateKey(groupIt->name, reward);
            a_state.activeRewardKeys.push_back(rewardKey);
            if (reward.isPersistent) {
                a_state.persistentRewardKeys.insert(rewardKey);
            }
            if (reward.typeReward == "Actor Value" &&
                reward.numericSource == NumericRewardSource::kFixed &&
                reward.numericOperation == NumericRewardOperation::kFlat) {
                const auto target =
                    ResolveActorValue(reward.actorValueName);
                const auto delta =
                    reward.actorValueAmount * reward.sourceMultiplier;
                a_state.numericRewardStates[rewardKey] = {
                    reward.actorValueName,
                    ReadActorValue(
                        a_actor, target, reward.percentBaseMode) - delta,
                    delta,
                    delta
                };
            }
        }
    }

    a_state.activationStateKnown = true;
    a_state.isActive = true;
    a_state.canRerollOnNextActivation = true;
    logger::info("[ActivationMigration] Regra '{}' migrada para '{}' com {} rewards identificados.",
        a_rule.name, a_actor->GetName(), a_state.activeRewardKeys.size());
}

void RemoveAppliedActivationRewards(
    RE::Actor* a_actor,
    const Rule& a_rule,
    AppliedRuleState& a_state)
{
    if (!a_actor) return;

    bool hasOutstandingRewards = false;
    if (!a_state.activationStateKnown) {
        RemoveRuleRewards(a_actor, a_rule);
    }
    else if (!a_state.activeRewardKeys.empty()) {
        Rule removalRule = a_rule;
        removalRule.rewardGroups.clear();

        for (const auto& rewardKey : a_state.activeRewardKeys) {
            const RewardGroup* group = nullptr;
            const Reward* reward = nullptr;
            if (!ResolveRewardState(a_rule, rewardKey, group, reward) || !group || !reward || reward->isPersistent) {
                continue;
            }

            if (reward->typeReward == "Actor Value" ||
                reward->typeReward == "Actor Scale") {
                if (!RemoveNumericReward(
                        a_actor, *reward, rewardKey,
                        a_rule.id, a_state)) {
                    hasOutstandingRewards = true;
                }
                continue;
            }

            auto targetGroup = std::ranges::find_if(removalRule.rewardGroups, [&](const RewardGroup& a_group) {
                return a_group.name == group->name;
            });
            if (targetGroup == removalRule.rewardGroups.end()) {
                RewardGroup selectedGroup = *group;
                selectedGroup.rewards.clear();
                removalRule.rewardGroups.push_back(std::move(selectedGroup));
                targetGroup = std::prev(removalRule.rewardGroups.end());
            }
            targetGroup->rewards.push_back(*reward);
        }

        if (!removalRule.rewardGroups.empty()) {
            RemoveRuleRewards(a_actor, removalRule);
        }

        for (const auto& rewardKey : a_state.activeRewardKeys) {
            const RewardGroup* group = nullptr;
            const Reward* reward = nullptr;
            if (!ResolveRewardState(a_rule, rewardKey, group, reward) || !group || !reward) continue;
            if (reward->typeReward == "Actor Value" ||
                reward->typeReward == "Actor Scale") continue;
            if (HasOutstandingNonPersistentReward(
                    a_actor, *reward, a_rule.id, group->name)) {
                hasOutstandingRewards = true;
                break;
            }
        }
    }

    a_state.activationStateKnown = true;
    a_state.isActive = false;
    a_state.runtimeActivationToken = 0;
    a_state.canRerollOnNextActivation = !hasOutstandingRewards;
    if (!hasOutstandingRewards) {
        a_state.activeRewardKeys.clear();
        a_state.appliedGroups.clear();
    }
}

void QueuePersistentOutfitForNormalUse(
    RE::Actor* a_actor,
    const Reward& a_reward,
    bool a_isPlayer,
    std::vector<PendingEquip>& a_equipQueue)
{
    if (!a_actor || a_isPlayer || a_reward.typeReward != "Outfit" ||
        !IsRewardActiveInCurrentContext(a_actor, a_reward)) {
        return;
    }

    auto [plugin, fID] = a_reward.ParseFormID();
    auto outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(fID);
    if (!outfit) return;

    for (auto* form : outfit->outfitItems) {
        auto bound = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (bound && a_actor->GetInventoryCount(bound) > 0) {
            a_equipQueue.push_back({ bound, 0 });
        }
    }
}

void StartRuleActivation(
    RE::Actor* a_actor,
    RE::TESNPC* a_baseNPC,
    const Rule& a_rule,
    AppliedRuleState& a_state,
    bool a_isPlayer,
    std::vector<PendingEquip>& a_equipQueue)
{
    a_state.activationStateKnown = true;
    a_state.isActive = true;
    a_state.canRerollOnNextActivation = true;
    a_state.version = a_rule.version;
    a_state.runtimeActivationToken =
        g_nextRuleActivationToken.fetch_add(
            1, std::memory_order_relaxed);
    a_state.appliedGroups.clear();
    a_state.activeRewardKeys.clear();

    const auto wonGroups = RuleManager::GetSingleton()->RollForGroups(a_baseNPC, a_rule);
    for (const auto& group : wonGroups) {
        a_state.appliedGroups.push_back(group.name);

        std::vector<const Reward*> selectedRewards;
        for (const auto index :
             DistributionCore::RollRewardIndices(
                 group,
                 [] {
                     return RuleManager::GetSingleton()->
                         GetRandomFloat(0.0f, 100.0f);
                 })) {
            selectedRewards.push_back(
                std::addressof(group.rewards[index]));
        }
        for (const auto* reward : selectedRewards) {
            if (!reward) continue;

            const auto rewardKey = BuildRewardStateKey(group.name, *reward);
            a_state.activeRewardKeys.push_back(rewardKey);

            if (reward->isPersistent &&
                ContainsRewardStateKey(
                    a_state.persistentRewardKeys, group.name, *reward)) {
                if (reward->typeReward == "Actor Value" ||
                    reward->typeReward == "Actor Scale") {
                    ApplyOrReconcileNumericReward(
                        a_actor, *reward, rewardKey,
                        a_rule.id, a_state);
                    continue;
                }
                RestorePersistentRewardIfNeeded(
                    a_actor, *reward, a_isPlayer, a_equipQueue, a_rule.id, group.name);
                QueuePersistentOutfitForNormalUse(a_actor, *reward, a_isPlayer, a_equipQueue);
                continue;
            }

            if (reward->typeReward == "Actor Value" ||
                reward->typeReward == "Actor Scale") {
                ApplyOrReconcileNumericReward(
                    a_actor, *reward, rewardKey,
                    a_rule.id, a_state);
            }
            else {
                ApplyRewardPhysicalTracked(
                    a_actor,
                    *reward,
                    a_isPlayer,
                    a_equipQueue,
                    a_rule.id,
                    a_rule.version,
                    a_state.runtimeActivationToken,
                    group.name);
            }
            if (reward->isPersistent) {
                a_state.persistentRewardKeys.insert(rewardKey);
            }
        }
    }
}

enum class RuleEvaluationPhase
{
    kStatic = 0,
    kQuest = 1,
    kRelationship = 2,
    kEnvironment = 3,
    kTag = 4,
    kFactionRank = 5,
    kFollower = 6,
    kAbility = 7,
    kInventory = 8,
    kEquipment = 9
};

RuleEvaluationPhase MaxPhase(RuleEvaluationPhase a_lhs, RuleEvaluationPhase a_rhs)
{
    return static_cast<int>(a_lhs) >= static_cast<int>(a_rhs) ? a_lhs : a_rhs;
}

RuleEvaluationPhase GetFilterEvaluationPhase(const std::string& a_type)
{
    if (a_type == "Keyword" || a_type == "Faction") {
        return RuleEvaluationPhase::kTag;
    }
    if (a_type == "Quest" || a_type == "NPC Trait") {
        return RuleEvaluationPhase::kQuest;
    }
    if (a_type == "Relationship Rank") {
        return RuleEvaluationPhase::kRelationship;
    }
    if (a_type == "Location" || a_type == "Cell" ||
        a_type == "Worldspace" || a_type == "Cell Type" ||
        a_type == "City Status" ||
        a_type == "Location Keyword") {
        return RuleEvaluationPhase::kEnvironment;
    }
    if (a_type == "Faction Rank") {
        return RuleEvaluationPhase::kFactionRank;
    }
    if (a_type == "Perk" || a_type == "Spell" || a_type == "Shout" ||
        a_type == "Actor Value") {
        return RuleEvaluationPhase::kAbility;
    }
    if (a_type == "Inventory Item" || a_type == "Inventory Count" ||
        a_type == "Gold" || a_type == "Equipped Item") {
        return RuleEvaluationPhase::kInventory;
    }
    if (a_type == "Equipped Category") {
        return RuleEvaluationPhase::kEquipment;
    }
    return RuleEvaluationPhase::kStatic;
}

RuleEvaluationPhase GetRuleEvaluationPhase(const Rule& a_rule)
{
    auto phase = RuleEvaluationPhase::kStatic;
    if (a_rule.followerState != RuleFollowerState::kAny) {
        phase = RuleEvaluationPhase::kFollower;
    }
    for (const auto& filter : a_rule.targetFilters) {
        phase = MaxPhase(phase, GetFilterEvaluationPhase(filter.type));
    }
    for (const auto& filter : a_rule.blacklistFilters) {
        phase = MaxPhase(phase, GetFilterEvaluationPhase(filter.type));
    }
    return phase;
}

const char* RuleEvaluationPhaseName(RuleEvaluationPhase a_phase)
{
    switch (a_phase) {
    case RuleEvaluationPhase::kStatic:
        return "Static";
    case RuleEvaluationPhase::kQuest:
        return "Quest/NPC Trait";
    case RuleEvaluationPhase::kRelationship:
        return "Relationship";
    case RuleEvaluationPhase::kEnvironment:
        return "Environment";
    case RuleEvaluationPhase::kTag:
        return "Keyword/Faction";
    case RuleEvaluationPhase::kFactionRank:
        return "Faction Rank";
    case RuleEvaluationPhase::kFollower:
        return "Follower";
    case RuleEvaluationPhase::kAbility:
        return "Spell/Perk/Shout/Actor Value";
    case RuleEvaluationPhase::kInventory:
        return "Inventory";
    case RuleEvaluationPhase::kEquipment:
        return "Equipment";
    default:
        return "Unknown";
    }
}

static void ApplyRulesToInstancePass(
    RE::Actor* a_actor,
    const RuleEvaluationDelta& a_delta,
    int a_forcedLevel) {
    if (!a_actor) return;

    // --- LOG DE INÍCIO ---
    std::string actorName = a_actor->GetName();
    RE::FormID actorID = a_actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(g_processingMutex);
        if (g_actorsInProcess.contains(actorID)) {
            // Se já estiver processando este ator, ignora a nova chamada para evitar conflitos
            // logger::debug("[ApplyRules] Ignorando chamada duplicada para {:08X} (em processamento)", actorID);
            return;
        }
        g_actorsInProcess.insert(actorID);
    }

    // Garantir que o ID seja removido do conjunto ao sair da função (RAII)
    struct ProcessCleaner {
        RE::FormID id;
        ~ProcessCleaner() {
            std::lock_guard<std::mutex> lock(g_processingMutex);
            g_actorsInProcess.erase(id);
        }
    } cleaner{ actorID };
    //logger::debug("[ApplyRules] INICIANDO processamento para {} (ID: {:08X})", actorName, actorID);

    if (a_actor->IsDead()) {
        //logger::info("[ApplyRules] FINALIZADO: Ator está morto.");
        return;
    }

    bool isPlayer = (actorID == 0x00000014) || a_actor->IsPlayer();
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) {
        logger::debug("[ApplyRules] FINALIZADO: BaseNPC não encontrado.");
        return;
    }

    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid) {
        logger::debug("[ApplyRules] FINALIZADO: Contexto de save inválido.");
        return;
    }

    auto* ruleManager = RuleManager::GetSingleton();
    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    bool equipmentReconciliationNeeded = false;
    constexpr auto equipmentObservationMask =
        ToMask(RuleDependency::kInventory) |
        ToMask(RuleDependency::kEquipment);
    const bool isPureExternalEquipmentObservation =
        !a_delta.IsFull() &&
        !a_delta.allowEquipmentReconciliation &&
        (a_delta.mask & equipmentObservationMask) != 0 &&
        (a_delta.mask & ~equipmentObservationMask) == 0;
    const auto actorLevel =
        a_forcedLevel != -1 ?
            a_forcedLevel :
            a_actor->GetLevel();
    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(baseNPC)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(actorID, fileNameStr);
    auto& actorRuleStates = session.npcRuleVersions[npcKey];

    // Saves anteriores ao ledger numérico já têm o ModAV gravado no .ess.
    // Documentamos esse delta antes de observar os baselines para não aplicá-lo
    // novamente nem incorporá-lo à base de um reward percentual.
    for (auto& [ruleID, state] : actorRuleStates) {
        if (state.activationStateKnown || state.version <= 0) {
            continue;
        }
        const auto* historicalRule =
            ruleManager->GetRuleVersion(ruleID, state.version);
        if (!historicalRule) {
            historicalRule = ruleManager->FindRule(ruleID);
        }
        if (historicalRule) {
            MigrateLegacyActivationState(
                a_actor, *historicalRule, state);
        }
    }

    // --- LÓGICA DE REMOÇÃO DE REGRAS INVÁLIDAS ---
    if (session.npcRuleVersions.contains(npcKey)) {
        auto& appliedRulesMap = session.npcRuleVersions[npcKey];
        for (auto it = appliedRulesMap.begin(); it != appliedRulesMap.end();) {
            const std::string& ruleID = it->first;
            auto& state = it->second;
            const auto* ruleDef = ruleManager->FindRule(ruleID);

            bool shouldRemove = false;
            if (!ruleDef) {
                shouldRemove = true;
                QueueRemoveActorValueRule(a_actor, ruleID);
                QueueRemoveActorScaleRule(a_actor, ruleID);
                for (const auto& [rewardKey, numeric] :
                     state.numericRewardStates) {
                    (void)rewardKey;
                    const auto target = ResolveActorValue(
                        numeric.targetActorValueName);
                    if (target != RE::ActorValue::kNone) {
                        a_actor->AsActorValueOwner()->ModActorValue(
                            RE::ACTOR_VALUE_MODIFIERS::kPermanent,
                            target,
                            -numeric.appliedDelta);
                    }
                }
                state.numericRewardStates.clear();
                state.persistentRewardKeys.clear();
            }
            else {
                const Rule& rule = *ruleDef;
                if (rule.isEnabled &&
                    ruleManager->IsRuleInUnstableCycle(
                        rule.id)) {
                    ++it;
                    continue;
                }
                const auto dependencyMask =
                    ruleManager->GetRuleDependencyMask(rule.id);
                if (!a_delta.IsFull() &&
                    (dependencyMask & a_delta.mask) == 0) {
                    ++it;
                    continue;
                }
                if (!rule.isEnabled ||
                    !MatchesRuleLevel(actorLevel, rule) ||
                    !IsNPCMatchingTargets(baseNPC, rule, false, a_actor) ||
                    IsNPCMatchingTargets(baseNPC, rule, true, a_actor)) {
                    shouldRemove = true;
                }
            }

            if (shouldRemove) {
                if (!state.activationStateKnown || state.isActive) {
                    const Rule* appliedRule = ruleManager->GetRuleVersion(ruleID, state.version);
                    if (!appliedRule && ruleDef) {
                        appliedRule = ruleDef;
                    }

                    if (appliedRule) {
                        if (!isPureExternalEquipmentObservation) {
                            equipmentReconciliationNeeded |=
                                RuleHasEquipmentRewards(*appliedRule);
                        }
                        RemoveAppliedActivationRewards(a_actor, *appliedRule, state);
                    }
                    else {
                        state.activationStateKnown = true;
                        state.isActive = false;
                        state.runtimeActivationToken = 0;
                        state.canRerollOnNextActivation = true;
                        state.activeRewardKeys.clear();
                        state.appliedGroups.clear();
                    }
                }
                ++it;
            }
            else {
                ++it;
            }
        }
    }

    // --- COLETA DE REGRAS ---
    auto rulesToProcess =
        ruleManager->GetCandidateRuleIDs(a_actor, a_delta);
    std::unordered_set<std::string> scheduledRuleIDs(
        rulesToProcess.begin(), rulesToProcess.end());

    // Regras já documentadas também precisam ser vistas quando uma de suas
    // dependências muda, inclusive para executar a cascata de remoção.
    if (session.npcRuleVersions.contains(npcKey)) {
        for (const auto& [ruleID, state] : session.npcRuleVersions.at(npcKey)) {
            if (!a_delta.IsFull() &&
                (ruleManager->GetRuleDependencyMask(ruleID) &
                    a_delta.mask) == 0) {
                continue;
            }
            if (scheduledRuleIDs.insert(ruleID).second) {
                rulesToProcess.push_back(ruleID);
            }
        }
    }

    logger::debug(
        "[RuleScheduler] '{}' evaluating {} candidate rules (mask={:X}, changedForms={}).",
        actorName, rulesToProcess.size(), a_delta.mask, a_delta.changedForms.size());

    if (rulesToProcess.empty()) {
        ReconcileActorValueLedger(a_actor, actorRuleStates);
        if (!isPlayer &&
            (equipmentReconciliationNeeded ||
             ShouldReconcileEquipmentForDelta(
                 a_actor, a_delta))) {
            ReconcileEquipmentContext(a_actor);
        }
        logger::debug("[ApplyRules] FINALIZADO: Nenhuma regra aplicável para {}", actorName);
        return;
    }

    // --- PROCESSAMENTO E APLICAÇÃO ---
    std::vector<PendingEquip> equipQueue;
    constexpr std::array rulePhases{
        RuleEvaluationPhase::kStatic,
        RuleEvaluationPhase::kQuest,
        RuleEvaluationPhase::kRelationship,
        RuleEvaluationPhase::kEnvironment,
        RuleEvaluationPhase::kTag,
        RuleEvaluationPhase::kFactionRank,
        RuleEvaluationPhase::kFollower,
        RuleEvaluationPhase::kAbility,
        RuleEvaluationPhase::kInventory,
        RuleEvaluationPhase::kEquipment
    };

    std::array<std::vector<const Rule*>, rulePhases.size()> rulesByPhase;
    for (const auto& ruleID : rulesToProcess) {
        const auto* rule = ruleManager->FindRule(ruleID);
        if (!rule || !rule->isEnabled ||
            ruleManager->IsRuleInUnstableCycle(rule->id)) {
            continue;
        }
        const auto phaseIndex =
            static_cast<std::size_t>(GetRuleEvaluationPhase(*rule));
        if (phaseIndex < rulesByPhase.size()) {
            rulesByPhase[phaseIndex].push_back(rule);
        }
    }

    for (std::size_t phaseIndex = 0;
         phaseIndex < rulePhases.size();
         ++phaseIndex) {
        const auto phase = rulePhases[phaseIndex];
        logger::debug("[RulePhase] Processando fase '{}' para '{}'", RuleEvaluationPhaseName(phase), actorName);

        std::vector<const Rule*> phaseRules;
        phaseRules.reserve(rulesByPhase[phaseIndex].size());
        for (const auto* currentRule : rulesByPhase[phaseIndex]) {
            if (!currentRule ||
                !IsNPCMatchingTargets(baseNPC, *currentRule, false, a_actor) ||
                IsNPCMatchingTargets(baseNPC, *currentRule, true, a_actor)) {
                continue;
            }

            if (!MatchesRuleLevel(actorLevel, *currentRule)) {
                continue;
            }

            phaseRules.push_back(currentRule);
        }

        for (const auto* currentRulePtr : phaseRules) {
            if (!currentRulePtr) continue;
            const Rule& currentRule = *currentRulePtr;
            const std::string& ruleID = currentRule.id;

            if (!MatchesRuleLevel(actorLevel, currentRule)) {
                continue;
            }
            AppliedRuleState& state = session.npcRuleVersions[npcKey][ruleID];
            int oldVersion = state.version;

            const Rule* previousRule = oldVersion > 0 ?
                ruleManager->GetRuleVersion(ruleID, oldVersion) :
                nullptr;

            if (!state.activationStateKnown && oldVersion > 0) {
                MigrateLegacyActivationState(
                    a_actor, previousRule ? *previousRule : currentRule, state);
            }

            if (oldVersion > 0 && oldVersion != currentRule.version) {
                QueueRemoveActorValueRule(a_actor, ruleID);
                QueueRemoveActorScaleRule(a_actor, ruleID);
                if (state.isActive) {
                    RemoveAppliedActivationRewards(
                        a_actor, previousRule ? *previousRule : currentRule, state);
                }
                for (const auto& [rewardKey, numeric] :
                     state.numericRewardStates) {
                    (void)rewardKey;
                    const auto target = ResolveActorValue(
                        numeric.targetActorValueName);
                    if (target != RE::ActorValue::kNone) {
                        a_actor->AsActorValueOwner()->ModActorValue(
                            RE::ACTOR_VALUE_MODIFIERS::kPermanent,
                            target,
                            -numeric.appliedDelta);
                    }
                }
                state.numericRewardStates.clear();
                state.persistentRewardKeys.clear();
                state.isActive = false;
                state.runtimeActivationToken = 0;
                state.canRerollOnNextActivation = true;
                state.activeRewardKeys.clear();
                state.appliedGroups.clear();
            }

            if (!state.isActive) {
                if (state.canRerollOnNextActivation) {
                    logger::debug("[Activation] Nova ativação da regra '{}' para '{}'.",
                        currentRule.name, actorName);
                    StartRuleActivation(
                        a_actor, baseNPC, currentRule, state, isPlayer, equipQueue);
                }
                else {
                    // Algum reward não persistente saiu do ator e continua
                    // documentado pelo ledger. Reativamos sem novo sorteio
                    // para impedir duplicação/farming.
                    state.isActive = true;
                    state.version = currentRule.version;
                    logger::debug(
                        "[Activation] Regra '{}' reativada para '{}' sem reroll; rewards anteriores continuam documentados.",
                        currentRule.name, actorName);
                }
            }
            else {
                // A regra continua na mesma ativação: não há reroll. Apenas
                // rewards persistentes podem ser reconciliados pelo ledger.
                for (const auto& rewardKey : state.activeRewardKeys) {
                    const RewardGroup* group = nullptr;
                    const Reward* reward = nullptr;
                    if (!ResolveRewardState(currentRule, rewardKey, group, reward) ||
                        !group || !reward) {
                        continue;
                    }

                    if (reward->typeReward == "Actor Value" ||
                        reward->typeReward == "Actor Scale") {
                        ApplyOrReconcileNumericReward(
                            a_actor, *reward, rewardKey,
                            ruleID, state);
                        continue;
                    }
                    if (!reward->isPersistent) continue;

                    RestorePersistentRewardIfNeeded(
                        a_actor, *reward, isPlayer, equipQueue, ruleID, group->name);
                }
                state.version = currentRule.version;
            }


        // Only rewards physically added/restored by this pass authorize an
        // equipment reconciliation. External equip events remain
        // observational and are handled by their rule-filter delta alone.
        equipmentReconciliationNeeded |= !equipQueue.empty();
        equipQueue.clear();

        logger::debug(" [Regras aplicadas] {} (Save #{}) - Regra: '{}', Versão: {}, Grupos Aplicados: {}",
			actorName, session.saveNumber, currentRule.name, state.version, fmt::join(state.appliedGroups, ", "));
        // --- LOG DE FINALIZAÇÃO ---
        logger::debug("[ApplyRules] FINALIZADO com sucesso para {} ({:08X})", actorName, actorID);
    }
    }

    // Também recompõe quando nenhuma regra da fase correspondeu (por
    // exemplo, após sair de uma Cell/Location e remover o outfit anterior).
    if (!isPlayer &&
        (equipmentReconciliationNeeded ||
         ShouldReconcileEquipmentForDelta(a_actor, a_delta))) {
        ReconcileEquipmentContext(a_actor);
    }
    ReconcileActorValueLedger(a_actor, actorRuleStates);
}

void ApplyRulesToInstance(
    RE::Actor* a_actor,
    const RuleEvaluationDelta& a_delta,
    int a_forcedLevel)
{
    if (!a_actor ||
        g_ruleEvaluationSuspended.load(std::memory_order_acquire)) {
        return;
    }

    const auto buildSignature = [a_actor]() {
        std::size_t seed = 0;
        const auto combine = [&seed](const auto& a_value) {
            const auto valueHash = std::hash<std::decay_t<decltype(a_value)>>{}(a_value);
            seed ^= valueHash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };

        const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
        const auto& session = SaveStateManager::GetSingleton()->GetSessionData();
        if (const auto npcRules = session.npcRuleVersions.find(npcKey);
            npcRules != session.npcRuleVersions.end()) {
            for (const auto& [ruleID, state] : npcRules->second) {
                combine(ruleID);
                combine(state.version);
                combine(state.isActive);
                combine(state.canRerollOnNextActivation);
                for (const auto& rewardKey : state.activeRewardKeys) {
                    combine(rewardKey);
                }
                for (const auto& [rewardKey, numeric] :
                     state.numericRewardStates) {
                    combine(rewardKey);
                    combine(numeric.appliedDelta);
                }
            }
        }
        if (const auto keywords = session.virtualKeywords.find(npcKey);
            keywords != session.virtualKeywords.end()) {
            for (const auto& keyword : keywords->second) {
                combine(keyword);
            }
        }
        if (const auto factions = session.addedFactions.find(npcKey);
            factions != session.addedFactions.end()) {
            for (const auto& [faction, rank] : factions->second) {
                combine(faction);
                combine(rank);
            }
        }
        if (auto cell = a_actor->GetParentCell()) {
            combine(cell->GetFormID());
        }
        if (auto location = a_actor->GetCurrentLocation()) {
            combine(location->GetFormID());
        }
        combine(IsActorInCombatContext(a_actor));
        combine(IsActivePlayerFollower(a_actor));
        const auto inventory = a_actor->GetInventory();
        for (const auto& [item, data] : inventory) {
            if (!item || data.first <= 0) {
                continue;
            }
            combine(item->GetFormID());
            combine(data.first);
            const bool worn = data.second && data.second->IsWorn();
            combine(worn);
        }
        return seed;
    };

    RuleEvaluationDelta pending = a_delta;
    pending.Merge(
        RuleManager::GetSingleton()->DetectBaseNPCChanges(a_actor));
    pending.Merge(
        RuleManager::GetSingleton()->DetectActorValueChanges(a_actor));
    pending.Merge(
        RuleManager::GetSingleton()->DetectFollowerStateChanges(a_actor));
    std::set<std::size_t> visitedStates;
    visitedStates.insert(buildSignature());
    constexpr std::size_t kMaxCascadePasses = 32;

    for (std::size_t pass = 0; pass < kMaxCascadePasses; ++pass) {
        RuleEvaluationDelta produced;
        produced.mask = ToMask(RuleDependency::kNone);

        const auto previousActor = g_activeEvaluationActor;
        auto* previousCollector = g_activeProducedDelta;
        g_activeEvaluationActor = a_actor->GetFormID();
        g_activeProducedDelta = std::addressof(produced);
        ApplyRulesToInstancePass(a_actor, pending, a_forcedLevel);
        g_activeEvaluationActor = previousActor;
        g_activeProducedDelta = previousCollector;
        produced.Merge(
            RuleManager::GetSingleton()->DetectActorValueChanges(a_actor));
        produced.Merge(
            RuleManager::GetSingleton()->DetectFollowerStateChanges(a_actor));

        if (produced.mask == ToMask(RuleDependency::kNone)) {
            return;
        }

        const auto signature = buildSignature();
        if (!visitedStates.insert(signature).second) {
            logger::error(
                "[RuleCascade] Cycle detected for '{}' ({:08X}) after {} passes; "
                "the repeated state was stopped before another pass.",
                a_actor->GetName(), a_actor->GetFormID(), pass + 1);
            return;
        }

        pending = std::move(produced);
    }

    logger::error(
        "[RuleCascade] Safety limit of {} passes reached for '{}' ({:08X}); "
        "remaining nested deltas were discarded.",
        kMaxCascadePasses, a_actor->GetName(), a_actor->GetFormID());
}

void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel)
{
    ApplyRulesToInstance(a_actor, RuleEvaluationDelta::Full(), a_forcedLevel);
}

bool ResetRuleActivationForActor(
    RE::Actor* actor,
    const std::string_view ruleID)
{
    if (!actor || actor->IsDead() ||
        !SaveStateManager::GetSingleton()->GetCurrentContext().isValid) {
        return false;
    }

    auto& rulesByID = SaveStateManager::GetSingleton()->
        GetSessionData().npcRuleVersions[
            SaveStateManager::BuildNPCKey(actor)];
    bool found = ruleID.empty();
    for (auto& [appliedRuleID, state] : rulesByID) {
        if (!ruleID.empty() && appliedRuleID != ruleID) {
            continue;
        }
        found = true;
        QueueRemoveActorValueRule(actor, appliedRuleID);
        QueueRemoveActorScaleRule(actor, appliedRuleID);
        const Rule* definition = RuleManager::GetSingleton()->
            GetRuleVersion(appliedRuleID, state.version);
        if (!definition) {
            definition = RuleManager::GetSingleton()->
                FindRule(appliedRuleID);
        }
        if (definition &&
            (!state.activationStateKnown || state.isActive)) {
            RemoveAppliedActivationRewards(actor, *definition, state);
        }
        for (const auto& [rewardKey, numeric] :
             state.numericRewardStates) {
            (void)rewardKey;
            const auto target = ResolveActorValue(
                numeric.targetActorValueName);
            if (target != RE::ActorValue::kNone) {
                actor->AsActorValueOwner()->ModActorValue(
                    RE::ACTOR_VALUE_MODIFIERS::kPermanent,
                    target,
                    -numeric.appliedDelta);
            }
        }
        state.numericRewardStates.clear();
        state.persistentRewardKeys.clear();
        state.activationStateKnown = true;
        state.isActive = false;
        state.runtimeActivationToken = 0;
        state.canRerollOnNextActivation = true;
        state.activeRewardKeys.clear();
        state.appliedGroups.clear();
    }
    if (!found) {
        return false;
    }
    ApplyRulesToInstance(actor, RuleEvaluationDelta::Full());
    return true;
}
