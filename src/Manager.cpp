#include "Manager.h"
#include "SaveState.h"

namespace
{
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
}

void Manager::PopulateAllLists(bool forceRefresh) {
    if (_isPopulated && !forceRefresh) return;

    logger::info("Iniciando escaneamento de FormTypes{}...", forceRefresh ? " (refresh forçado)" : "");

    PopulateList<RE::BGSKeyword>("Keyword");
    PopulateList<RE::TESGlobal>("Global");
    PopulateList<RE::TESFaction>("Faction");
    auto& factionRankList = _dataStore["Faction Rank"];
    factionRankList.clear();
    for (auto faction : _dataStore["Faction"]) {
        faction.formType = "Faction Rank";
        factionRankList.push_back(std::move(faction));
    }
    RebuildEditorIDIndex("Faction Rank");
    logger::debug("Carregados {} itens do tipo {}", factionRankList.size(), "Faction Rank");
    PopulateList<RE::TESRace>("Race");
    PopulateList<RE::BGSPerk>("Perk");
    PopulateList<RE::SpellItem>("Spell");
    PopulateList<RE::TESShout>("Shout");
    PopulateList<RE::TESNPC>("NPC");
    PopulateList<RE::TESObjectWEAP>("Weapon");
    PopulateList<RE::TESObjectARMO>("Armor");
    PopulateList<RE::TESObjectARMO>("Skin");
    PopulateList<RE::BGSOutfit>("Outfit");


    // --- NOVOS TIPOS ADICIONADOS ---
    PopulateList<RE::AlchemyItem>("Potion");
    PopulateList<RE::IngredientItem>("Ingredient");
    PopulateList<RE::ScrollItem>("Scroll");
    PopulateList<RE::TESObjectBOOK>("Book");
    PopulateList<RE::TESAmmo>("Ammo");
    PopulateList<RE::TESObjectLIGH>(
        "Light",
        [](RE::TESObjectLIGH* light) {
            return light && light->CanBeCarried();
        });
    PopulateList<RE::TESLevItem>(
        "Leveled Item",
        [](RE::TESLevItem* list) {
            return list && !list->entries.empty();
        });
    PopulateList<RE::TESObjectMISC>("Misc");
    auto& goldList = _dataStore["Gold"];
    goldList.clear();
    for (auto gold : _dataStore["Misc"]) {
        if (gold.formID == 0xF || gold.editorID == "Gold001") {
            gold.formType = "Gold";
            goldList.push_back(std::move(gold));
        }
    }
    RebuildEditorIDIndex("Gold");
    logger::debug("Carregados {} itens do tipo {}", goldList.size(), "Gold");
    PopulateList<RE::TESSoulGem>("SoulGem");
    PopulateList<RE::TESKey>("Key");
    auto& inventoryItems = _dataStore["Inventory Item"];
    inventoryItems.clear();
    for (const auto& typeName : { "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Light", "Misc", "SoulGem", "Key" }) {
        const auto& source = _dataStore[typeName];
        for (auto item : source) {
            item.formType = "Inventory Item";
            inventoryItems.push_back(std::move(item));
        }
    }
    RebuildEditorIDIndex("Inventory Item");
    logger::debug("Carregados {} itens do tipo {}", inventoryItems.size(), "Inventory Item");
    auto& inventoryCountItems = _dataStore["Inventory Count"];
    inventoryCountItems = inventoryItems;
    for (auto& item : inventoryCountItems) {
        item.formType = "Inventory Count";
    }
    RebuildEditorIDIndex("Inventory Count");
    logger::debug("Carregados {} itens do tipo {}", inventoryCountItems.size(), "Inventory Count");
    auto& equippedItems = _dataStore["Equipped Item"];
    equippedItems.clear();
    for (const auto& typeName : { "Weapon", "Armor", "Ammo", "Light" }) {
        const auto& source = _dataStore[typeName];
        for (auto item : source) {
            item.formType = "Equipped Item";
            equippedItems.push_back(std::move(item));
        }
    }
    RebuildEditorIDIndex("Equipped Item");
    logger::debug("Carregados {} itens do tipo {}", equippedItems.size(), "Equipped Item");
    // - v.1.2.0
    PopulateList<RE::TESCombatStyle>("Combat Style");
    PopulateList<RE::BGSVoiceType>("Voice Type");
    PopulateList<RE::TESClass>("Class");
    PopulateList<RE::BGSLocation>("Location");
    PopulateList<RE::TESQuest>("Quest");
    PopulateList<RE::TESWorldSpace>("Worldspace");
    PopulateCellList();
    PopulateList<RE::BGSHeadPart>("HeadPart Misc", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kMisc;
        });
    PopulateList<RE::BGSHeadPart>("HeadPart Face", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kFace;
        });
    PopulateList<RE::BGSHeadPart>("HeadPart Eyes", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kEyes;
        });
    PopulateList<RE::BGSHeadPart>("Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kHair;
        });
    PopulateList<RE::BGSHeadPart>("Facial Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kFacialHair;
        });
    PopulateList<RE::BGSHeadPart>("HeadPart Scar", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kScar;
        });
    PopulateList<RE::BGSHeadPart>("HeadPart Eyebrows", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kEyebrows;
        });
    PopulateList<RE::TESLevCharacter>("Leveled NPC", [](RE::TESLevCharacter* lvnc) {
        return lvnc && !lvnc->entries.empty();
        });
    PopulateList<RE::TESPackage>("Package");
    PopulateSpecialFilterLists();

    _isPopulated = true;
    ++_listRevision;
    for (auto cb : _readyCallbacks) {
        if (cb) cb();
    }
    _readyCallbacks.clear();
}

void Manager::RefreshLists(std::string_view a_signatures) {
    const auto includes = [a_signatures](std::string_view a_signature) {
        const auto equalsIgnoreCase = [](std::string_view a_left, std::string_view a_right) {
            if (a_left.size() != a_right.size()) return false;

            for (std::size_t i = 0; i < a_left.size(); ++i) {
                const auto toUpperASCII = [](char a_character) {
                    return a_character >= 'a' && a_character <= 'z' ?
                        static_cast<char>(a_character - ('a' - 'A')) :
                        a_character;
                };
                if (toUpperASCII(a_left[i]) != toUpperASCII(a_right[i])) return false;
            }
            return true;
        };

        std::size_t begin = 0;
        while (begin <= a_signatures.size()) {
            const auto end = a_signatures.find(',', begin);
            auto token = a_signatures.substr(begin, end == std::string_view::npos ? a_signatures.size() - begin : end - begin);
            while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
            while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
            if (equalsIgnoreCase(token, a_signature)) return true;
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return false;
    };

    if (a_signatures.empty() || includes("All")) {
        PopulateAllLists(true);
        return;
    }

    if (includes("KYWD")) PopulateList<RE::BGSKeyword>("Keyword");
    if (includes("GLOB")) PopulateList<RE::TESGlobal>("Global");
    if (includes("FACT")) {
        PopulateList<RE::TESFaction>("Faction");
        auto& factionRanks = _dataStore["Faction Rank"];
        factionRanks.clear();
        for (auto faction : _dataStore["Faction"]) {
            faction.formType = "Faction Rank";
            factionRanks.push_back(std::move(faction));
        }
        RebuildEditorIDIndex("Faction Rank");
    }
    if (includes("PERK")) PopulateList<RE::BGSPerk>("Perk");
    if (includes("SPEL")) PopulateList<RE::SpellItem>("Spell");
    if (includes("SHOU")) PopulateList<RE::TESShout>("Shout");
    if (includes("NPC_")) PopulateList<RE::TESNPC>("NPC");
    if (includes("WEAP")) PopulateList<RE::TESObjectWEAP>("Weapon");
    if (includes("ARMO")) {
        PopulateList<RE::TESObjectARMO>("Armor");
        PopulateList<RE::TESObjectARMO>("Skin");
    }
    if (includes("OTFT")) PopulateList<RE::BGSOutfit>("Outfit");
    if (includes("ALCH")) PopulateList<RE::AlchemyItem>("Potion");
    if (includes("INGR")) PopulateList<RE::IngredientItem>("Ingredient");
    if (includes("SCRL")) PopulateList<RE::ScrollItem>("Scroll");
    if (includes("BOOK")) PopulateList<RE::TESObjectBOOK>("Book");
    if (includes("AMMO")) PopulateList<RE::TESAmmo>("Ammo");
    if (includes("LIGH") || includes("Light")) {
        PopulateList<RE::TESObjectLIGH>(
            "Light",
            [](RE::TESObjectLIGH* light) {
                return light && light->CanBeCarried();
            });
    }
    if (includes("LVLI") || includes("Leveled Item")) {
        PopulateList<RE::TESLevItem>(
            "Leveled Item",
            [](RE::TESLevItem* list) {
                return list && !list->entries.empty();
            });
    }
    if (includes("MISC")) {
        PopulateList<RE::TESObjectMISC>("Misc");
        auto& gold = _dataStore["Gold"];
        gold.clear();
        for (auto item : _dataStore["Misc"]) {
            if (item.formID == 0xF || item.editorID == "Gold001") {
                item.formType = "Gold";
                gold.push_back(std::move(item));
            }
        }
        RebuildEditorIDIndex("Gold");
    }
    if (includes("SLGM")) PopulateList<RE::TESSoulGem>("SoulGem");
    if (includes("KEYM")) PopulateList<RE::TESKey>("Key");
    if (includes("CSTY")) PopulateList<RE::TESCombatStyle>("Combat Style");
    if (includes("VTYP")) PopulateList<RE::BGSVoiceType>("Voice Type");
    if (includes("CLAS")) PopulateList<RE::TESClass>("Class");
    if (includes("LCTN") || includes("Location")) PopulateList<RE::BGSLocation>("Location");
    if (includes("QUST") || includes("Quest")) PopulateList<RE::TESQuest>("Quest");
    if (includes("WRLD") || includes("Worldspace")) PopulateList<RE::TESWorldSpace>("Worldspace");
    if (includes("CELL") || includes("Cell")) PopulateCellList();
    if (includes("HDPT")) {
        PopulateList<RE::BGSHeadPart>("HeadPart Misc", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kMisc; });
        PopulateList<RE::BGSHeadPart>("HeadPart Face", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kFace; });
        PopulateList<RE::BGSHeadPart>("HeadPart Eyes", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kEyes; });
        PopulateList<RE::BGSHeadPart>("Hair", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kHair; });
        PopulateList<RE::BGSHeadPart>("Facial Hair", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kFacialHair; });
        PopulateList<RE::BGSHeadPart>("HeadPart Scar", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kScar; });
        PopulateList<RE::BGSHeadPart>("HeadPart Eyebrows", [](RE::BGSHeadPart* hp) { return hp->type == RE::BGSHeadPart::HeadPartType::kEyebrows; });
    }
    if (includes("LVLN")) {
        PopulateList<RE::TESLevCharacter>("Leveled NPC", [](RE::TESLevCharacter* lvnc) { return lvnc && !lvnc->entries.empty(); });
    }

    if (includes("NPC_") || includes("KYWD") || includes("All")) {
        PopulateSpecialFilterLists();
    }

    const bool inventoryChanged = includes("WEAP") || includes("ARMO") || includes("ALCH") || includes("INGR") ||
        includes("SCRL") || includes("BOOK") || includes("AMMO") || includes("LIGH") || includes("Light") ||
        includes("MISC") || includes("SLGM") || includes("KEYM");
    if (inventoryChanged) {
        auto& inventory = _dataStore["Inventory Item"];
        inventory.clear();
        for (const auto& typeName : { "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Light", "Misc", "SoulGem", "Key" }) {
            for (auto item : _dataStore[typeName]) {
                item.formType = "Inventory Item";
                inventory.push_back(std::move(item));
            }
        }
        auto& inventoryCount = _dataStore["Inventory Count"];
        inventoryCount = inventory;
        for (auto& item : inventoryCount) item.formType = "Inventory Count";
        RebuildEditorIDIndex("Inventory Item");
        RebuildEditorIDIndex("Inventory Count");
    }

    if (includes("WEAP") || includes("ARMO") || includes("AMMO") ||
        includes("LIGH") || includes("Light")) {
        auto& equipped = _dataStore["Equipped Item"];
        equipped.clear();
        for (const auto& typeName : { "Weapon", "Armor", "Ammo", "Light" }) {
            for (auto item : _dataStore[typeName]) {
                item.formType = "Equipped Item";
                equipped.push_back(std::move(item));
            }
        }
        RebuildEditorIDIndex("Equipped Item");
    }

    ++_listRevision;
}

void Manager::PopulateSpecialFilterLists()
{
    auto& locationKeywords = _dataStore["Location Keyword"];
    locationKeywords = _dataStore["Keyword"];
    for (auto& keyword : locationKeywords) {
        keyword.formType = "Location Keyword";
    }
    RebuildEditorIDIndex("Location Keyword");

    auto& sourcePlugins = _dataStore["Source Plugin"];
    sourcePlugins.clear();
    std::set<std::string> pluginNames;
    for (const auto& npc : _dataStore["NPC"]) {
        if (!npc.pluginName.empty()) {
            pluginNames.insert(npc.pluginName);
        }
    }
    RE::FormID pseudoID = 1;
    for (const auto& pluginName : pluginNames) {
        sourcePlugins.push_back({
            pseudoID++,
            pluginName,
            pluginName,
            pluginName,
            "Source Plugin"
        });
    }

    const auto setPseudoList = [&](const std::string& type,
        const std::initializer_list<std::pair<const char*, const char*>> entries) {
        auto& list = _dataStore[type];
        list.clear();
        RE::FormID id = 1;
        for (const auto& [editorID, name] : entries) {
            list.push_back({
                id++,
                editorID,
                name,
                "EDF",
                type
            });
        }
    };
    setPseudoList("NPC Trait", {
        { "Unique", "Unique" },
        { "Essential", "Essential" },
        { "Protected", "Protected" }
    });
    setPseudoList("Relationship Rank", {
        { "Player", "Relationship to Player" }
    });
    setPseudoList("Cell Type", {
        { "Interior", "Interior" },
        { "Exterior", "Exterior" }
    });
    setPseudoList("City Status", {
        { "InsideCity", "Inside a City" },
        { "OutsideCity", "Outside a City" }
    });
    setPseudoList("Equipped Category", {
        { "Unarmed", "Unarmed" },
        { "AnyWeapon", "Any Weapon" },
        { "OneHanded", "One-Handed Weapon" },
        { "TwoHanded", "Two-Handed Weapon" },
        { "Bow", "Bow" },
        { "Crossbow", "Crossbow" },
        { "Staff", "Staff" },
        { "Shield", "Shield" },
        { "HeavyArmor", "Heavy Armor" },
        { "LightArmor", "Light Armor" },
        { "Clothing", "Clothing" }
    });
}

const std::vector<InternalFormInfo>& Manager::GetList(const std::string& typeName) {
    static std::vector<InternalFormInfo> empty;
    auto it = _dataStore.find(typeName);
    if (it != _dataStore.end()) {
        return it->second;
    }
    return empty;
}

std::string Manager::NormalizeEditorID(const std::string_view editorID)
{
    std::string normalized;
    normalized.reserve(editorID.size());
    for (const auto character : editorID) {
        normalized.push_back(
            character >= 'A' && character <= 'Z' ?
                static_cast<char>(character + ('a' - 'A')) :
                character);
    }
    return normalized;
}

void Manager::RebuildEditorIDIndex(const std::string_view typeName)
{
    const auto type = std::string(typeName);
    auto& index = _formsByEditorID[type];
    index.clear();

    const auto list = _dataStore.find(type);
    if (list == _dataStore.end()) {
        return;
    }

    index.reserve(list->second.size());
    std::size_t collisions = 0;
    for (const auto& info : list->second) {
        if (info.editorID.empty() || info.formID == 0) {
            continue;
        }
        const auto key = NormalizeEditorID(info.editorID);
        const auto [found, inserted] = index.try_emplace(key, info.formID);
        if (!inserted && found->second != info.formID) {
            ++collisions;
        }
    }

    if (collisions > 0) {
        logger::warn(
            "[EditorIDIndex] Type '{}' contains {} duplicate EditorID entries; "
            "the first deterministic entry is authoritative.",
            type, collisions);
    }
}

std::optional<RE::FormID> Manager::FindFormIDByEditorID(
    const std::string_view typeName,
    const std::string_view editorID) const
{
    if (editorID.empty()) {
        return std::nullopt;
    }

    const auto byType = _formsByEditorID.find(std::string(typeName));
    if (byType == _formsByEditorID.end()) {
        return std::nullopt;
    }
    const auto found = byType->second.find(NormalizeEditorID(editorID));
    return found != byType->second.end() ?
        std::optional<RE::FormID>{ found->second } :
        std::nullopt;
}

void Manager::RegisterReadyCallback(std::function<void()> callback) {
    if (_isPopulated) {
        callback();
    } else {
        _readyCallbacks.push_back(callback);
    }
}

std::string Manager::ToUTF8(std::string_view a_str) {
    if (a_str.empty()) return "";

    const auto length = static_cast<int>(a_str.size());

    // Localized and dynamic forms may already provide UTF-8. Passing valid
    // UTF-8 through CP_ACP produces mojibake such as "Р...".
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            a_str.data(),
            length,
            nullptr,
            0) > 0) {
        return std::string(a_str);
    }

    // Preserve support for actual ANSI plugin strings.
    const int wlen = MultiByteToWideChar(
        CP_ACP, 0, a_str.data(), length, nullptr, 0);
    if (wlen <= 0) return std::string(a_str);

    std::wstring wstr(wlen, 0);
    if (MultiByteToWideChar(
            CP_ACP, 0, a_str.data(), length,
            wstr.data(), wlen) <= 0) {
        return std::string(a_str);
    }

    const int u8len = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), wlen,
        nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return std::string(a_str);

    std::string u8str(u8len, 0);
    if (WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), wlen,
            u8str.data(), u8len, nullptr, nullptr) <= 0) {
        return std::string(a_str);
    }

    return u8str;
}

void Manager::PopulateCellList() {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    // TESDataHandler::GetFormArray<TESObjectCELL>() only contains Cells that
    // have been materialized in memory. Read plugin records as the authoritative
    // catalog, then merge runtime/dynamic Cells that are present in the array.
    // Record-scanning approach: ModExplorerMenu/BakaHelpExtender (MIT).
    struct CellRecord {
        std::string editorID;
        std::string pluginName;
    };

    constexpr std::uint32_t kCellRecordType = 0x4C4C4543;  // "CELL" as little-endian uint32
    constexpr std::uint32_t kEditorIDType = 0x44494445;    // "EDID" as little-endian uint32
    constexpr std::uint32_t kMaxEditorIDSize = 64 * 1024;

    std::unordered_map<RE::FormID, CellRecord> records;
    std::size_t scannedFiles = 0;

    const auto scanFile = [&](RE::TESFile* file) {
        if (!file || !file->OpenTES(RE::NiFile::OpenMode::kReadOnly, false)) {
            if (file) {
                logger::warn("[CellCatalog] Não foi possível abrir '{}'.", file->GetFilename());
            }
            return;
        }

        ++scannedFiles;
        try {
            do {
                if (file->currentform.form != kCellRecordType) continue;

                const auto formID = file->GetRuntimeFormID(file->currentform.formID);
                std::string editorID;

                do {
                    if (file->GetCurrentSubRecordType() != kEditorIDType) continue;

                    const auto size = file->GetCurrentSubRecordSize();
                    if (size == 0 || size > kMaxEditorIDSize) break;

                    std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
                    if (file->ReadData(buffer.data(), size)) {
                        const auto end = std::find(buffer.begin(), buffer.begin() + size, '\0');
                        editorID.assign(buffer.begin(), end);
                    }
                    break;
                } while (file->SeekNextSubrecord());

                // Os plugins são percorridos em load order. A primeira
                // ocorrência conserva o arquivo proprietário do FormID quando
                // plugins posteriores apenas sobrescrevem o mesmo record.
                records.try_emplace(formID, CellRecord{
                    ToUTF8(editorID),
                    ToUTF8(file->GetFilename())
                });
            } while (file->SeekNextForm(true));
        }
        catch (const std::exception& e) {
            logger::error("[CellCatalog] Falha lendo '{}': {}", file->GetFilename(), e.what());
        }
        catch (...) {
            logger::error("[CellCatalog] Falha desconhecida lendo '{}'.", file->GetFilename());
        }

        if (!file->CloseTES(false)) {
            logger::warn("[CellCatalog] Não foi possível fechar '{}'.", file->GetFilename());
        }
    };

    for (std::uint8_t i = 0; i < dataHandler->GetLoadedModCount(); ++i) {
        scanFile(dataHandler->GetLoadedMods()[i]);
    }
    for (std::uint16_t i = 0; i < dataHandler->GetLoadedLightModCount(); ++i) {
        scanFile(dataHandler->GetLoadedLightMods()[i]);
    }

    auto& list = _dataStore["Cell"];
    list.clear();
    list.reserve(records.size());

    std::unordered_map<RE::FormID, std::size_t> indices;
    indices.reserve(records.size());

    for (const auto& [formID, record] : records) {
        // Cells sem EDID não são úteis no seletor e tornam a lista de
        // wilderness excessivamente grande.
        if (record.editorID.empty()) continue;

        InternalFormInfo info;
        info.formID = formID;
        info.editorID = record.editorID;
        info.pluginName = record.pluginName;
        info.formType = "Cell";

        if (auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(formID)) {
            if (auto fullName = cell->As<RE::TESFullName>()) {
                info.name = ToUTF8(fullName->fullName.c_str());
            }
        }

        indices.emplace(formID, list.size());
        list.push_back(std::move(info));
    }

    std::size_t runtimeCellsAdded = 0;
    for (auto* cell : dataHandler->GetFormArray<RE::TESObjectCELL>()) {
        if (!cell) continue;

        try {
            const auto formID = cell->GetFormID();
            if (auto found = indices.find(formID); found != indices.end()) {
                auto& info = list[found->second];
                if (info.name.empty()) {
                    if (auto fullName = cell->As<RE::TESFullName>()) {
                        info.name = ToUTF8(fullName->fullName.c_str());
                    }
                }
                continue;
            }

            InternalFormInfo info;
            info.formID = formID;
            info.formType = "Cell";
            info.pluginName = "Dynamic";
            if (auto file = GetSourceFileByFormID(cell)) {
                info.pluginName = ToUTF8(file->GetFilename());
            }
            info.editorID = ToUTF8(clib_util::editorID::get_editorID(cell));
            if (auto fullName = cell->As<RE::TESFullName>()) {
                info.name = ToUTF8(fullName->fullName.c_str());
            }

            indices.emplace(formID, list.size());
            list.push_back(std::move(info));
            ++runtimeCellsAdded;
        }
        catch (const std::exception& e) {
            logger::error("[CellCatalog] Falha processando Cell runtime {:08X}: {}",
                cell->GetFormID(), e.what());
        }
        catch (...) {
            logger::error("[CellCatalog] Falha desconhecida processando Cell runtime {:08X}.",
                cell->GetFormID());
        }
    }

    std::ranges::sort(list, [](const InternalFormInfo& left, const InternalFormInfo& right) {
        return std::tie(left.pluginName, left.editorID, left.formID) <
            std::tie(right.pluginName, right.editorID, right.formID);
    });
    RebuildEditorIDIndex("Cell");

    logger::info("[CellCatalog] Carregadas {} Cells de {} plugins ({} adicionadas da memória/runtime).",
        list.size(), scannedFiles, runtimeCellsAdded);
}

template <typename T>
void Manager::PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    auto& list = _dataStore[a_typeName];
    list.clear();

    const auto& forms = dataHandler->GetFormArray<T>();
    list.reserve(forms.size());

    for (const auto& form : forms) {
        if (!form) continue;

        if (a_filter && !a_filter(form)) {
            continue;
        }
        // VariÃ¡veis de auxÃ­lio para o log de erro caso o catch seja acionado
        RE::FormID currentID = 0;
        std::string currentPlugin = "Unknown";

        try {
            currentID = form->GetFormID();

            // ObtÃ©m o nome do plugin de origem antes de qualquer processamento complexo
            if (auto file = GetSourceFileByFormID(form)) {
                currentPlugin = std::string(file->GetFilename());
            }
            else {
                currentPlugin = "Dynamic";
            }

            InternalFormInfo info;
            info.formID = currentID;
            info.formType = a_typeName;
            info.pluginName = ToUTF8(currentPlugin);
            if (auto* boundObject =
                    form->template As<RE::TESBoundObject>()) {
                info.playable = boundObject->GetPlayable();
            }

            // EditorID: clib_util pode lanÃ§ar exceÃ§Ãµes em contextos raros de memÃ³ria
            std::string rawEditorID = clib_util::editorID::get_editorID(form);
            info.editorID = ToUTF8(rawEditorID);

            std::string rawName = "";
            if (form->Is(RE::FormType::NPC)) {
                if (auto npc = form->As<RE::TESNPC>()) {
                    rawName = npc->fullName.c_str();
                }
            }
            else if (auto fullName = form->As<RE::TESFullName>()) {
                rawName = fullName->fullName.c_str();
            }

            // A conversÃ£o UTF-8 Ã© um ponto comum de falha se a string estiver corrompida
            info.name = ToUTF8(rawName);

            list.push_back(info);
        }
        catch (const std::exception& e) {
            // Log detalhado com FormID em Hexadecimal e o erro especÃ­fico
            logger::error("[PopulateList] Critical error on item {:08X} of plugin '{}' (Type: {}). Error: {}",
                currentID, currentPlugin, a_typeName, e.what());
        }
        catch (...) {
            // Captura erros desconhecidos que nÃ£o herdam de std::exception
            logger::error("[PopulateList] Uknown error on item {:08X} of plugin '{}' (Type: {})",
                currentID, currentPlugin, a_typeName);
        }
    }
    RebuildEditorIDIndex(a_typeName);
    logger::debug("Carregados {} itens do tipo {}", list.size(), a_typeName);
}

void Manager::ConvertAllNPCOutfitsToInventory() {
    auto settings = NPCSettings::GetSingleton();
    if (settings->outfitMode == OutfitConversionMode::kDisabled) {
        logger::info("[Outfit] ConversÃ£o desativada nas configuraÃ§Ãµes.");
        return;
    }
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    // ObtÃ©m todos os NPCs carregados no jogo
    const auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();
    RE::BGSOutfit* rdoEmptyOutfit = dataHandler->LookupForm<RE::BGSOutfit>(0x800, "RDO.esp");
    uint32_t count = 0;
    for (auto* npc : npcArray) {
        // Verifica se o NPC existe e se possui um Outfit padrÃ£o (DOFT)
        if (npc && npc->defaultOutfit) {
            if (settings->outfitMode == OutfitConversionMode::kFullConversion) {
                RE::BGSOutfit* outfit = npc->defaultOutfit;
                for (auto* item : outfit->outfitItems) {
                    if (item) {
                        auto* boundItem = item->As<RE::TESBoundObject>();
                        if (boundItem && npc->GetObjectCount(boundItem) == 0) {
                            npc->AddObjectToContainer(boundItem, 1, npc);
                        }
                    }
                }
            }

            npc->SetDefaultOutfit(rdoEmptyOutfit);

            // npc->sleepOutfit = nullptr;

            count++;
        }
    }
    logger::info("Processados {} NPCs: Outfits convertidos em itens de inventÃ¡rio.", count);
}
