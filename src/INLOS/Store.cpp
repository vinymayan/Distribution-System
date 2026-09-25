#include "INLOS/Store.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <miniz.h>
#include <sqlite3.h>

#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace INLOS
{
    namespace
    {
        constexpr auto kPackagesRoot =
            "Data/Viny Mods/INLOS/Packages";

        struct Database
        {
            sqlite3* handle = nullptr;
            ~Database()
            {
                if (handle) {
                    sqlite3_close(handle);
                }
            }
        };

        struct Statement
        {
            sqlite3_stmt* handle = nullptr;
            ~Statement()
            {
                if (handle) {
                    sqlite3_finalize(handle);
                }
            }
        };

        bool Exec(
            sqlite3* a_db,
            const std::string_view a_sql,
            const std::string_view a_context)
        {
            char* error = nullptr;
            const auto result = sqlite3_exec(
                a_db, a_sql.data(), nullptr, nullptr, &error);
            if (result == SQLITE_OK) {
                return true;
            }
            logger::error(
                "[INLOS Store] {} failed: {}",
                a_context,
                error ? error : sqlite3_errmsg(a_db));
            sqlite3_free(error);
            return false;
        }

        bool Prepare(
            sqlite3* a_db,
            const std::string_view a_sql,
            Statement& a_statement,
            const std::string_view a_context)
        {
            if (sqlite3_prepare_v2(
                    a_db,
                    a_sql.data(),
                    static_cast<int>(a_sql.size()),
                    &a_statement.handle,
                    nullptr) == SQLITE_OK) {
                return true;
            }
            logger::error(
                "[INLOS Store] {} prepare failed: {}",
                a_context,
                sqlite3_errmsg(a_db));
            return false;
        }

        bool EnsureColumn(
            sqlite3* a_db,
            const std::string_view a_table,
            const std::string_view a_column,
            const std::string_view a_definition)
        {
            Statement statement;
            if (!Prepare(a_db,
                    std::format("SELECT 1 FROM pragma_table_info('{}') WHERE name=?1", a_table),
                    statement,
                    "inspect column")) {
                return false;
            }
            sqlite3_bind_text(statement.handle, 1, a_column.data(),
                static_cast<int>(a_column.size()), SQLITE_TRANSIENT);
            if (sqlite3_step(statement.handle) == SQLITE_ROW) {
                return true;
            }
            return Exec(a_db,
                std::format("ALTER TABLE {} ADD COLUMN {} {}", a_table, a_column, a_definition),
                "add column");
        }

        bool OpenDatabase(
            const std::filesystem::path& a_path,
            Database& a_db,
            const bool a_create = true)
        {
            if (sqlite3_open_v2(
                    a_path.string().c_str(),
                    &a_db.handle,
                    SQLITE_OPEN_READWRITE |
                        (a_create ? SQLITE_OPEN_CREATE : 0),
                    nullptr) != SQLITE_OK) {
                logger::error(
                    "[INLOS Store] Could not open '{}': {}",
                    a_path.string(),
                    a_db.handle ? sqlite3_errmsg(a_db.handle) : "unknown");
                return false;
            }
            sqlite3_busy_timeout(a_db.handle, 5000);
            return Exec(a_db.handle, "PRAGMA foreign_keys=ON", "foreign keys") &&
                Exec(a_db.handle, "PRAGMA journal_mode=WAL", "WAL") &&
                Exec(a_db.handle, "PRAGMA synchronous=NORMAL", "synchronous");
        }

        void BindText(
            sqlite3_stmt* a_statement,
            const int a_index,
            const std::string_view a_value)
        {
            sqlite3_bind_text(
                a_statement,
                a_index,
                a_value.data(),
                static_cast<int>(a_value.size()),
                SQLITE_TRANSIENT);
        }

        std::string ColumnText(sqlite3_stmt* a_statement, const int a_column)
        {
            const auto* value = sqlite3_column_text(a_statement, a_column);
            return value ?
                reinterpret_cast<const char*>(value) :
                std::string{};
        }

        std::string GenerateUUID()
        {
            static thread_local std::mt19937_64 generator{
                std::random_device{}()
            };
            std::uniform_int_distribution<std::uint32_t> distribution(
                0, 0xFFFFFFFFu);
            const auto a = distribution(generator);
            const auto b = distribution(generator);
            const auto c = distribution(generator);
            const auto d = distribution(generator);
            return std::format(
                "{:08x}-{:04x}-4{:03x}-{:01x}{:03x}-{:08x}{:04x}",
                a,
                b >> 16,
                b & 0x0FFF,
                8 + ((c >> 28) & 0x3),
                (c >> 16) & 0x0FFF,
                d,
                c & 0xFFFF);
        }

        std::string SanitizeFolder(std::string_view a_name)
        {
            std::string result;
            result.reserve(a_name.size());
            for (const auto character : a_name) {
                if (std::isalnum(static_cast<unsigned char>(character))) {
                    result.push_back(character);
                }
                else if (character == ' ' || character == '-' ||
                         character == '_') {
                    result.push_back('_');
                }
            }
            while (!result.empty() && result.back() == '_') {
                result.pop_back();
            }
            return result.empty() ? "Package" : result;
        }

        std::string RuleFingerprint(const LootRule& a_rule)
        {
            std::ostringstream stream;
            const auto& rule = a_rule.criteria;
            stream << rule.name << '|' << rule.isEnabled << '|' <<
                rule.level << '|' << static_cast<int>(rule.levelComparison) <<
                '|' << rule.maximumLevel << '|' << rule.targetGender << '|' <<
                rule.targetHumanoid << '|' << rule.targetChild << '|' <<
                static_cast<int>(rule.combatState) << '|' <<
                static_cast<int>(rule.followerState) << '|' <<
                static_cast<int>(rule.actorScope) << '|' <<
                static_cast<int>(rule.summonedState) << '|' <<
                static_cast<int>(rule.hostilityState) << '|' <<
                rule.targetRequiresAll << '|' << rule.blacklistedGender << '|' <<
                rule.blacklistedHumanoid << '|' << rule.blacklistedChild << '|' <<
                rule.blacklistRequiresAll << '|' <<
                static_cast<int>(a_rule.trigger) << '|' <<
                static_cast<int>(a_rule.destination) << '|' <<
                a_rule.requirePlayerKiller;
            const auto appendFilters = [&](const auto& a_filters) {
                for (const auto& filter : a_filters) {
                    stream << "|F:" << filter.type << ':' <<
                        filter.formIDStr << ':' << filter.editorID << ':' <<
                        filter.actorValueName << ':' << filter.optionMode << ':' <<
                        filter.optionValue << ':' << filter.optionText << ':' <<
                        static_cast<int>(filter.actorValueMode) << ':' <<
                        static_cast<int>(filter.comparison) << ':' <<
                        filter.minimumValue << ':' << filter.maximumValue << ':' <<
                        filter.isNot;
                }
            };
            appendFilters(rule.targetFilters);
            appendFilters(rule.blacklistFilters);
            for (const auto& group : rule.rewardGroups) {
                stream << "|G:" << group.name << ':' << group.isExclusive <<
                    ':' << group.chanceGroup;
                for (const auto& reward : group.rewards) {
                    stream << "|R:" << reward.typeReward << ':' <<
                        reward.formIDStr << ':' << reward.editorID << ':' <<
                        reward.amount << ':' << reward.chanceReward << ':' <<
                        reward.functionOnType << ':' << reward.isPersistent;
                }
            }
            return std::to_string(
                std::hash<std::string>{}(stream.str()));
        }

        bool ReadManifest(
            const std::filesystem::path& a_path,
            Package& a_package)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file) {
                return false;
            }
            std::stringstream content;
            content << file.rdbuf();
            rapidjson::Document document;
            document.Parse(content.str().c_str());
            if (document.HasParseError() || !document.IsObject()) {
                return false;
            }
            const auto getString = [&](const char* a_name) {
                const auto found = document.FindMember(a_name);
                return found != document.MemberEnd() &&
                    found->value.IsString() ?
                    std::string(found->value.GetString()) :
                    std::string{};
            };
            a_package.id = getString("id");
            a_package.displayName = getString("displayName");
            if (const auto found = document.FindMember("enabled");
                found != document.MemberEnd() && found->value.IsBool()) {
                a_package.enabled = found->value.GetBool();
            }
            if (const auto found = document.FindMember("schemaVersion");
                found != document.MemberEnd() && found->value.IsInt()) {
                a_package.schemaVersion = found->value.GetInt();
            }
            const auto domain = getString("domain");
            return !a_package.id.empty() &&
                !a_package.displayName.empty() &&
                (domain.empty() || domain == "inlos");
        }

        bool WriteManifest(const Package& a_package)
        {
            rapidjson::Document document;
            document.SetObject();
            auto& allocator = document.GetAllocator();
            const auto addString = [&](const char* a_name, const std::string& a_value) {
                rapidjson::Value value;
                value.SetString(
                    a_value.c_str(),
                    static_cast<rapidjson::SizeType>(a_value.size()),
                    allocator);
                document.AddMember(
                    rapidjson::Value(a_name, allocator),
                    value,
                    allocator);
            };
            document.AddMember(
                "schemaVersion", Store::kSchemaVersion, allocator);
            addString("id", a_package.id);
            addString("displayName", a_package.displayName);
            addString("domain", "inlos");
            document.AddMember("enabled", a_package.enabled, allocator);
            addString("database", "package.db");

            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            document.Accept(writer);
            std::ofstream file(
                a_package.path / "manifest.json",
                std::ios::binary | std::ios::trunc);
            if (!file) {
                return false;
            }
            file.write(
                buffer.GetString(),
                static_cast<std::streamsize>(buffer.GetSize()));
            return file.good();
        }

        constexpr auto kCreateSchema = R"sql(
CREATE TABLE IF NOT EXISTS metadata(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS rules(
    rule_id TEXT PRIMARY KEY,
    current_version INTEGER NOT NULL CHECK(current_version >= 0),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS rule_versions(
    rule_id TEXT NOT NULL,
    version INTEGER NOT NULL CHECK(version >= 0),
    name TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),
    level_min INTEGER NOT NULL,
    level_comparison INTEGER NOT NULL CHECK(level_comparison BETWEEN 0 AND 3),
    level_max INTEGER NOT NULL,
    target_gender INTEGER NOT NULL,
    target_humanoid INTEGER NOT NULL,
    target_child INTEGER NOT NULL,
    combat_state INTEGER NOT NULL,
    follower_state INTEGER NOT NULL,
    actor_scope INTEGER NOT NULL,
    summoned_state INTEGER NOT NULL,
    hostility_state INTEGER NOT NULL,
    target_requires_all INTEGER NOT NULL CHECK(target_requires_all IN(0,1)),
    blacklist_gender INTEGER NOT NULL,
    blacklist_humanoid INTEGER NOT NULL,
    blacklist_child INTEGER NOT NULL,
    blacklist_requires_all INTEGER NOT NULL CHECK(blacklist_requires_all IN(0,1)),
    trigger_type INTEGER NOT NULL CHECK(trigger_type BETWEEN 0 AND 2),
    destination INTEGER NOT NULL CHECK(destination BETWEEN 0 AND 1),
    require_player_killer INTEGER NOT NULL CHECK(require_player_killer IN(0,1)),
    PRIMARY KEY(rule_id, version),
    FOREIGN KEY(rule_id) REFERENCES rules(rule_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS rule_filters(
    rule_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    scope INTEGER NOT NULL CHECK(scope IN(0,1)),
    position INTEGER NOT NULL,
    type TEXT NOT NULL,
    form_id TEXT NOT NULL,
    editor_id TEXT NOT NULL,
    actor_value_name TEXT NOT NULL,
    option_mode INTEGER NOT NULL,
    option_value INTEGER NOT NULL,
    option_text TEXT NOT NULL,
    actor_value_mode INTEGER NOT NULL,
    comparison INTEGER NOT NULL,
    minimum_value REAL NOT NULL,
    maximum_value REAL NOT NULL,
    is_not INTEGER NOT NULL DEFAULT 0 CHECK(is_not IN(0,1)),
    PRIMARY KEY(rule_id, version, scope, position),
    FOREIGN KEY(rule_id, version)
        REFERENCES rule_versions(rule_id, version) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS reward_groups(
    rule_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    exclusive_group INTEGER NOT NULL CHECK(exclusive_group IN(0,1)),
    chance REAL NOT NULL,
    PRIMARY KEY(rule_id, version, position),
    FOREIGN KEY(rule_id, version)
        REFERENCES rule_versions(rule_id, version) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS rewards(
    rule_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    group_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    type TEXT NOT NULL,
    form_id TEXT NOT NULL,
    editor_id TEXT NOT NULL,
    amount INTEGER NOT NULL CHECK(amount >= 0),
    chance REAL NOT NULL,
    function_mode INTEGER NOT NULL,
    persistent INTEGER NOT NULL CHECK(persistent IN(0,1)),
    PRIMARY KEY(rule_id, version, group_position, position),
    FOREIGN KEY(rule_id, version, group_position)
        REFERENCES reward_groups(rule_id, version, position)
        ON DELETE CASCADE
);
)sql";
    }

    Store* Store::GetSingleton()
    {
        static Store singleton;
        return std::addressof(singleton);
    }

    bool Store::EnsurePackage(Package& a_package)
    {
        std::error_code error;
        std::filesystem::create_directories(a_package.path, error);
        if (error) {
            logger::error(
                "[INLOS Store] Could not create '{}': {}",
                a_package.path.string(),
                error.message());
            return false;
        }
        Database database;
        if (!OpenDatabase(a_package.path / "package.db", database) ||
            !Exec(database.handle, "BEGIN IMMEDIATE", "begin schema") ||
            !Exec(database.handle, kCreateSchema, "create schema")) {
            return false;
        }
        Statement metadata;
        if (!Prepare(
                database.handle,
                "INSERT OR REPLACE INTO metadata(key,value) VALUES"
                "('package_id',?1),('schema_version','1')",
                metadata,
                "metadata")) {
            Exec(database.handle, "ROLLBACK", "rollback schema");
            return false;
        }
        BindText(metadata.handle, 1, a_package.id);
        if (sqlite3_step(metadata.handle) != SQLITE_DONE ||
            !Exec(database.handle, "PRAGMA user_version=1", "user version") ||
            !Exec(database.handle, "COMMIT", "commit schema")) {
            Exec(database.handle, "ROLLBACK", "rollback schema");
            return false;
        }
        return WriteManifest(a_package);
    }

    bool Store::Load()
    {
        std::scoped_lock lock(_lock);
        _packages.clear();
        _rules.clear();
        _ruleIndices.clear();
        _packagesToDelete.clear();

        std::error_code error;
        const auto root = std::filesystem::path(kPackagesRoot);
        std::filesystem::create_directories(root, error);
        if (error) {
            logger::error(
                "[INLOS Store] Could not create packages root: {}",
                error.message());
            return false;
        }

        for (const auto& entry :
             std::filesystem::directory_iterator(root, error)) {
            if (!entry.is_directory()) {
                continue;
            }
            Package package;
            package.path = entry.path();
            if (ReadManifest(entry.path() / "manifest.json", package) &&
                package.enabled &&
                package.schemaVersion == kSchemaVersion) {
                _packages.push_back(std::move(package));
            }
        }

        std::ranges::sort(
            _packages,
            [](const Package& a_lhs, const Package& a_rhs) {
                return a_lhs.path.string() < a_rhs.path.string();
            });
        std::set<std::string> packageIDs;
        std::erase_if(
            _packages,
            [&](const Package& a_package) {
                if (packageIDs.insert(a_package.id).second) {
                    return false;
                }
                logger::error(
                    "[INLOS Store] Duplicate package ID '{}' at '{}'; "
                    "the deterministic first package is authoritative.",
                    a_package.id,
                    a_package.path.string());
                return true;
            });

        if (std::ranges::none_of(
                _packages,
                [](const Package& a_package) {
                    return a_package.id == kLocalPackageID;
                })) {
            Package local;
            local.id = std::string(kLocalPackageID);
            local.displayName = "Local Rules";
            local.path = root / "Local_Rules";
            if (!EnsurePackage(local)) {
                return false;
            }
            _packages.push_back(std::move(local));
        }

        std::ranges::sort(
            _packages,
            [](const Package& a_lhs, const Package& a_rhs) {
                if (a_lhs.id == kLocalPackageID) return true;
                if (a_rhs.id == kLocalPackageID) return false;
                return a_lhs.id < a_rhs.id;
            });

        std::set<std::string> ruleIDs;
        for (const auto& package : _packages) {
            const auto before = _rules.size();
            if (!LoadPackage(package)) {
                logger::error(
                    "[INLOS Store] Package '{}' was skipped.",
                    package.displayName);
                continue;
            }
            for (auto index = before; index < _rules.size(); ++index) {
                if (!ruleIDs.insert(_rules[index].criteria.id).second) {
                    logger::error(
                        "[INLOS Store] Duplicate rule ID '{}' in package '{}'.",
                        _rules[index].criteria.id,
                        package.id);
                    _rules.erase(_rules.begin() + index);
                    --index;
                }
            }
        }
        RebuildIndices();
        logger::info(
            "[INLOS Store] Loaded {} rules from {} packages.",
            _rules.size(),
            _packages.size());
        return true;
    }

    bool Store::LoadPackage(const Package& a_package)
    {
        const auto databasePath = a_package.path / "package.db";
        if (!std::filesystem::is_regular_file(databasePath)) {
            logger::error(
                "[INLOS Store] Package '{}' has no package.db.",
                a_package.displayName);
            return false;
        }
        Database database;
        if (!OpenDatabase(databasePath, database, false)) {
            return false;
        }
        if (!EnsureColumn(database.handle, "rule_filters", "is_not",
                "INTEGER NOT NULL DEFAULT 0 CHECK(is_not IN(0,1))")) {
            return false;
        }

        Statement metadata;
        if (!Prepare(
                database.handle,
                "SELECT value FROM metadata WHERE key='package_id'",
                metadata,
                "validate package identity") ||
            sqlite3_step(metadata.handle) != SQLITE_ROW ||
            ColumnText(metadata.handle, 0) != a_package.id) {
            logger::error(
                "[INLOS Store] Manifest/database ID mismatch for '{}'.",
                a_package.path.string());
            return false;
        }
        Statement version;
        if (!Prepare(
                database.handle,
                "PRAGMA user_version",
                version,
                "validate schema version") ||
            sqlite3_step(version.handle) != SQLITE_ROW ||
            sqlite3_column_int(version.handle, 0) != kSchemaVersion) {
            logger::error(
                "[INLOS Store] Package '{}' has an unsupported database schema.",
                a_package.displayName);
            return false;
        }

        Statement rules;
        if (!Prepare(
                database.handle,
                "SELECT r.rule_id,r.current_version,v.name,v.enabled,"
                "v.level_min,v.level_comparison,v.level_max,"
                "v.target_gender,v.target_humanoid,v.target_child,"
                "v.combat_state,v.follower_state,v.actor_scope,"
                "v.summoned_state,v.hostility_state,v.target_requires_all,"
                "v.blacklist_gender,v.blacklist_humanoid,v.blacklist_child,"
                "v.blacklist_requires_all,v.trigger_type,v.destination,"
                "v.require_player_killer "
                "FROM rules r JOIN rule_versions v ON "
                "v.rule_id=r.rule_id AND v.version=r.current_version "
                "ORDER BY r.rule_id",
                rules,
                "load rules")) {
            return false;
        }

        while (sqlite3_step(rules.handle) == SQLITE_ROW) {
            LootRule loaded;
            auto& rule = loaded.criteria;
            rule.id = ColumnText(rules.handle, 0);
            rule.packageID = a_package.id;
            rule.version = sqlite3_column_int(rules.handle, 1);
            rule.name = ColumnText(rules.handle, 2);
            rule.isEnabled = sqlite3_column_int(rules.handle, 3) != 0;
            rule.level = sqlite3_column_int(rules.handle, 4);
            rule.levelComparison = static_cast<NumericComparison>(
                sqlite3_column_int(rules.handle, 5));
            rule.maximumLevel = sqlite3_column_int(rules.handle, 6);
            rule.targetGender = sqlite3_column_int(rules.handle, 7);
            rule.targetHumanoid = sqlite3_column_int(rules.handle, 8);
            rule.targetChild = sqlite3_column_int(rules.handle, 9);
            rule.combatState = static_cast<RuleCombatState>(
                sqlite3_column_int(rules.handle, 10));
            rule.followerState = static_cast<RuleFollowerState>(
                sqlite3_column_int(rules.handle, 11));
            rule.actorScope = static_cast<RuleActorScope>(
                sqlite3_column_int(rules.handle, 12));
            // INLOS evaluates loot only for dead/defeated NPCs. Keep the
            // legacy columns readable, but normalize obsolete conditions so
            // invisible values from older packages cannot block a rule.
            rule.combatState = RuleCombatState::kAny;
            rule.actorScope = RuleActorScope::kNPCOnly;
            rule.summonedState = static_cast<RuleSummonedState>(
                sqlite3_column_int(rules.handle, 13));
            rule.hostilityState = static_cast<RuleHostilityState>(
                sqlite3_column_int(rules.handle, 14));
            rule.targetRequiresAll =
                sqlite3_column_int(rules.handle, 15) != 0;
            rule.blacklistedGender = sqlite3_column_int(rules.handle, 16);
            rule.blacklistedHumanoid = sqlite3_column_int(rules.handle, 17);
            rule.blacklistedChild = sqlite3_column_int(rules.handle, 18);
            rule.blacklistRequiresAll =
                sqlite3_column_int(rules.handle, 19) != 0;
            loaded.trigger = static_cast<Trigger>(
                sqlite3_column_int(rules.handle, 20));
            loaded.destination = static_cast<Destination>(
                sqlite3_column_int(rules.handle, 21));
            loaded.requirePlayerKiller =
                sqlite3_column_int(rules.handle, 22) != 0;

            Statement filters;
            if (!Prepare(
                    database.handle,
                    "SELECT scope,type,form_id,editor_id,actor_value_name,"
                    "option_mode,option_value,option_text,actor_value_mode,"
                    "comparison,minimum_value,maximum_value,is_not "
                    "FROM rule_filters WHERE rule_id=?1 AND version=?2 "
                    "ORDER BY scope,position",
                    filters,
                    "load filters")) {
                return false;
            }
            BindText(filters.handle, 1, rule.id);
            sqlite3_bind_int(filters.handle, 2, rule.version);
            while (sqlite3_step(filters.handle) == SQLITE_ROW) {
                BlacklistFilter filter;
                const auto scope = sqlite3_column_int(filters.handle, 0);
                filter.type = ColumnText(filters.handle, 1);
                filter.formIDStr = ColumnText(filters.handle, 2);
                filter.editorID = ColumnText(filters.handle, 3);
                filter.actorValueName = ColumnText(filters.handle, 4);
                filter.optionMode = sqlite3_column_int(filters.handle, 5);
                filter.optionValue = sqlite3_column_int(filters.handle, 6);
                filter.optionText = ColumnText(filters.handle, 7);
                filter.actorValueMode = static_cast<ActorValueMode>(
                    sqlite3_column_int(filters.handle, 8));
                filter.comparison = static_cast<NumericComparison>(
                    sqlite3_column_int(filters.handle, 9));
                filter.minimumValue = static_cast<float>(
                    sqlite3_column_double(filters.handle, 10));
                filter.maximumValue = static_cast<float>(
                    sqlite3_column_double(filters.handle, 11));
                filter.isNot = sqlite3_column_int(filters.handle, 12) != 0;
                (scope == 0 ? rule.targetFilters : rule.blacklistFilters)
                    .push_back(std::move(filter));
            }

            Statement groups;
            if (!Prepare(
                    database.handle,
                    "SELECT position,name,exclusive_group,chance "
                    "FROM reward_groups WHERE rule_id=?1 AND version=?2 "
                    "ORDER BY position",
                    groups,
                    "load groups")) {
                return false;
            }
            BindText(groups.handle, 1, rule.id);
            sqlite3_bind_int(groups.handle, 2, rule.version);
            while (sqlite3_step(groups.handle) == SQLITE_ROW) {
                const auto groupPosition =
                    sqlite3_column_int(groups.handle, 0);
                RewardGroup group;
                group.name = ColumnText(groups.handle, 1);
                group.isExclusive =
                    sqlite3_column_int(groups.handle, 2) != 0;
                group.chanceGroup = static_cast<float>(
                    sqlite3_column_double(groups.handle, 3));

                Statement rewards;
                if (!Prepare(
                        database.handle,
                        "SELECT type,form_id,editor_id,amount,chance,"
                        "function_mode,persistent FROM rewards "
                        "WHERE rule_id=?1 AND version=?2 AND "
                        "group_position=?3 ORDER BY position",
                        rewards,
                        "load rewards")) {
                    return false;
                }
                BindText(rewards.handle, 1, rule.id);
                sqlite3_bind_int(rewards.handle, 2, rule.version);
                sqlite3_bind_int(rewards.handle, 3, groupPosition);
                while (sqlite3_step(rewards.handle) == SQLITE_ROW) {
                    Reward reward;
                    reward.typeReward = ColumnText(rewards.handle, 0);
                    reward.formIDStr = ColumnText(rewards.handle, 1);
                    reward.editorID = ColumnText(rewards.handle, 2);
                    reward.amount = static_cast<std::uint32_t>(
                        sqlite3_column_int64(rewards.handle, 3));
                    reward.chanceReward = static_cast<float>(
                        sqlite3_column_double(rewards.handle, 4));
                    reward.functionOnType =
                        sqlite3_column_int(rewards.handle, 5);
                    reward.isPersistent =
                        sqlite3_column_int(rewards.handle, 6) != 0;
                    group.rewards.push_back(std::move(reward));
                }
                rule.rewardGroups.push_back(std::move(group));
            }
            rule.lastSavedHash = RuleFingerprint(loaded);
            _rules.push_back(std::move(loaded));
        }
        return true;
    }

    bool Store::SaveRule(LootRule& a_rule)
    {
        auto package = std::ranges::find(
            _packages, a_rule.criteria.packageID, &Package::id);
        if (package == _packages.end()) {
            return false;
        }
        Database database;
        if (!OpenDatabase(package->path / "package.db", database) ||
            !Exec(database.handle, "BEGIN IMMEDIATE", "begin save")) {
            return false;
        }
        const auto rollback = [&] {
            Exec(database.handle, "ROLLBACK", "rollback save");
        };
        auto& rule = a_rule.criteria;
        const auto newVersion = rule.version + 1;

        Statement identity;
        if (!Prepare(
                database.handle,
                "INSERT INTO rules(rule_id,current_version) VALUES(?1,?2) "
                "ON CONFLICT(rule_id) DO UPDATE SET "
                "current_version=excluded.current_version,"
                "updated_at=CURRENT_TIMESTAMP",
                identity,
                "save identity")) {
            rollback();
            return false;
        }
        BindText(identity.handle, 1, rule.id);
        sqlite3_bind_int(identity.handle, 2, newVersion);
        if (sqlite3_step(identity.handle) != SQLITE_DONE) {
            rollback();
            return false;
        }

        Statement version;
        if (!Prepare(
                database.handle,
                "INSERT INTO rule_versions VALUES("
                "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,"
                "?14,?15,?16,?17,?18,?19,?20,?21,?22,?23)",
                version,
                "save version")) {
            rollback();
            return false;
        }
        BindText(version.handle, 1, rule.id);
        sqlite3_bind_int(version.handle, 2, newVersion);
        BindText(version.handle, 3, rule.name);
        sqlite3_bind_int(version.handle, 4, rule.isEnabled);
        sqlite3_bind_int(version.handle, 5, rule.level);
        sqlite3_bind_int(
            version.handle, 6, static_cast<int>(rule.levelComparison));
        sqlite3_bind_int(version.handle, 7, rule.maximumLevel);
        sqlite3_bind_int(version.handle, 8, rule.targetGender);
        sqlite3_bind_int(version.handle, 9, rule.targetHumanoid);
        sqlite3_bind_int(version.handle, 10, rule.targetChild);
        sqlite3_bind_int(
            version.handle, 11, static_cast<int>(rule.combatState));
        sqlite3_bind_int(
            version.handle, 12, static_cast<int>(rule.followerState));
        sqlite3_bind_int(
            version.handle, 13, static_cast<int>(rule.actorScope));
        sqlite3_bind_int(
            version.handle, 14, static_cast<int>(rule.summonedState));
        sqlite3_bind_int(
            version.handle, 15, static_cast<int>(rule.hostilityState));
        sqlite3_bind_int(version.handle, 16, rule.targetRequiresAll);
        sqlite3_bind_int(version.handle, 17, rule.blacklistedGender);
        sqlite3_bind_int(version.handle, 18, rule.blacklistedHumanoid);
        sqlite3_bind_int(version.handle, 19, rule.blacklistedChild);
        sqlite3_bind_int(version.handle, 20, rule.blacklistRequiresAll);
        sqlite3_bind_int(
            version.handle, 21, static_cast<int>(a_rule.trigger));
        sqlite3_bind_int(
            version.handle, 22, static_cast<int>(a_rule.destination));
        sqlite3_bind_int(version.handle, 23, a_rule.requirePlayerKiller);
        if (sqlite3_step(version.handle) != SQLITE_DONE) {
            rollback();
            return false;
        }

        Statement filterStatement;
        if (!Prepare(
                database.handle,
                "INSERT INTO rule_filters VALUES("
                "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)",
                filterStatement,
                "save filters")) {
            rollback();
            return false;
        }
        const auto saveFilters = [&](const auto& a_filters, const int a_scope) {
            for (std::size_t index = 0; index < a_filters.size(); ++index) {
                const auto& filter = a_filters[index];
                sqlite3_reset(filterStatement.handle);
                sqlite3_clear_bindings(filterStatement.handle);
                BindText(filterStatement.handle, 1, rule.id);
                sqlite3_bind_int(filterStatement.handle, 2, newVersion);
                sqlite3_bind_int(filterStatement.handle, 3, a_scope);
                sqlite3_bind_int(
                    filterStatement.handle, 4, static_cast<int>(index));
                BindText(filterStatement.handle, 5, filter.type);
                BindText(filterStatement.handle, 6, filter.formIDStr);
                BindText(filterStatement.handle, 7, filter.editorID);
                BindText(
                    filterStatement.handle, 8, filter.actorValueName);
                sqlite3_bind_int(
                    filterStatement.handle, 9, filter.optionMode);
                sqlite3_bind_int(
                    filterStatement.handle, 10, filter.optionValue);
                BindText(filterStatement.handle, 11, filter.optionText);
                sqlite3_bind_int(
                    filterStatement.handle,
                    12,
                    static_cast<int>(filter.actorValueMode));
                sqlite3_bind_int(
                    filterStatement.handle,
                    13,
                    static_cast<int>(filter.comparison));
                sqlite3_bind_double(
                    filterStatement.handle, 14, filter.minimumValue);
                sqlite3_bind_double(
                    filterStatement.handle, 15, filter.maximumValue);
                sqlite3_bind_int(
                    filterStatement.handle, 16, filter.isNot ? 1 : 0);
                if (sqlite3_step(filterStatement.handle) != SQLITE_DONE) {
                    return false;
                }
            }
            return true;
        };
        if (!saveFilters(rule.targetFilters, 0) ||
            !saveFilters(rule.blacklistFilters, 1)) {
            rollback();
            return false;
        }

        Statement groupStatement;
        Statement rewardStatement;
        if (!Prepare(
                database.handle,
                "INSERT INTO reward_groups VALUES(?1,?2,?3,?4,?5,?6)",
                groupStatement,
                "save groups") ||
            !Prepare(
                database.handle,
                "INSERT INTO rewards VALUES("
                "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                rewardStatement,
                "save rewards")) {
            rollback();
            return false;
        }
        for (std::size_t groupIndex = 0;
             groupIndex < rule.rewardGroups.size();
             ++groupIndex) {
            const auto& group = rule.rewardGroups[groupIndex];
            sqlite3_reset(groupStatement.handle);
            BindText(groupStatement.handle, 1, rule.id);
            sqlite3_bind_int(groupStatement.handle, 2, newVersion);
            sqlite3_bind_int(
                groupStatement.handle, 3, static_cast<int>(groupIndex));
            BindText(groupStatement.handle, 4, group.name);
            sqlite3_bind_int(groupStatement.handle, 5, group.isExclusive);
            sqlite3_bind_double(
                groupStatement.handle, 6, group.chanceGroup);
            if (sqlite3_step(groupStatement.handle) != SQLITE_DONE) {
                rollback();
                return false;
            }
            for (std::size_t rewardIndex = 0;
                 rewardIndex < group.rewards.size();
                 ++rewardIndex) {
                const auto& reward = group.rewards[rewardIndex];
                sqlite3_reset(rewardStatement.handle);
                sqlite3_clear_bindings(rewardStatement.handle);
                BindText(rewardStatement.handle, 1, rule.id);
                sqlite3_bind_int(rewardStatement.handle, 2, newVersion);
                sqlite3_bind_int(
                    rewardStatement.handle, 3, static_cast<int>(groupIndex));
                sqlite3_bind_int(
                    rewardStatement.handle, 4, static_cast<int>(rewardIndex));
                BindText(rewardStatement.handle, 5, reward.typeReward);
                BindText(rewardStatement.handle, 6, reward.formIDStr);
                BindText(rewardStatement.handle, 7, reward.editorID);
                sqlite3_bind_int64(
                    rewardStatement.handle, 8, reward.amount);
                sqlite3_bind_double(
                    rewardStatement.handle, 9, reward.chanceReward);
                sqlite3_bind_int(
                    rewardStatement.handle, 10, reward.functionOnType);
                sqlite3_bind_int(
                    rewardStatement.handle, 11, reward.isPersistent);
                if (sqlite3_step(rewardStatement.handle) != SQLITE_DONE) {
                    rollback();
                    return false;
                }
            }
        }

        Statement prune;
        if (!Prepare(
                database.handle,
                "DELETE FROM rule_versions WHERE rule_id=?1 AND version NOT IN "
                "(SELECT version FROM rule_versions WHERE rule_id=?1 "
                "ORDER BY version DESC LIMIT 100)",
                prune,
                "prune versions")) {
            rollback();
            return false;
        }
        BindText(prune.handle, 1, rule.id);
        if (sqlite3_step(prune.handle) != SQLITE_DONE ||
            !Exec(database.handle, "COMMIT", "commit save")) {
            rollback();
            return false;
        }

        rule.version = newVersion;
        rule.lastSavedHash = RuleFingerprint(a_rule);
        return true;
    }

    bool Store::SaveAll()
    {
        std::scoped_lock lock(_lock);
        for (auto& rule : _rules) {
            if (_packagesToDelete.contains(
                    rule.criteria.packageID)) {
                continue;
            }
            if (rule.criteria.lastSavedHash != RuleFingerprint(rule) &&
                !SaveRule(rule)) {
                return false;
            }
        }
        const auto pendingPackages = _packagesToDelete;
        for (const auto& packageID : pendingPackages) {
            if (!DeletePackageNow(packageID)) {
                return false;
            }
            std::erase_if(
                _rules,
                [&](const LootRule& a_rule) {
                    return a_rule.criteria.packageID == packageID;
                });
            _packagesToDelete.erase(packageID);
        }
        RebuildIndices();
        return true;
    }

    bool Store::MarkPackageForDeletion(
        const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        if (a_packageID.empty() ||
            a_packageID == kLocalPackageID) {
            return false;
        }
        const auto found = std::ranges::find(
            _packages, a_packageID, &Package::id);
        return found != _packages.end() &&
            _packagesToDelete.emplace(a_packageID).second;
    }

    bool Store::CancelPackageDeletion(
        const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        return _packagesToDelete.erase(
            std::string(a_packageID)) > 0;
    }

    bool Store::IsPackagePendingDeletion(
        const std::string_view a_packageID) const
    {
        return _packagesToDelete.contains(
            std::string(a_packageID));
    }

    bool Store::DeletePackageNow(
        const std::string_view a_packageID)
    {
        if (a_packageID.empty() ||
            a_packageID == kLocalPackageID) {
            return false;
        }
        const auto found = std::ranges::find(
            _packages, a_packageID, &Package::id);
        if (found == _packages.end()) {
            return false;
        }

        std::error_code error;
        const auto root = std::filesystem::absolute(
            std::filesystem::path(kPackagesRoot),
            error).lexically_normal();
        if (error) {
            return false;
        }
        const auto target = std::filesystem::absolute(
            found->path,
            error).lexically_normal();
        if (error) {
            return false;
        }
        const auto relative = target.lexically_relative(root);
        if (relative.empty() || relative == "." ||
            *relative.begin() == "..") {
            logger::error(
                "[INLOS Store] Refusing to delete package outside '{}'.",
                root.string());
            return false;
        }
        if (std::filesystem::exists(target, error)) {
            std::filesystem::remove_all(target, error);
            if (error) {
                logger::error(
                    "[INLOS Store] Could not delete '{}': {}.",
                    target.string(),
                    error.message());
                return false;
            }
        }
        logger::info(
            "[INLOS Store] Deleted package '{}' from '{}'.",
            found->displayName,
            target.string());
        _packages.erase(found);
        return true;
    }

    std::optional<std::string> Store::CreatePackage(
        const std::string_view a_displayName)
    {
        std::scoped_lock lock(_lock);
        std::string name(a_displayName);
        if (name.empty()) {
            return std::nullopt;
        }
        const auto duplicate = std::ranges::any_of(
            _packages,
            [&](const Package& a_package) {
                return _stricmp(
                    a_package.displayName.c_str(),
                    name.c_str()) == 0;
            });
        if (duplicate) {
            return std::nullopt;
        }
        Package package;
        package.id = GenerateUUID();
        package.displayName = std::move(name);
        package.path = std::filesystem::path(kPackagesRoot) /
            std::format(
                "{}_{}",
                SanitizeFolder(package.displayName),
                package.id.substr(0, 8));
        if (!EnsurePackage(package)) {
            return std::nullopt;
        }
        const auto id = package.id;
        _packages.push_back(std::move(package));
        return id;
    }

    LootRule& Store::CreateRule(const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        LootRule rule;
        rule.destination = Destination::kPlayer;
        rule.criteria.id = GenerateUUID();
        rule.criteria.packageID =
            std::ranges::any_of(
                _packages,
                [&](const Package& a_package) {
                    return a_package.id == a_packageID;
                }) ?
                std::string(a_packageID) :
                std::string(kLocalPackageID);
        rule.criteria.name = "New Loot Rule";
        rule.criteria.actorScope = RuleActorScope::kNPCOnly;
        rule.criteria.combatState = RuleCombatState::kAny;
        rule.criteria.version = 0;
        rule.criteria.lastSavedHash.clear();
        rule.criteria.rewardGroups.push_back(
            RewardGroup{ .name = "Loot" });
        _rules.push_back(std::move(rule));
        RebuildIndices();
        return _rules.back();
    }

    bool Store::DeleteRule(const std::string_view a_ruleID)
    {
        std::scoped_lock lock(_lock);
        const auto found = std::ranges::find_if(
            _rules,
            [&](const LootRule& a_rule) {
                return a_rule.criteria.id == a_ruleID;
            });
        if (found == _rules.end()) {
            return false;
        }
        const auto package = std::ranges::find(
            _packages, found->criteria.packageID, &Package::id);
        if (package == _packages.end()) {
            return false;
        }
        Database database;
        if (!OpenDatabase(package->path / "package.db", database) ||
            !Exec(database.handle, "BEGIN IMMEDIATE", "begin delete")) {
            return false;
        }
        Statement statement;
        if (!Prepare(
                database.handle,
                "DELETE FROM rules WHERE rule_id=?1",
                statement,
                "delete rule")) {
            Exec(database.handle, "ROLLBACK", "rollback delete");
            return false;
        }
        BindText(statement.handle, 1, a_ruleID);
        if (sqlite3_step(statement.handle) != SQLITE_DONE ||
            !Exec(database.handle, "COMMIT", "commit delete")) {
            Exec(database.handle, "ROLLBACK", "rollback delete");
            return false;
        }
        _rules.erase(found);
        RebuildIndices();
        return true;
    }

    bool Store::ExportRulesPackage(
        const std::string_view a_archiveName,
        const std::set<std::string>& a_ruleIDs)
    {
        std::scoped_lock lock(_lock);
        std::map<std::string, std::set<std::string>> selectedByPackage;
        for (const auto& rule : _rules) {
            if (a_ruleIDs.contains(rule.criteria.id) &&
                !_packagesToDelete.contains(rule.criteria.packageID)) {
                selectedByPackage[rule.criteria.packageID].insert(
                    rule.criteria.id);
            }
        }
        if (selectedByPackage.empty()) {
            return false;
        }

        const auto exportRoot =
            std::filesystem::path("Data/Viny Mods/INLOS/Export");
        std::error_code error;
        std::filesystem::create_directories(exportRoot, error);
        if (error) {
            return false;
        }
        const auto archiveBase = SanitizeFolder(
            a_archiveName.empty() ?
                "INLOS_Export" :
                a_archiveName);
        const auto archivePath =
            exportRoot / (archiveBase + ".zip");
        std::vector<std::pair<const Package*, std::filesystem::path>> snapshots;
        const auto cleanup = [&] {
            for (const auto& entry : snapshots) {
                const auto& path = entry.second;
                std::filesystem::remove(path, error);
                std::filesystem::remove(path.string() + "-wal", error);
                std::filesystem::remove(path.string() + "-shm", error);
                std::filesystem::remove(path.string() + "-journal", error);
            }
        };
        for (const auto& [packageID, selectedRules] : selectedByPackage) {
            const auto package = std::ranges::find(
                _packages, packageID, &Package::id);
            if (package == _packages.end()) {
                cleanup();
                return false;
            }
            const auto snapshotPath = exportRoot /
                std::format(".inlos-snapshot-{}.db", GenerateUUID());
            snapshots.emplace_back(std::addressof(*package), snapshotPath);
            bool snapshotReady = false;
            {
                Database source;
                Database destination;
                const auto sourcePath = package->path / "package.db";
                if (sqlite3_open_v2(sourcePath.string().c_str(),
                        &source.handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
                    sqlite3_open_v2(snapshotPath.string().c_str(),
                        &destination.handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) == SQLITE_OK) {
                    if (auto* backup = sqlite3_backup_init(
                            destination.handle, "main", source.handle, "main")) {
                        const auto step = sqlite3_backup_step(backup, -1);
                        const auto finish = sqlite3_backup_finish(backup);
                        snapshotReady = step == SQLITE_DONE && finish == SQLITE_OK;
                    }
                }
            }
            if (snapshotReady) {
                Database snapshot;
                snapshotReady = sqlite3_open_v2(
                    snapshotPath.string().c_str(), &snapshot.handle,
                    SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK &&
                    Exec(snapshot.handle, "PRAGMA foreign_keys=ON", "export foreign keys") &&
                    Exec(snapshot.handle, "PRAGMA journal_mode=DELETE", "export journal mode") &&
                    Exec(snapshot.handle, "BEGIN IMMEDIATE", "begin export selection");
                if (snapshotReady) {
                    Statement rules;
                    Statement remove;
                    snapshotReady = Prepare(snapshot.handle,
                        "SELECT rule_id FROM rules", rules, "list export rules") &&
                        Prepare(snapshot.handle,
                            "DELETE FROM rules WHERE rule_id=?1", remove,
                            "remove unselected export rule");
                    std::vector<std::string> unselected;
                    int step = SQLITE_OK;
                    while (snapshotReady &&
                        (step = sqlite3_step(rules.handle)) == SQLITE_ROW) {
                        const auto ruleID = ColumnText(rules.handle, 0);
                        if (!selectedRules.contains(ruleID)) {
                            unselected.push_back(ruleID);
                        }
                    }
                    snapshotReady = snapshotReady && step == SQLITE_DONE;
                    for (const auto& ruleID : unselected) {
                        if (!snapshotReady) break;
                        sqlite3_reset(remove.handle);
                        sqlite3_clear_bindings(remove.handle);
                        BindText(remove.handle, 1, ruleID);
                        snapshotReady = sqlite3_step(remove.handle) == SQLITE_DONE;
                    }
                    snapshotReady = snapshotReady && Exec(snapshot.handle,
                        "COMMIT", "commit export selection");
                    if (!snapshotReady) {
                        Exec(snapshot.handle, "ROLLBACK", "rollback export selection");
                    }
                }
            }
            if (!snapshotReady) {
                cleanup();
                logger::error(
                    "[INLOS Store] Could not snapshot package '{}'.",
                    package->displayName);
                return false;
            }
        }

        mz_zip_archive archive{};
        const auto zipInitialized = mz_zip_writer_init_file(
            std::addressof(archive),
            archivePath.string().c_str(),
            0);
        bool success = zipInitialized;
        for (const auto& [package, snapshotPath] : snapshots) {
            if (!success) {
                break;
            }
            const auto internalRoot =
                "Data/Viny Mods/INLOS/Packages/" +
                package->path.filename().generic_string() + "/";
            success = mz_zip_writer_add_file(
                std::addressof(archive),
                (internalRoot + "manifest.json").c_str(),
                (package->path / "manifest.json").string().c_str(),
                nullptr, 0, MZ_BEST_COMPRESSION) &&
                mz_zip_writer_add_file(
                    std::addressof(archive),
                    (internalRoot + "package.db").c_str(),
                    snapshotPath.string().c_str(),
                    nullptr, 0, MZ_BEST_COMPRESSION);
        }
        if (zipInitialized) {
            success =
                mz_zip_writer_finalize_archive(
                    std::addressof(archive)) &&
                success;
            mz_zip_writer_end(std::addressof(archive));
        }
        cleanup();
        if (!success) {
            std::filesystem::remove(archivePath, error);
            logger::error(
                "[INLOS Store] Could not export selected rules.");
            return false;
        }
        logger::info(
            "[INLOS Store] Exported {} package(s) to '{}'.",
            snapshots.size(),
            archivePath.string());
        return true;
    }

    LootRule* Store::FindRule(const std::string_view a_ruleID)
    {
        const auto found = _ruleIndices.find(std::string(a_ruleID));
        return found != _ruleIndices.end() ?
            std::addressof(_rules[found->second]) :
            nullptr;
    }

    bool Store::IsRuleModified(const LootRule& a_rule) const
    {
        return a_rule.criteria.lastSavedHash !=
            RuleFingerprint(a_rule);
    }

    void Store::RebuildIndices()
    {
        _ruleIndices.clear();
        for (std::size_t index = 0; index < _rules.size(); ++index) {
            _ruleIndices.emplace(_rules[index].criteria.id, index);
        }
    }
}
