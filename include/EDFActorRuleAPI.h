#pragma once
#include "EDFAPI.h"

// Separate extension: the existing EDF rule API ABI is unchanged.
namespace EDF::ActorRules
{
    struct Request
    {
        std::uint32_t structSize{ sizeof(Request) };
        const char* requester{};
        const char* actorKey{};
        std::uint32_t actorFormID{};
        const std::uint32_t* perks{};
        std::uint32_t perkCount{};
    };
    struct Interface
    {
        std::uint32_t version{ 1 };
        // Replaces this owner's session-bound rule; does not reevaluate.
        bool (*QueueSync)(const Request*, API::Callback, void*) noexcept;
    };
    inline Interface* GetAPI()
    {
        auto module = GetModuleHandleA("EDF.dll");
        auto getter = module ? reinterpret_cast<Interface* (*)()>(
            GetProcAddress(module, "GetEDFActorRuleAPI")) : nullptr;
        auto api = getter ? getter() : nullptr;
        return api && api->version == 1 ? api : nullptr;
    }
}
