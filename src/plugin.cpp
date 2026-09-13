#include "logger.h"
#include "hooks.h"
#include "UI.h"
#include "Events.h"
#include "Manager.h"
#include "Rule.h"
#include "DistributionCore/Domain.h"
#include "EDFAPI.h"
#include "WhoEditThatAPI.h"

namespace {
    bool hasDFG = false;
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kPostLoad) {
        hasDFG = GetModuleHandleA("DynamicFormsGenerator.dll") != nullptr;
        if (hasDFG) {
            logger::info("DynamicFormsGenerator.dll found.");
        }
    }

    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
		if (WhoEditThat::API::GetAPI()) {
			logger::info("WhoEditThat API v1 found; Actor Value rewards use the shared ledger.");
		}
		else {
			logger::error("WhoEditThat API v1 is required for Actor Value rewards.");
		}
		Hooks::Install();
        // Init Rules
        RuleManager::GetSingleton()->LoadRules();
        if(!hasDFG) {
            Manager::GetSingleton()->PopulateAllLists();
            RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
		}
		//Manager::GetSingleton()->ConvertAllNPCOutfitsToInventory();
       // CellAttachHandler::Register();
        //CellFullyLoadedHandler::Register();
        LoadEventHandler::Register();
        PersistentItemTransferHandler::Register();
        EquipmentEventHandler::Register();
        QuestEventHandler::Register();
        PlayerLevel::Register();
        CombatEventHandler::Register();
        FollowerDialogueEventHandler::Register();
        //auto ui = RE::UI::GetSingleton();
        //if (ui) {
        //    ui->AddEventSink(EventSink::GetSingleton());
        //    // SKSE::log::info("EventSink de Menu registrado com sucesso.");
        //}

        //if (QuickLoot::API::QuickLootAPI::Init("ProgressNPC")) {
        //    logger::info("QuickLoot API conectada com sucesso.");

        //    // Registrar handlers usando as funções que criamos no Events.cpp
        //    QuickLoot::API::QuickLootAPI::RegisterOpenLootMenuHandler(EventSink::OnQuickLootOpen);
        //    QuickLoot::API::QuickLootAPI::RegisterCloseLootMenuHandler(EventSink::OnQuickLootClose);
        //}
        //else {
        //    logger::warn("QuickLootIE não detectado ou falha na API.");
        //}

        LocationChangeHandler::Register();
        ActorCellChangeHandler::Register();
        SPIDUI::Register();
        EDF::API::SetReady(true);
    }
    if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
        SuspendRuleEvaluationForLoad();
        const char* saveName = static_cast<const char*>(message->data);
        if (!saveName) {
            logger::warn("PreLoadGame: Nome do save é nulo.");
            return;
        }

        logger::info("Iniciando pré-carregamento para o save: {}", saveName);

        RE::BGSSaveLoadFileEntry tempEntry{};
        tempEntry.fileName = saveName;

        bool success = false;
        // Tentamos ler o cabeçalho para obter o CharID real do save
        for (int i = 0; i < 5; ++i) {
            if (tempEntry.PopulateFileEntryData()) {
                success = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (success) {
            uint32_t charID = 0;
            std::string fileName = saveName;

            // Tentativa de extrair do nome do arquivo (mais rápido/seguro para CharID de pasta)
            try {
                size_t firstUnderscore = fileName.find('_');
                size_t secondUnderscore = fileName.find('_', firstUnderscore + 1);
                if (firstUnderscore != std::string::npos && secondUnderscore != std::string::npos) {
                    std::string charIdStr = fileName.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1);
                    charID = static_cast<uint32_t>(std::stoul(charIdStr, nullptr, 16));
                }
                else {
                    charID = tempEntry.characterID;
                }
            }
            catch (...) {
                charID = tempEntry.characterID;
            }
            // Isso minimiza o tempo em que o isValid fica false durante a transição
            SaveStateManager::GetSingleton()->SetCurrentContext(
                charID, tempEntry.saveNumber, saveName);
            logger::info("Contexto carregado: Personagem {:X}, Save {}", charID, tempEntry.saveNumber);
        }
        else {
            // Se falhou ao ler o save, limpamos o contexto por segurança
            SaveStateManager::GetSingleton()->ClearContext();
            logger::error("Falha ao ler cabeçalho. Contexto invalidado.");
        }
    }
    if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
        ResumeRuleEvaluationAfterLoad();
    }
    if (message->type == SKSE::MessagingInterface::kSaveGame) {
        // O data do kSaveGame contém o nome do arquivo de save
        const char* saveName = static_cast<const char*>(message->data);

        if(saveName) {
            logger::info("[Plugin] Mensagem kSaveGame recebida: {}", saveName);
            SaveStateManager::GetSingleton()->PersistCurrentSave(saveName);
        }
    }


    if (message->type == SKSE::MessagingInterface::kNewGame) {
        logger::info("[Plugin] New Game detectado. Inicializando contexto padrão");
        // CharID 0 e Save 0 representam uma sessão nova sem persistência de disco ainda.
        SaveStateManager::GetSingleton()->SetCurrentContext(0, 0);
        ResumeRuleEvaluationAfterLoad();

    }
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    SKSE::Init(skse);
    DistributionCore::RegisterBuiltInTypes();
    DynamicFormsGeneratorListener::GetSingleton()->Register();
    BackgroundCloneHook::Install();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
