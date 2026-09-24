#pragma once

#include <Windows.h>
#include <cstdint>
#include <limits>

namespace EDF::API
{
    inline constexpr std::uint32_t kInterfaceVersion = 1;
    inline constexpr std::int32_t kAnyVersion = -1;

    enum class Operation : std::uint32_t
    {
        kCreate = 1,
        kUpdate = 2,
        kDelete = 3,
        kLookup = 4,
        kReevaluateActor = 5,
        kResetActor = 6
    };

    enum class Status : std::uint32_t
    {
        kSuccess = 0,
        kNotReady,
        kInvalidArgument,
        kInvalidJson,
        kValidationFailed,
        kNotFound,
        kNotOwner,
        kVersionConflict,
        kPersistenceFailed,
        kActorUnavailable,
        kInternalError
    };

    struct CreateRuleRequest
    {
        std::uint32_t structSize{ sizeof(CreateRuleRequest) };
        const char* requester = nullptr;
        const char* ruleJson = nullptr;
    };

    struct UpdateRuleRequest
    {
        std::uint32_t structSize{ sizeof(UpdateRuleRequest) };
        const char* requester = nullptr;
        const char* ruleID = nullptr;
        std::int32_t expectedVersion = kAnyVersion;
        const char* ruleJson = nullptr;
    };

    struct DeleteRuleRequest
    {
        std::uint32_t structSize{ sizeof(DeleteRuleRequest) };
        const char* requester = nullptr;
        const char* ruleID = nullptr;
        std::int32_t expectedVersion = kAnyVersion;
    };

    struct LookupRuleRequest
    {
        std::uint32_t structSize{ sizeof(LookupRuleRequest) };
        const char* requester = nullptr;
        const char* ruleID = nullptr;
    };

    struct ActorRequest
    {
        std::uint32_t structSize{ sizeof(ActorRequest) };
        const char* requester = nullptr;
        std::uint32_t actorFormID = 0;
        // Optional. QueueReevaluateActor uses it to verify that the exact rule became active;
        // QueueResetActor uses it to select a rule. Empty reset targets every active rule.
        const char* ruleID = nullptr;
    };

    struct Result
    {
        std::uint32_t structSize{ sizeof(Result) };
        Operation operation{ Operation::kLookup };
        Status status{ Status::kInternalError };
        std::uint32_t actorFormID = 0;
        std::int32_t version = 0;
        char ruleID[64]{};
        char error[256]{};
        // Valid only for the duration of the callback.
        const char* ruleJson = nullptr;
        std::uint32_t ruleJsonLength = 0;
    };

    using Callback = void (*)(const Result*, void* userData);

    class IEDFRuleAPI
    {
    public:
        virtual ~IEDFRuleAPI() = default;
        virtual std::uint32_t GetVersion() const noexcept = 0;
        virtual bool IsReady() const noexcept = 0;
        virtual bool QueueCreateRule(
            const CreateRuleRequest*, Callback, void*) noexcept = 0;
        virtual bool QueueUpdateRule(
            const UpdateRuleRequest*, Callback, void*) noexcept = 0;
        virtual bool QueueDeleteRule(
            const DeleteRuleRequest*, Callback, void*) noexcept = 0;
        virtual bool QueueLookupRule(
            const LookupRuleRequest*, Callback, void*) noexcept = 0;
        virtual bool QueueReevaluateActor(
            const ActorRequest*, Callback, void*) noexcept = 0;
        virtual bool QueueResetActor(
            const ActorRequest*, Callback, void*) noexcept = 0;
    };

    inline IEDFRuleAPI* GetAPI() noexcept
    {
        const auto module = GetModuleHandleA("EDF.dll");
        if (!module) return nullptr;
        using Getter = void* (*)();
        const auto getter = reinterpret_cast<Getter>(
            GetProcAddress(module, "GetEDFRuleAPI"));
        if (!getter) return nullptr;
        auto* api = static_cast<IEDFRuleAPI*>(getter());
        return api && api->GetVersion() == kInterfaceVersion ? api : nullptr;
    }

#ifdef IS_HOST_PLUGIN
    void SetReady(bool ready) noexcept;
#endif
}
