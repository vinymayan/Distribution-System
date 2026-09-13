#include "RulePackageStore.h"

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <format>
#include <random>
#include <unordered_set>

namespace
{
    namespace fs = std::filesystem;

    constexpr int SCHEMA_VERSION = 6;
    constexpr std::string_view LEGACY_RULES_DIR = "Data/Viny Mods/EDF/Rules";
    constexpr std::string_view LEGACY_SKSE_RULES_DIR = "Data/SKSE/Plugins/EDF/Rules";
    constexpr std::string_view LEGACY_BACKUP_DIR = "Data/Viny Mods/EDF/Legacy Backup";

    struct SqliteDb
    {
        sqlite3* handle{ nullptr };

        ~SqliteDb()
        {
            if (handle) {
                sqlite3_close(handle);
            }
        }

        SqliteDb() = default;
        SqliteDb(const SqliteDb&) = delete;
        SqliteDb& operator=(const SqliteDb&) = delete;
    };

    struct SqliteStatement
    {
        sqlite3_stmt* handle{ nullptr };

        ~SqliteStatement()
        {
            if (handle) {
                sqlite3_finalize(handle);
            }
        }

        SqliteStatement() = default;
        SqliteStatement(const SqliteStatement&) = delete;
        SqliteStatement& operator=(const SqliteStatement&) = delete;
    };

    bool Exec(sqlite3* db, const char* sql, const std::string_view context)
    {
        char* error = nullptr;
        const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
        if (rc == SQLITE_OK) {
            return true;
        }
        logger::error("[RulePackageStore] SQLite exec failed in '{}': {}", context, error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return false;
    }

    bool Prepare(sqlite3* db, const char* sql, SqliteStatement& statement, const std::string_view context)
    {
        const auto rc = sqlite3_prepare_v2(db, sql, -1, &statement.handle, nullptr);
        if (rc == SQLITE_OK) {
            return true;
        }
        logger::error("[RulePackageStore] SQLite prepare failed in '{}': {}", context, sqlite3_errmsg(db));
        return false;
    }

    void BindText(sqlite3_stmt* statement, const int index, const std::string_view value)
    {
        sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }

    std::string ColumnText(sqlite3_stmt* statement, const int index)
    {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
        return text ? text : "";
    }

    bool EnsureColumn(
        sqlite3* db,
        const std::string_view table,
        const std::string_view column,
        const std::string_view declaration,
        const std::string_view context)
    {
        SqliteStatement columns;
        const auto query = std::format("PRAGMA table_info({});", table);
        if (!Prepare(db, query.c_str(), columns, context)) {
            return false;
        }
        while (sqlite3_step(columns.handle) == SQLITE_ROW) {
            if (ColumnText(columns.handle, 1) == column) {
                return true;
            }
        }
        const auto migration = std::format(
            "ALTER TABLE {} ADD COLUMN {} {};",
            table,
            column,
            declaration);
        return Exec(db, migration.c_str(), context);
    }

    std::string GenerateUUID()
    {
        static std::random_device randomDevice;
        static std::mt19937_64 generator(randomDevice());
        static std::uniform_int_distribution<std::uint32_t> distribution(0, 0xFFFFFFFF);

        auto a = distribution(generator);
        auto b = distribution(generator);
        auto c = distribution(generator);
        auto d = distribution(generator);
        b = (b & 0xFFFF0FFFU) | 0x00004000U;
        c = (c & 0x3FFFFFFFU) | 0x80000000U;
        return std::format(
            "{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}",
            a,
            b >> 16,
            b & 0xFFFF,
            c >> 16,
            c & 0xFFFF,
            d);
    }

    std::string SanitizeFolder(std::string value)
    {
        for (auto& ch : value) {
            const auto c = static_cast<unsigned char>(ch);
            if (std::isalnum(c) == 0 && ch != '_' && ch != '-' && ch != '.') {
                ch = '_';
            }
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '.')) {
            value.pop_back();
        }
        return value.empty() ? "Package" : value;
    }

    std::string ShortID(std::string_view id)
    {
        std::string result;
        for (const auto ch : id) {
            if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
                result.push_back(ch);
                if (result.size() == 8) {
                    break;
                }
            }
        }
        return result.empty() ? "package" : result;
    }

    fs::path PackageFolder(
        const fs::path& root,
        const std::string_view displayName,
        const std::string_view packageID)
    {
        if (packageID == RulePackageStore::LOCAL_PACKAGE_ID) {
            return root / "Local_Rules";
        }
        return root / std::format("{}_{}", SanitizeFolder(std::string(displayName)), ShortID(packageID));
    }

    bool WriteManifest(const RulePackage& package)
    {
        std::error_code ec;
        fs::create_directories(package.path, ec);
        if (ec) {
            logger::error("[RulePackageStore] Could not create package directory '{}': {}", package.path.string(), ec.message());
            return false;
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();
        doc.AddMember("schemaVersion", SCHEMA_VERSION, allocator);
        doc.AddMember("id", rapidjson::Value(package.id.c_str(), allocator), allocator);
        doc.AddMember("displayName", rapidjson::Value(package.displayName.c_str(), allocator), allocator);
        doc.AddMember("enabled", package.enabled, allocator);
        doc.AddMember("database", "package.db", allocator);

        std::ofstream stream(package.path / "manifest.json");
        if (!stream.is_open()) {
            logger::error("[RulePackageStore] Could not write manifest for package '{}'.", package.displayName);
            return false;
        }
        rapidjson::OStreamWrapper wrapper(stream);
        rapidjson::PrettyWriter writer(wrapper);
        doc.Accept(writer);
        return stream.good();
    }

    std::optional<RulePackage> ReadManifest(const fs::path& folder)
    {
        std::ifstream stream(folder / "manifest.json");
        if (!stream.is_open()) {
            return std::nullopt;
        }
        rapidjson::IStreamWrapper wrapper(stream);
        rapidjson::Document doc;
        doc.ParseStream(wrapper);
        if (doc.HasParseError() || !doc.IsObject() ||
            !doc.HasMember("id") || !doc["id"].IsString() ||
            !doc.HasMember("displayName") || !doc["displayName"].IsString() ||
            !doc.HasMember("schemaVersion") || !doc["schemaVersion"].IsInt() ||
            doc["schemaVersion"].GetInt() < 1 ||
            doc["schemaVersion"].GetInt() > SCHEMA_VERSION ||
            !doc.HasMember("database") || !doc["database"].IsString() ||
            std::string_view(doc["database"].GetString()) != "package.db") {
            logger::error("[RulePackageStore] Invalid package manifest '{}'.", (folder / "manifest.json").string());
            return std::nullopt;
        }

        RulePackage package;
        package.id = doc["id"].GetString();
        package.displayName = doc["displayName"].GetString();
        package.enabled = !doc.HasMember("enabled") || !doc["enabled"].IsBool() || doc["enabled"].GetBool();
        package.path = folder;
        package.schemaVersion = doc["schemaVersion"].GetInt();
        if (package.id.empty() || package.displayName.empty()) {
            logger::error("[RulePackageStore] Package manifest '{}' has an empty ID or name.", folder.string());
            return std::nullopt;
        }
        return package;
    }

    bool EnsureSchema(sqlite3* db, const RulePackage& package)
    {
        const auto context = package.displayName;
        SqliteStatement schemaVersion;
        if (!Prepare(db, "PRAGMA user_version;", schemaVersion, context) ||
            sqlite3_step(schemaVersion.handle) != SQLITE_ROW) {
            return false;
        }
        const auto existingVersion = sqlite3_column_int(schemaVersion.handle, 0);
        if (existingVersion > SCHEMA_VERSION) {
            logger::error(
                "[RulePackageStore] Package '{}' uses unsupported schema version {}.",
                package.displayName,
                existingVersion);
            return false;
        }
        sqlite3_finalize(schemaVersion.handle);
        schemaVersion.handle = nullptr;
        if (!Exec(db, "PRAGMA foreign_keys=ON;", context) ||
            !Exec(db, "PRAGMA journal_mode=WAL;", context) ||
            !Exec(db, "PRAGMA synchronous=NORMAL;", context) ||
            !Exec(db, "PRAGMA busy_timeout=3000;", context)) {
            return false;
        }

        if (!Exec(db, "BEGIN IMMEDIATE;", context)) {
            return false;
        }
        struct MigrationTransaction {
            sqlite3* db = nullptr;
            bool active = true;

            ~MigrationTransaction()
            {
                if (active && db) {
                    sqlite3_exec(
                        db, "ROLLBACK;", nullptr, nullptr, nullptr);
                }
            }
        } transaction{ db };

        if (!Exec(db,
                "CREATE TABLE IF NOT EXISTS metadata("
                "key TEXT PRIMARY KEY NOT NULL,"
                "value TEXT NOT NULL"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS rules("
                "rule_id TEXT PRIMARY KEY NOT NULL,"
                "current_version INTEGER NOT NULL,"
                "pending_delete INTEGER NOT NULL DEFAULT 0 CHECK(pending_delete IN(0,1)),"
                "legacy_source TEXT,"
                "created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
                "updated_at INTEGER NOT NULL DEFAULT(unixepoch())"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS rule_versions("
                "rule_id TEXT NOT NULL,"
                "version INTEGER NOT NULL,"
                "name TEXT NOT NULL,"
                "enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),"
                "level INTEGER NOT NULL,"
                "level_comparison INTEGER NOT NULL DEFAULT 0 CHECK(level_comparison IN(0,1,2,3)),"
                "maximum_level INTEGER NOT NULL DEFAULT 1 CHECK(maximum_level >= 1),"
                "target_gender INTEGER NOT NULL,"
                "target_humanoid INTEGER NOT NULL,"
                "target_child INTEGER NOT NULL,"
                "combat_state INTEGER NOT NULL CHECK(combat_state IN(0,1,2)),"
                "follower_state INTEGER NOT NULL CHECK(follower_state IN(0,1,2)),"
                "actor_scope INTEGER NOT NULL DEFAULT 0 CHECK(actor_scope IN(0,1,2)),"
                "summoned_state INTEGER NOT NULL DEFAULT 0 CHECK(summoned_state IN(0,1,2)),"
                "hostility_state INTEGER NOT NULL DEFAULT 0 CHECK(hostility_state IN(0,1,2)),"
                "target_requires_all INTEGER NOT NULL CHECK(target_requires_all IN(0,1)),"
                "rule_exclusive INTEGER NOT NULL CHECK(rule_exclusive IN(0,1)),"
                "blacklisted_gender INTEGER NOT NULL,"
                "blacklisted_humanoid INTEGER NOT NULL,"
                "blacklisted_child INTEGER NOT NULL,"
                "blacklist_requires_all INTEGER NOT NULL CHECK(blacklist_requires_all IN(0,1)),"
                "content_hash TEXT NOT NULL,"
                "created_at INTEGER NOT NULL DEFAULT(unixepoch()),"
                "PRIMARY KEY(rule_id,version),"
                "FOREIGN KEY(rule_id) REFERENCES rules(rule_id) ON DELETE CASCADE"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS rule_filters("
                "rule_id TEXT NOT NULL,"
                "version INTEGER NOT NULL,"
                "scope TEXT NOT NULL CHECK(scope IN('target','blacklist')),"
                "ordinal INTEGER NOT NULL,"
                "type TEXT NOT NULL,"
                "form_id TEXT NOT NULL,"
                "editor_id TEXT NOT NULL,"
                "actor_value_name TEXT NOT NULL DEFAULT '',"
                "option_mode INTEGER NOT NULL DEFAULT 0,"
                "option_value INTEGER NOT NULL DEFAULT 0,"
                "option_text TEXT NOT NULL DEFAULT '',"
                "actor_value_mode INTEGER NOT NULL DEFAULT 0 CHECK(actor_value_mode IN(0,1,2)),"
                "numeric_comparison INTEGER NOT NULL DEFAULT 0 CHECK(numeric_comparison IN(0,1,2,3)),"
                "minimum_value REAL NOT NULL DEFAULT 0,"
                "maximum_value REAL NOT NULL DEFAULT 0,"
                "is_not INTEGER NOT NULL DEFAULT 0 CHECK(is_not IN(0,1)),"
                "PRIMARY KEY(rule_id,version,scope,ordinal),"
                "FOREIGN KEY(rule_id,version) REFERENCES rule_versions(rule_id,version) ON DELETE CASCADE"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS reward_groups("
                "rule_id TEXT NOT NULL,"
                "version INTEGER NOT NULL,"
                "group_ordinal INTEGER NOT NULL,"
                "name TEXT NOT NULL,"
                "exclusive INTEGER NOT NULL CHECK(exclusive IN(0,1)),"
                "chance REAL NOT NULL,"
                "PRIMARY KEY(rule_id,version,group_ordinal),"
                "FOREIGN KEY(rule_id,version) REFERENCES rule_versions(rule_id,version) ON DELETE CASCADE"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS rewards("
                "rule_id TEXT NOT NULL,"
                "version INTEGER NOT NULL,"
                "group_ordinal INTEGER NOT NULL,"
                "reward_ordinal INTEGER NOT NULL,"
                "type_reward TEXT NOT NULL,"
                "form_id TEXT NOT NULL,"
                "editor_id TEXT NOT NULL,"
                "amount INTEGER NOT NULL,"
                "chance REAL NOT NULL,"
                "function_on_type INTEGER NOT NULL,"
                "equip_contexts INTEGER NOT NULL CHECK(equip_contexts BETWEEN 1 AND 7),"
                "persistent INTEGER NOT NULL CHECK(persistent IN(0,1)),"
                "actor_value_name TEXT NOT NULL DEFAULT '',"
                "actor_value_amount REAL NOT NULL DEFAULT 0,"
                "numeric_operation INTEGER NOT NULL DEFAULT 0 CHECK(numeric_operation IN(0,1)),"
                "numeric_source INTEGER NOT NULL DEFAULT 0 CHECK(numeric_source IN(0,1,2)),"
                "numeric_binding INTEGER NOT NULL DEFAULT 0 CHECK(numeric_binding IN(0,1)),"
                "percent_base_mode INTEGER NOT NULL DEFAULT 1 CHECK(percent_base_mode IN(0,1,2)),"
                "source_actor_value_name TEXT NOT NULL DEFAULT '',"
                "source_actor_value_mode INTEGER NOT NULL DEFAULT 0 CHECK(source_actor_value_mode IN(0,1,2)),"
                "source_global_form_id TEXT NOT NULL DEFAULT '',"
                "source_global_editor_id TEXT NOT NULL DEFAULT '',"
                "source_multiplier REAL NOT NULL DEFAULT 1,"
                "PRIMARY KEY(rule_id,version,group_ordinal,reward_ordinal),"
                "FOREIGN KEY(rule_id,version,group_ordinal) "
                "REFERENCES reward_groups(rule_id,version,group_ordinal) ON DELETE CASCADE"
                ");",
                context) ||
            !Exec(db,
                "CREATE TABLE IF NOT EXISTS legacy_imports("
                "source_path TEXT PRIMARY KEY NOT NULL,"
                "content_hash TEXT NOT NULL,"
                "rule_id TEXT NOT NULL,"
                "imported_at INTEGER NOT NULL DEFAULT(unixepoch()),"
                "backup_path TEXT NOT NULL,"
                "status TEXT NOT NULL CHECK(status IN('pending_backup','complete'))"
                ");",
                context) ||
            !Exec(db, "CREATE INDEX IF NOT EXISTS idx_rule_versions_current ON rule_versions(rule_id,version DESC);", context)) {
            return false;
        }
        if (!EnsureColumn(
                db, "rule_versions", "level_comparison",
                "INTEGER NOT NULL DEFAULT 0 CHECK(level_comparison IN(0,1,2,3))", context) ||
            !EnsureColumn(
                db, "rules", "pending_delete",
                "INTEGER NOT NULL DEFAULT 0 CHECK(pending_delete IN(0,1))", context) ||
            !EnsureColumn(
                db, "rule_versions", "maximum_level",
                "INTEGER NOT NULL DEFAULT 1 CHECK(maximum_level >= 1)", context) ||
            !EnsureColumn(
                db, "rule_filters", "option_mode",
                "INTEGER NOT NULL DEFAULT 0", context) ||
            !EnsureColumn(
                db, "rule_filters", "option_value",
                "INTEGER NOT NULL DEFAULT 0", context) ||
            !EnsureColumn(
                db, "rule_filters", "option_text",
                "TEXT NOT NULL DEFAULT ''", context) ||
            !EnsureColumn(
                db, "rule_filters", "is_not",
                "INTEGER NOT NULL DEFAULT 0 CHECK(is_not IN(0,1))", context) ||
            !EnsureColumn(
                db, "rule_versions", "actor_scope",
                "INTEGER NOT NULL DEFAULT 0 CHECK(actor_scope IN(0,1,2))", context) ||
            !EnsureColumn(
                db, "rule_versions", "summoned_state",
                "INTEGER NOT NULL DEFAULT 0 CHECK(summoned_state IN(0,1,2))", context) ||
            !EnsureColumn(
                db, "rule_versions", "hostility_state",
                "INTEGER NOT NULL DEFAULT 0 CHECK(hostility_state IN(0,1,2))", context) ||
            !EnsureColumn(
                db, "rewards", "actor_value_name",
                "TEXT NOT NULL DEFAULT ''", context) ||
            !EnsureColumn(
                db, "rewards", "actor_value_amount",
                "REAL NOT NULL DEFAULT 0", context) ||
            !EnsureColumn(db, "rewards", "numeric_operation",
                "INTEGER NOT NULL DEFAULT 0 CHECK(numeric_operation IN(0,1))", context) ||
            !EnsureColumn(db, "rewards", "numeric_source",
                "INTEGER NOT NULL DEFAULT 0 CHECK(numeric_source IN(0,1,2))", context) ||
            !EnsureColumn(db, "rewards", "numeric_binding",
                "INTEGER NOT NULL DEFAULT 0 CHECK(numeric_binding IN(0,1))", context) ||
            !EnsureColumn(db, "rewards", "percent_base_mode",
                "INTEGER NOT NULL DEFAULT 1 CHECK(percent_base_mode IN(0,1,2))", context) ||
            !EnsureColumn(db, "rewards", "source_actor_value_name",
                "TEXT NOT NULL DEFAULT ''", context) ||
            !EnsureColumn(db, "rewards", "source_actor_value_mode",
                "INTEGER NOT NULL DEFAULT 0 CHECK(source_actor_value_mode IN(0,1,2))", context) ||
            !EnsureColumn(db, "rewards", "source_global_form_id",
                "TEXT NOT NULL DEFAULT ''", context) ||
            !EnsureColumn(db, "rewards", "source_global_editor_id",
                "TEXT NOT NULL DEFAULT ''", context) ||
            !EnsureColumn(db, "rewards", "source_multiplier",
                "REAL NOT NULL DEFAULT 1", context)) {
            return false;
        }

        SqliteStatement insertMetadata;
        if (!Prepare(db,
                "INSERT OR IGNORE INTO metadata(key,value) VALUES(?1,?2);",
                insertMetadata,
                context)) {
            return false;
        }
        BindText(insertMetadata.handle, 1, "package_id");
        BindText(insertMetadata.handle, 2, package.id);
        if (sqlite3_step(insertMetadata.handle) != SQLITE_DONE) {
            return false;
        }
        if (!Exec(
                db,
                "UPDATE metadata SET value='6' WHERE key='schema_version';",
                context)) {
            return false;
        }
        sqlite3_reset(insertMetadata.handle);
        sqlite3_clear_bindings(insertMetadata.handle);
        BindText(insertMetadata.handle, 1, "schema_version");
        BindText(insertMetadata.handle, 2, std::to_string(SCHEMA_VERSION));
        if (sqlite3_step(insertMetadata.handle) != SQLITE_DONE) {
            return false;
        }

        SqliteStatement readMetadata;
        if (!Prepare(db, "SELECT value FROM metadata WHERE key='package_id';", readMetadata, context) ||
            sqlite3_step(readMetadata.handle) != SQLITE_ROW) {
            return false;
        }
        const auto storedID = ColumnText(readMetadata.handle, 0);
        if (storedID != package.id) {
            logger::error(
                "[RulePackageStore] Manifest/database package ID mismatch in '{}': manifest='{}', database='{}'.",
                package.path.string(),
                package.id,
                storedID);
            return false;
        }
        sqlite3_finalize(readMetadata.handle);
        readMetadata.handle = nullptr;
        sqlite3_finalize(insertMetadata.handle);
        insertMetadata.handle = nullptr;
        if (!Exec(db, "PRAGMA user_version=6;", context) ||
            !Exec(db, "COMMIT;", context)) {
            return false;
        }
        transaction.active = false;

        if ((existingVersion < SCHEMA_VERSION ||
                package.schemaVersion < SCHEMA_VERSION) &&
            !WriteManifest(package)) {
            logger::error(
                "[RulePackageStore] Database '{}' was migrated, but its manifest could not be updated.",
                package.path.string());
            return false;
        }
        return true;
    }

    bool OpenPackage(const RulePackage& package, SqliteDb& db)
    {
        const auto dbPath = package.path / "package.db";
        const auto rc = sqlite3_open_v2(
            dbPath.string().c_str(),
            &db.handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr);
        if (rc != SQLITE_OK) {
            logger::error(
                "[RulePackageStore] Could not open package database '{}': {}",
                dbPath.string(),
                db.handle ? sqlite3_errmsg(db.handle) : "unknown error");
            return false;
        }
        return EnsureSchema(db.handle, package);
    }

    bool Begin(sqlite3* db, const std::string_view context)
    {
        return Exec(db, "BEGIN IMMEDIATE;", context);
    }

    bool Commit(sqlite3* db, const std::string_view context)
    {
        return Exec(db, "COMMIT;", context);
    }

    void Rollback(sqlite3* db, const std::string_view context)
    {
        if (!Exec(db, "ROLLBACK;", context)) {
            logger::error("[RulePackageStore] Rollback also failed in '{}'.", context);
        }
    }

    bool InsertVersion(sqlite3* db, const Rule& rule, const std::string_view context)
    {
        SqliteStatement versionStatement;
        if (!Prepare(db,
                "INSERT INTO rule_versions("
                "rule_id,version,name,enabled,level,level_comparison,maximum_level,"
                "target_gender,target_humanoid,target_child,"
                "combat_state,follower_state,actor_scope,summoned_state,hostility_state,"
                "target_requires_all,rule_exclusive,blacklisted_gender,blacklisted_humanoid,"
                "blacklisted_child,blacklist_requires_all,content_hash"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22);",
                versionStatement,
                context)) {
            return false;
        }
        BindText(versionStatement.handle, 1, rule.id);
        sqlite3_bind_int(versionStatement.handle, 2, rule.version);
        BindText(versionStatement.handle, 3, rule.name);
        sqlite3_bind_int(versionStatement.handle, 4, rule.isEnabled ? 1 : 0);
        sqlite3_bind_int(versionStatement.handle, 5, rule.level);
        sqlite3_bind_int(
            versionStatement.handle, 6,
            static_cast<int>(rule.levelComparison));
        sqlite3_bind_int(
            versionStatement.handle, 7,
            rule.levelComparison == NumericComparison::kBetween ?
                rule.maximumLevel :
                rule.level);
        sqlite3_bind_int(versionStatement.handle, 8, rule.targetGender);
        sqlite3_bind_int(versionStatement.handle, 9, rule.targetHumanoid);
        sqlite3_bind_int(versionStatement.handle, 10, rule.targetChild);
        sqlite3_bind_int(versionStatement.handle, 11, static_cast<int>(rule.combatState));
        sqlite3_bind_int(versionStatement.handle, 12, static_cast<int>(rule.followerState));
        sqlite3_bind_int(versionStatement.handle, 13, static_cast<int>(rule.actorScope));
        sqlite3_bind_int(versionStatement.handle, 14, static_cast<int>(rule.summonedState));
        sqlite3_bind_int(versionStatement.handle, 15, static_cast<int>(rule.hostilityState));
        sqlite3_bind_int(versionStatement.handle, 16, rule.targetRequiresAll ? 1 : 0);
        sqlite3_bind_int(versionStatement.handle, 17, rule.isExclusive ? 1 : 0);
        sqlite3_bind_int(versionStatement.handle, 18, rule.blacklistedGender);
        sqlite3_bind_int(versionStatement.handle, 19, rule.blacklistedHumanoid);
        sqlite3_bind_int(versionStatement.handle, 20, rule.blacklistedChild);
        sqlite3_bind_int(versionStatement.handle, 21, rule.blacklistRequiresAll ? 1 : 0);
        BindText(versionStatement.handle, 22, rule.CalculateHash());
        if (sqlite3_step(versionStatement.handle) != SQLITE_DONE) {
            logger::error("[RulePackageStore] Could not insert rule version '{}:{}': {}", rule.id, rule.version, sqlite3_errmsg(db));
            return false;
        }

        const auto insertFilters = [&](const std::vector<BlacklistFilter>& filters, const std::string_view scope) {
            SqliteStatement statement;
            if (!Prepare(db,
                    "INSERT INTO rule_filters("
                    "rule_id,version,scope,ordinal,type,form_id,editor_id,"
                    "actor_value_name,option_mode,option_value,option_text,"
                    "actor_value_mode,numeric_comparison,minimum_value,maximum_value,is_not"
                    ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);",
                    statement,
                    context)) {
                return false;
            }
            for (std::size_t index = 0; index < filters.size(); ++index) {
                sqlite3_reset(statement.handle);
                sqlite3_clear_bindings(statement.handle);
                BindText(statement.handle, 1, rule.id);
                sqlite3_bind_int(statement.handle, 2, rule.version);
                BindText(statement.handle, 3, scope);
                sqlite3_bind_int64(statement.handle, 4, static_cast<sqlite3_int64>(index));
                BindText(statement.handle, 5, filters[index].type);
                BindText(statement.handle, 6, filters[index].formIDStr);
                BindText(statement.handle, 7, filters[index].editorID);
                BindText(statement.handle, 8, filters[index].actorValueName);
                sqlite3_bind_int(statement.handle, 9, filters[index].optionMode);
                sqlite3_bind_int(statement.handle, 10, filters[index].optionValue);
                BindText(statement.handle, 11, filters[index].optionText);
                sqlite3_bind_int(
                    statement.handle, 12,
                    static_cast<int>(filters[index].actorValueMode));
                sqlite3_bind_int(
                    statement.handle, 13,
                    static_cast<int>(filters[index].comparison));
                sqlite3_bind_double(
                    statement.handle, 14,
                    filters[index].minimumValue);
                sqlite3_bind_double(
                    statement.handle, 15,
                    filters[index].maximumValue);
                sqlite3_bind_int(
                    statement.handle, 16,
                    filters[index].isNot ? 1 : 0);
                if (sqlite3_step(statement.handle) != SQLITE_DONE) {
                    return false;
                }
            }
            return true;
        };

        if (!insertFilters(rule.targetFilters, "target") ||
            !insertFilters(rule.blacklistFilters, "blacklist")) {
            logger::error("[RulePackageStore] Could not insert filters for rule '{}': {}", rule.id, sqlite3_errmsg(db));
            return false;
        }

        SqliteStatement groupStatement;
        SqliteStatement rewardStatement;
        if (!Prepare(db,
                "INSERT INTO reward_groups(rule_id,version,group_ordinal,name,exclusive,chance) "
                "VALUES(?1,?2,?3,?4,?5,?6);",
                groupStatement,
                context) ||
            !Prepare(db,
                "INSERT INTO rewards("
                "rule_id,version,group_ordinal,reward_ordinal,type_reward,form_id,editor_id,"
                "amount,chance,function_on_type,equip_contexts,persistent,"
                "actor_value_name,actor_value_amount,numeric_operation,numeric_source,"
                "numeric_binding,percent_base_mode,source_actor_value_name,"
                "source_actor_value_mode,source_global_form_id,source_global_editor_id,source_multiplier"
                ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23);",
                rewardStatement,
                context)) {
            return false;
        }

        for (std::size_t groupIndex = 0; groupIndex < rule.rewardGroups.size(); ++groupIndex) {
            const auto& group = rule.rewardGroups[groupIndex];
            sqlite3_reset(groupStatement.handle);
            sqlite3_clear_bindings(groupStatement.handle);
            BindText(groupStatement.handle, 1, rule.id);
            sqlite3_bind_int(groupStatement.handle, 2, rule.version);
            sqlite3_bind_int64(groupStatement.handle, 3, static_cast<sqlite3_int64>(groupIndex));
            BindText(groupStatement.handle, 4, group.name);
            sqlite3_bind_int(groupStatement.handle, 5, group.isExclusive ? 1 : 0);
            sqlite3_bind_double(groupStatement.handle, 6, group.chanceGroup);
            if (sqlite3_step(groupStatement.handle) != SQLITE_DONE) {
                return false;
            }

            for (std::size_t rewardIndex = 0; rewardIndex < group.rewards.size(); ++rewardIndex) {
                const auto& reward = group.rewards[rewardIndex];
                sqlite3_reset(rewardStatement.handle);
                sqlite3_clear_bindings(rewardStatement.handle);
                BindText(rewardStatement.handle, 1, rule.id);
                sqlite3_bind_int(rewardStatement.handle, 2, rule.version);
                sqlite3_bind_int64(rewardStatement.handle, 3, static_cast<sqlite3_int64>(groupIndex));
                sqlite3_bind_int64(rewardStatement.handle, 4, static_cast<sqlite3_int64>(rewardIndex));
                BindText(rewardStatement.handle, 5, reward.typeReward);
                BindText(rewardStatement.handle, 6, reward.formIDStr);
                BindText(rewardStatement.handle, 7, reward.editorID);
                sqlite3_bind_int64(rewardStatement.handle, 8, reward.amount);
                sqlite3_bind_double(rewardStatement.handle, 9, reward.chanceReward);
                sqlite3_bind_int(rewardStatement.handle, 10, reward.functionOnType);
                sqlite3_bind_int(rewardStatement.handle, 11, reward.equipContexts);
                sqlite3_bind_int(rewardStatement.handle, 12, reward.isPersistent ? 1 : 0);
                BindText(rewardStatement.handle, 13, reward.actorValueName);
                sqlite3_bind_double(
                    rewardStatement.handle, 14,
                    reward.actorValueAmount);
                sqlite3_bind_int(rewardStatement.handle, 15,
                    static_cast<int>(reward.numericOperation));
                sqlite3_bind_int(rewardStatement.handle, 16,
                    static_cast<int>(reward.numericSource));
                sqlite3_bind_int(rewardStatement.handle, 17,
                    static_cast<int>(reward.numericBinding));
                sqlite3_bind_int(rewardStatement.handle, 18,
                    static_cast<int>(reward.percentBaseMode));
                BindText(rewardStatement.handle, 19,
                    reward.sourceActorValueName);
                sqlite3_bind_int(rewardStatement.handle, 20,
                    static_cast<int>(reward.sourceActorValueMode));
                BindText(rewardStatement.handle, 21,
                    reward.sourceGlobalFormID);
                BindText(rewardStatement.handle, 22,
                    reward.sourceGlobalEditorID);
                sqlite3_bind_double(rewardStatement.handle, 23,
                    reward.sourceMultiplier);
                if (sqlite3_step(rewardStatement.handle) != SQLITE_DONE) {
                    return false;
                }
            }
        }
        return true;
    }

    bool InsertRuleHistory(
        sqlite3* db,
        const Rule& latest,
        const std::vector<Rule>& history,
        const std::string_view legacySource,
        const std::string_view context)
    {
        SqliteStatement ruleStatement;
        if (!Prepare(db,
                "INSERT INTO rules(rule_id,current_version,legacy_source,updated_at) "
                "VALUES(?1,?2,?3,unixepoch()) "
                "ON CONFLICT(rule_id) DO UPDATE SET "
                "current_version=excluded.current_version,legacy_source=excluded.legacy_source,updated_at=unixepoch();",
                ruleStatement,
                context)) {
            return false;
        }
        BindText(ruleStatement.handle, 1, latest.id);
        sqlite3_bind_int(ruleStatement.handle, 2, latest.version);
        if (legacySource.empty()) {
            sqlite3_bind_null(ruleStatement.handle, 3);
        } else {
            BindText(ruleStatement.handle, 3, legacySource);
        }
        if (sqlite3_step(ruleStatement.handle) != SQLITE_DONE) {
            return false;
        }

        std::unordered_set<int> versions;
        for (const auto& version : history) {
            if (!versions.insert(version.version).second) {
                continue;
            }
            if (!InsertVersion(db, version, context)) {
                return false;
            }
        }
        if (versions.empty()) {
            return InsertVersion(db, latest, context);
        }
        return true;
    }

    bool ReadRuleVersion(sqlite3* db, const std::string& ruleID, const int version, const std::string& packageID, Rule& rule)
    {
        SqliteStatement statement;
        if (!Prepare(db,
                "SELECT name,enabled,level,level_comparison,maximum_level,"
                "target_gender,target_humanoid,target_child,target_requires_all,"
                "combat_state,follower_state,actor_scope,summoned_state,hostility_state,"
                "rule_exclusive,blacklisted_gender,blacklisted_humanoid,blacklisted_child,blacklist_requires_all "
                "FROM rule_versions WHERE rule_id=?1 AND version=?2;",
                statement,
                packageID)) {
            return false;
        }
        BindText(statement.handle, 1, ruleID);
        sqlite3_bind_int(statement.handle, 2, version);
        if (sqlite3_step(statement.handle) != SQLITE_ROW) {
            return false;
        }

        rule.id = ruleID;
        rule.packageID = packageID;
        rule.version = version;
        rule.name = ColumnText(statement.handle, 0);
        rule.isEnabled = sqlite3_column_int(statement.handle, 1) != 0;
        rule.level = std::max(
            1, sqlite3_column_int(statement.handle, 2));
        rule.levelComparison = static_cast<NumericComparison>(
            std::clamp(sqlite3_column_int(statement.handle, 3), 0, 3));
        rule.maximumLevel = std::max(
            1, sqlite3_column_int(statement.handle, 4));
        if (rule.levelComparison != NumericComparison::kBetween) {
            rule.maximumLevel = rule.level;
        }
        rule.targetGender = sqlite3_column_int(statement.handle, 5);
        rule.targetHumanoid = sqlite3_column_int(statement.handle, 6);
        rule.targetChild = sqlite3_column_int(statement.handle, 7);
        rule.targetRequiresAll = sqlite3_column_int(statement.handle, 8) != 0;
        rule.combatState = static_cast<RuleCombatState>(
            std::clamp(sqlite3_column_int(statement.handle, 9), 0, 2));
        rule.followerState = static_cast<RuleFollowerState>(
            std::clamp(sqlite3_column_int(statement.handle, 10), 0, 2));
        rule.actorScope = static_cast<RuleActorScope>(
            std::clamp(sqlite3_column_int(statement.handle, 11), 0, 2));
        rule.summonedState = static_cast<RuleSummonedState>(
            std::clamp(sqlite3_column_int(statement.handle, 12), 0, 2));
        rule.hostilityState = static_cast<RuleHostilityState>(
            std::clamp(sqlite3_column_int(statement.handle, 13), 0, 2));
        rule.isExclusive = sqlite3_column_int(statement.handle, 14) != 0;
        rule.blacklistedGender = sqlite3_column_int(statement.handle, 15);
        rule.blacklistedHumanoid = sqlite3_column_int(statement.handle, 16);
        rule.blacklistedChild = sqlite3_column_int(statement.handle, 17);
        rule.blacklistRequiresAll = sqlite3_column_int(statement.handle, 18) != 0;

        SqliteStatement filters;
        if (!Prepare(db,
                "SELECT scope,type,form_id,editor_id,actor_value_name,"
                "option_mode,option_value,option_text,"
                "actor_value_mode,numeric_comparison,minimum_value,maximum_value,is_not "
                "FROM rule_filters "
                "WHERE rule_id=?1 AND version=?2 ORDER BY scope,ordinal;",
                filters,
                packageID)) {
            return false;
        }
        BindText(filters.handle, 1, ruleID);
        sqlite3_bind_int(filters.handle, 2, version);
        while (sqlite3_step(filters.handle) == SQLITE_ROW) {
            BlacklistFilter filter;
            const auto scope = ColumnText(filters.handle, 0);
            filter.type = ColumnText(filters.handle, 1);
            filter.formIDStr = ColumnText(filters.handle, 2);
            filter.editorID = ColumnText(filters.handle, 3);
            filter.actorValueName = ColumnText(filters.handle, 4);
            filter.optionMode = sqlite3_column_int(filters.handle, 5);
            filter.optionValue = sqlite3_column_int(filters.handle, 6);
            filter.optionText = ColumnText(filters.handle, 7);
            filter.actorValueMode = static_cast<ActorValueMode>(
                std::clamp(sqlite3_column_int(filters.handle, 8), 0, 2));
            filter.comparison = static_cast<NumericComparison>(
                std::clamp(sqlite3_column_int(filters.handle, 9), 0, 3));
            filter.minimumValue = static_cast<float>(
                sqlite3_column_double(filters.handle, 10));
            filter.maximumValue = static_cast<float>(
                sqlite3_column_double(filters.handle, 11));
            filter.isNot = sqlite3_column_int(filters.handle, 12) != 0;
            NormalizeNumericValueFilter(filter);
            if (scope == "target") {
                rule.targetFilters.push_back(std::move(filter));
            } else {
                rule.blacklistFilters.push_back(std::move(filter));
            }
        }

        SqliteStatement groups;
        if (!Prepare(db,
                "SELECT group_ordinal,name,exclusive,chance FROM reward_groups "
                "WHERE rule_id=?1 AND version=?2 ORDER BY group_ordinal;",
                groups,
                packageID)) {
            return false;
        }
        BindText(groups.handle, 1, ruleID);
        sqlite3_bind_int(groups.handle, 2, version);
        while (sqlite3_step(groups.handle) == SQLITE_ROW) {
            const auto groupOrdinal = sqlite3_column_int(groups.handle, 0);
            RewardGroup group;
            group.name = ColumnText(groups.handle, 1);
            group.isExclusive = sqlite3_column_int(groups.handle, 2) != 0;
            group.chanceGroup = static_cast<float>(sqlite3_column_double(groups.handle, 3));

            SqliteStatement rewards;
            if (!Prepare(db,
                    "SELECT type_reward,form_id,editor_id,amount,chance,function_on_type,equip_contexts,persistent,"
                    "actor_value_name,actor_value_amount,numeric_operation,numeric_source,"
                    "numeric_binding,percent_base_mode,source_actor_value_name,source_actor_value_mode,"
                    "source_global_form_id,source_global_editor_id,source_multiplier "
                    "FROM rewards WHERE rule_id=?1 AND version=?2 AND group_ordinal=?3 "
                    "ORDER BY reward_ordinal;",
                    rewards,
                    packageID)) {
                return false;
            }
            BindText(rewards.handle, 1, ruleID);
            sqlite3_bind_int(rewards.handle, 2, version);
            sqlite3_bind_int(rewards.handle, 3, groupOrdinal);
            while (sqlite3_step(rewards.handle) == SQLITE_ROW) {
                Reward reward;
                reward.typeReward = ColumnText(rewards.handle, 0);
                reward.formIDStr = ColumnText(rewards.handle, 1);
                reward.editorID = ColumnText(rewards.handle, 2);
                reward.amount = static_cast<std::uint32_t>(sqlite3_column_int64(rewards.handle, 3));
                reward.chanceReward = static_cast<float>(sqlite3_column_double(rewards.handle, 4));
                reward.functionOnType = sqlite3_column_int(rewards.handle, 5);
                reward.equipContexts = static_cast<EquipmentContextMask>(
                    std::clamp(sqlite3_column_int(rewards.handle, 6), 1,
                        static_cast<int>(kAllEquipmentContexts)));
                reward.isPersistent = sqlite3_column_int(rewards.handle, 7) != 0;
                reward.actorValueName = ColumnText(rewards.handle, 8);
                reward.actorValueAmount = static_cast<float>(
                    sqlite3_column_double(rewards.handle, 9));
                reward.numericOperation = static_cast<NumericRewardOperation>(
                    std::clamp(sqlite3_column_int(rewards.handle, 10), 0, 1));
                reward.numericSource = static_cast<NumericRewardSource>(
                    std::clamp(sqlite3_column_int(rewards.handle, 11), 0, 2));
                reward.numericBinding = static_cast<NumericRewardBinding>(
                    std::clamp(sqlite3_column_int(rewards.handle, 12), 0, 1));
                reward.percentBaseMode = static_cast<ActorValueMode>(
                    std::clamp(sqlite3_column_int(rewards.handle, 13), 0, 2));
                reward.sourceActorValueName = ColumnText(rewards.handle, 14);
                reward.sourceActorValueMode = static_cast<ActorValueMode>(
                    std::clamp(sqlite3_column_int(rewards.handle, 15), 0, 2));
                reward.sourceGlobalFormID = ColumnText(rewards.handle, 16);
                reward.sourceGlobalEditorID = ColumnText(rewards.handle, 17);
                reward.sourceMultiplier = static_cast<float>(
                    sqlite3_column_double(rewards.handle, 18));
                group.rewards.push_back(std::move(reward));
            }
            rule.rewardGroups.push_back(std::move(group));
        }
        rule.lastSavedHash = rule.CalculateHash();
        return true;
    }

    bool LoadPackageRules(
        const RulePackage& package,
        std::vector<Rule>& rules,
        std::map<std::string, std::vector<Rule>>& histories,
        std::map<std::string, std::string>& owners,
        std::set<std::string>& deletedRuleIDs)
    {
        SqliteDb db;
        if (!OpenPackage(package, db)) {
            return false;
        }

        SqliteStatement ruleStatement;
        if (!Prepare(db.handle, "SELECT rule_id,current_version,pending_delete FROM rules ORDER BY rule_id;", ruleStatement, package.id)) {
            return false;
        }
        while (sqlite3_step(ruleStatement.handle) == SQLITE_ROW) {
            const auto ruleID = ColumnText(ruleStatement.handle, 0);
            const auto currentVersion = sqlite3_column_int(ruleStatement.handle, 1);
            const bool pendingDelete =
                sqlite3_column_int(ruleStatement.handle, 2) != 0;
            if (ruleID.empty()) {
                continue;
            }
            if (const auto owner = owners.find(ruleID); owner != owners.end()) {
                logger::error(
                    "[RulePackageStore] Duplicate global rule ID '{}' in packages '{}' and '{}'; package '{}' was skipped for this rule.",
                    ruleID,
                    owner->second,
                    package.id,
                    package.id);
                continue;
            }

            SqliteStatement versions;
            if (!Prepare(db.handle,
                    "SELECT version FROM rule_versions WHERE rule_id=?1 ORDER BY version DESC;",
                    versions,
                    package.id)) {
                continue;
            }
            BindText(versions.handle, 1, ruleID);
            std::vector<Rule> history;
            while (sqlite3_step(versions.handle) == SQLITE_ROW) {
                Rule version;
                if (ReadRuleVersion(db.handle, ruleID, sqlite3_column_int(versions.handle, 0), package.id, version)) {
                    history.push_back(std::move(version));
                }
            }
            const auto current = std::ranges::find_if(history, [currentVersion](const Rule& value) {
                return value.version == currentVersion;
            });
            if (current == history.end()) {
                logger::error("[RulePackageStore] Rule '{}' in package '{}' has no current version {}.", ruleID, package.id, currentVersion);
                continue;
            }
            histories[ruleID] = std::move(history);
            owners[ruleID] = package.id;
            if (pendingDelete) {
                deletedRuleIDs.insert(ruleID);
            }
            else {
                rules.push_back(*current);
            }
        }
        return true;
    }

    std::string Fnv1a(const std::string_view data)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto ch : data) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
        return std::format("{:016x}", hash);
    }

    std::string SourceKey(const fs::path& path)
    {
        std::error_code ec;
        const auto absolute = fs::absolute(path, ec);
        return (ec ? path : absolute).lexically_normal().generic_string();
    }

    bool MoveLegacyFile(const fs::path& source, const fs::path& destination)
    {
        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            return false;
        }
        fs::rename(source, destination, ec);
        if (!ec) {
            return true;
        }
        ec.clear();
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
        ec.clear();
        fs::remove(source, ec);
        return !ec;
    }

    fs::path BackupPath(const fs::path& source, const bool skseSource, const std::string_view hash)
    {
        auto folder = fs::path(LEGACY_BACKUP_DIR) / (skseSource ? "SKSE Rules" : "Rules");
        auto destination = folder / source.filename();
        if (fs::exists(destination)) {
            destination = folder / std::format("{}_{}{}", source.stem().string(), hash.substr(0, 8), source.extension().string());
        }
        return destination;
    }

    bool UpdateImportStatus(sqlite3* db, const std::string_view source, const std::string_view status)
    {
        SqliteStatement statement;
        if (!Prepare(db, "UPDATE legacy_imports SET status=?2 WHERE source_path=?1;", statement, "legacy import status")) {
            return false;
        }
        BindText(statement.handle, 1, source);
        BindText(statement.handle, 2, status);
        return sqlite3_step(statement.handle) == SQLITE_DONE;
    }

    void RetryPendingBackups(sqlite3* db)
    {
        SqliteStatement statement;
        if (!Prepare(db,
                "SELECT source_path,backup_path FROM legacy_imports WHERE status='pending_backup' ORDER BY source_path;",
                statement,
                "legacy backup retry")) {
            return;
        }
        std::vector<std::pair<std::string, std::string>> pending;
        while (sqlite3_step(statement.handle) == SQLITE_ROW) {
            pending.emplace_back(ColumnText(statement.handle, 0), ColumnText(statement.handle, 1));
        }
        sqlite3_finalize(statement.handle);
        statement.handle = nullptr;
        for (const auto& [source, backup] : pending) {
            if (!fs::exists(source) || MoveLegacyFile(source, backup)) {
                UpdateImportStatus(db, source, "complete");
            }
        }
    }

    bool ImportLegacyFile(
        sqlite3* db,
        const fs::path& path,
        const bool skseSource,
        const std::unordered_set<std::string>& externalRuleIDs)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            return false;
        }
        const std::string contents{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>() };
        const auto source = SourceKey(path);
        const auto contentHash = Fnv1a(contents);
        const auto backup = BackupPath(path, skseSource, contentHash);

        SqliteStatement previousImport;
        if (Prepare(db,
                "SELECT content_hash,status,backup_path FROM legacy_imports WHERE source_path=?1;",
                previousImport,
                "legacy import lookup")) {
            BindText(previousImport.handle, 1, source);
            if (sqlite3_step(previousImport.handle) == SQLITE_ROW &&
                ColumnText(previousImport.handle, 0) == contentHash) {
                const auto recordedBackup = ColumnText(previousImport.handle, 2);
                if (MoveLegacyFile(path, recordedBackup.empty() ? backup : fs::path(recordedBackup))) {
                    UpdateImportStatus(db, source, "complete");
                }
                return true;
            }
        }

        Rule latest;
        std::vector<Rule> history;
        std::string error;
        if (!ParseLegacyRuleFile(path, latest, history, error)) {
            logger::error("[RulePackageStore] Legacy rule '{}' was not imported: {}", path.string(), error);
            return false;
        }
        if (latest.id.empty()) {
            logger::error("[RulePackageStore] Legacy rule '{}' has no rule ID.", path.string());
            return false;
        }
        if (externalRuleIDs.contains(latest.id)) {
            logger::error(
                "[RulePackageStore] Legacy rule '{}' conflicts with rule ID '{}' in an installed SQL package.",
                path.string(),
                latest.id);
            return false;
        }
        latest.packageID = std::string(RulePackageStore::LOCAL_PACKAGE_ID);
        for (auto& version : history) {
            version.packageID = latest.packageID;
        }

        SqliteStatement existing;
        if (!Prepare(db,
                "SELECT current_version,COALESCE(legacy_source,'') FROM rules WHERE rule_id=?1;",
                existing,
                "legacy conflict lookup")) {
            return false;
        }
        BindText(existing.handle, 1, latest.id);
        if (sqlite3_step(existing.handle) == SQLITE_ROW) {
            const auto currentVersion = sqlite3_column_int(existing.handle, 0);
            const auto legacySource = ColumnText(existing.handle, 1);
            if (legacySource != source) {
                logger::error(
                    "[RulePackageStore] Legacy rule '{}' conflicts with existing global rule ID '{}' owned by another source.",
                    path.string(),
                    latest.id);
                return false;
            }
            if (latest.version <= currentVersion) {
                logger::error(
                    "[RulePackageStore] Legacy update '{}' has version {}, but stored rule '{}' is already version {}.",
                    path.string(),
                    latest.version,
                    latest.id,
                    currentVersion);
                return false;
            }
        }
        sqlite3_finalize(existing.handle);
        existing.handle = nullptr;

        if (!Begin(db, "legacy import")) {
            return false;
        }
        SqliteStatement removeRule;
        if (!Prepare(db, "DELETE FROM rules WHERE rule_id=?1;", removeRule, "legacy import") ) {
            Rollback(db, "legacy import");
            return false;
        }
        BindText(removeRule.handle, 1, latest.id);
        if (sqlite3_step(removeRule.handle) != SQLITE_DONE ||
            !InsertRuleHistory(db, latest, history, source, "legacy import")) {
            Rollback(db, "legacy import");
            return false;
        }

        SqliteStatement importStatement;
        if (!Prepare(db,
                "INSERT INTO legacy_imports(source_path,content_hash,rule_id,backup_path,status,imported_at) "
                "VALUES(?1,?2,?3,?4,'pending_backup',unixepoch()) "
                "ON CONFLICT(source_path) DO UPDATE SET "
                "content_hash=excluded.content_hash,rule_id=excluded.rule_id,backup_path=excluded.backup_path,"
                "status='pending_backup',imported_at=unixepoch();",
                importStatement,
                "legacy import")) {
            Rollback(db, "legacy import");
            return false;
        }
        BindText(importStatement.handle, 1, source);
        BindText(importStatement.handle, 2, contentHash);
        BindText(importStatement.handle, 3, latest.id);
        BindText(importStatement.handle, 4, backup.generic_string());
        if (sqlite3_step(importStatement.handle) != SQLITE_DONE || !Commit(db, "legacy import")) {
            Rollback(db, "legacy import");
            return false;
        }

        if (MoveLegacyFile(path, backup)) {
            UpdateImportStatus(db, source, "complete");
            logger::info("[RulePackageStore] Imported legacy rule '{}' into Local Rules and moved it to '{}'.", latest.id, backup.string());
        } else {
            logger::warn(
                "[RulePackageStore] Imported legacy rule '{}', but backup move is pending for '{}'.",
                latest.id,
                path.string());
        }
        return true;
    }

    void ImportLegacyDirectory(
        sqlite3* db,
        const fs::path& directory,
        const bool skseSource,
        const std::unordered_set<std::string>& externalRuleIDs)
    {
        std::error_code ec;
        if (!fs::exists(directory, ec)) {
            return;
        }
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (ec) {
                logger::error("[RulePackageStore] Could not enumerate legacy directory '{}': {}", directory.string(), ec.message());
                return;
            }
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        for (const auto& file : files) {
            ImportLegacyFile(db, file, skseSource, externalRuleIDs);
        }
    }

    bool ImportLegacyRules(
        const RulePackage& localPackage,
        const std::unordered_set<std::string>& externalRuleIDs)
    {
        SqliteDb db;
        if (!OpenPackage(localPackage, db)) {
            return false;
        }
        RetryPendingBackups(db.handle);
        ImportLegacyDirectory(db.handle, fs::path(LEGACY_RULES_DIR), false, externalRuleIDs);
        ImportLegacyDirectory(db.handle, fs::path(LEGACY_SKSE_RULES_DIR), true, externalRuleIDs);
        return true;
    }

    std::unordered_set<std::string> CollectExternalRuleIDs(
        const std::vector<RulePackage>& packages)
    {
        std::unordered_set<std::string> result;
        for (const auto& package : packages) {
            if (package.id == RulePackageStore::LOCAL_PACKAGE_ID) {
                continue;
            }
            SqliteDb db;
            if (!OpenPackage(package, db)) {
                continue;
            }
            SqliteStatement rules;
            if (!Prepare(db.handle, "SELECT rule_id FROM rules;", rules, package.id)) {
                continue;
            }
            while (sqlite3_step(rules.handle) == SQLITE_ROW) {
                result.insert(ColumnText(rules.handle, 0));
            }
        }
        return result;
    }

    const RulePackage* FindPackage(const std::vector<RulePackage>& packages, const std::string_view id)
    {
        const auto found = std::ranges::find_if(packages, [id](const RulePackage& package) {
            return package.id == id;
        });
        return found == packages.end() ? nullptr : &*found;
    }
}

bool RulePackageStore::Load(
    std::vector<Rule>& rules,
    std::map<std::string, std::vector<Rule>>& histories,
    std::map<std::string, std::string>& owners,
    std::set<std::string>& deletedRuleIDs)
{
    rules.clear();
    histories.clear();
    owners.clear();
    deletedRuleIDs.clear();
    _packages.clear();

    const auto packagesRoot = fs::path(PACKAGES_DIR);
    const auto localFolder = PackageFolder(packagesRoot, LOCAL_PACKAGE_NAME, LOCAL_PACKAGE_ID);
    RulePackage localPackage{
        std::string(LOCAL_PACKAGE_ID),
        std::string(LOCAL_PACKAGE_NAME),
        true,
        localFolder,
        SCHEMA_VERSION
    };
    if (!fs::exists(localFolder / "manifest.json") && !WriteManifest(localPackage)) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(packagesRoot, ec);
    if (ec) {
        return false;
    }

    std::vector<RulePackage> discovered;
    for (const auto& entry : fs::directory_iterator(packagesRoot, ec)) {
        if (ec) {
            logger::error("[RulePackageStore] Could not enumerate packages: {}", ec.message());
            return false;
        }
        if (!entry.is_directory()) {
            continue;
        }
        if (auto package = ReadManifest(entry.path()); package && package->enabled) {
            discovered.push_back(std::move(*package));
        }
    }

    std::ranges::sort(discovered, [](const RulePackage& lhs, const RulePackage& rhs) {
        const bool lhsLocal = lhs.id == RulePackageStore::LOCAL_PACKAGE_ID;
        const bool rhsLocal = rhs.id == RulePackageStore::LOCAL_PACKAGE_ID;
        if (lhsLocal != rhsLocal) return lhsLocal;
        return lhs.id < rhs.id;
    });

    std::unordered_set<std::string> packageIDs;
    for (auto& package : discovered) {
        if (!packageIDs.insert(package.id).second) {
            logger::error("[RulePackageStore] Duplicate package ID '{}' at '{}'; package skipped.", package.id, package.path.string());
            continue;
        }
        SqliteDb validation;
        if (OpenPackage(package, validation)) {
            _packages.push_back(package);
        }
    }

    const auto* local = FindPackage(_packages, LOCAL_PACKAGE_ID);
    if (!local) {
        logger::error("[RulePackageStore] Local Rules package could not be initialized.");
        return false;
    }
    ImportLegacyRules(*local, CollectExternalRuleIDs(_packages));

    bool ok = true;
    for (const auto& package : _packages) {
        ok = LoadPackageRules(
            package, rules, histories, owners, deletedRuleIDs) && ok;
    }
    logger::info("[RulePackageStore] Loaded {} rules from {} packages.", rules.size(), _packages.size());
    return ok;
}

bool RulePackageStore::SaveRule(Rule& rule, std::vector<Rule>& history)
{
    const auto packageID = rule.packageID.empty() ? std::string(LOCAL_PACKAGE_ID) : rule.packageID;
    const auto* package = FindPackage(_packages, packageID);
    if (!package) {
        logger::error("[RulePackageStore] Cannot save rule '{}': package '{}' does not exist.", rule.id, packageID);
        return false;
    }

    SqliteDb db;
    if (!OpenPackage(*package, db) || !Begin(db.handle, packageID)) {
        return false;
    }

    Rule saved = rule;
    saved.packageID = packageID;
    saved.version = rule.version + 1;

    SqliteStatement ruleStatement;
    if (!Prepare(db.handle,
            "INSERT INTO rules(rule_id,current_version,pending_delete,updated_at) VALUES(?1,?2,0,unixepoch()) "
            "ON CONFLICT(rule_id) DO UPDATE SET current_version=excluded.current_version,pending_delete=0,updated_at=unixepoch();",
            ruleStatement,
            packageID)) {
        Rollback(db.handle, packageID);
        return false;
    }
    BindText(ruleStatement.handle, 1, saved.id);
    sqlite3_bind_int(ruleStatement.handle, 2, saved.version);
    if (sqlite3_step(ruleStatement.handle) != SQLITE_DONE || !InsertVersion(db.handle, saved, packageID)) {
        Rollback(db.handle, packageID);
        return false;
    }

    if (!Commit(db.handle, packageID)) {
        Rollback(db.handle, packageID);
        return false;
    }

    saved.lastSavedHash = saved.CalculateHash();
    rule = saved;
    std::erase_if(history, [&saved](const Rule& value) {
        return value.version == saved.version;
    });
    history.insert(history.begin(), saved);
    return true;
}

bool RulePackageStore::MarkRuleDeleted(const std::string_view ruleID, const std::string_view packageID)
{
    const auto* package = FindPackage(_packages, packageID);
    if (!package) {
        return false;
    }
    SqliteDb db;
    if (!OpenPackage(*package, db) || !Begin(db.handle, packageID)) {
        return false;
    }
    SqliteStatement statement;
    if (!Prepare(
            db.handle,
            "UPDATE rules SET pending_delete=1,updated_at=unixepoch() WHERE rule_id=?1;",
            statement,
            packageID)) {
        Rollback(db.handle, packageID);
        return false;
    }
    BindText(statement.handle, 1, ruleID);
    if (sqlite3_step(statement.handle) != SQLITE_DONE || !Commit(db.handle, packageID)) {
        Rollback(db.handle, packageID);
        return false;
    }
    return true;
}

bool RulePackageStore::DeletePackage(const std::string_view packageID)
{
    if (packageID.empty() || packageID == LOCAL_PACKAGE_ID) {
        logger::error(
            "[RulePackageStore] The Local Rules package cannot be deleted.");
        return false;
    }

    const auto found = std::ranges::find_if(
        _packages,
        [packageID](const RulePackage& package) {
            return package.id == packageID;
        });
    if (found == _packages.end()) {
        logger::error(
            "[RulePackageStore] Package '{}' was not found for deletion.",
            packageID);
        return false;
    }

    std::error_code ec;
    const auto packagesRoot =
        fs::absolute(fs::path(PACKAGES_DIR), ec).lexically_normal();
    if (ec) {
        logger::error(
            "[RulePackageStore] Could not resolve packages root: {}.",
            ec.message());
        return false;
    }
    const auto packagePath =
        fs::absolute(found->path, ec).lexically_normal();
    if (ec) {
        logger::error(
            "[RulePackageStore] Could not resolve package path '{}': {}.",
            found->path.string(),
            ec.message());
        return false;
    }
    const auto relative = packagePath.lexically_relative(packagesRoot);
    if (relative.empty() || relative == "." ||
        *relative.begin() == "..") {
        logger::error(
            "[RulePackageStore] Refusing to delete package '{}' outside '{}'.",
            packagePath.string(),
            packagesRoot.string());
        return false;
    }

    if (fs::exists(packagePath, ec)) {
        fs::remove_all(packagePath, ec);
        if (ec) {
            logger::error(
                "[RulePackageStore] Could not delete package directory '{}': {}.",
                packagePath.string(),
                ec.message());
            return false;
        }
    }

    logger::info(
        "[RulePackageStore] Package '{}' deleted from '{}'.",
        found->displayName,
        packagePath.string());
    _packages.erase(found);
    return true;
}

std::optional<std::string> RulePackageStore::CreatePackage(
    const std::string_view displayName,
    const std::string_view requestedID)
{
    const std::string name(displayName);
    if (name.empty()) {
        return std::nullopt;
    }
    const auto duplicateName = std::ranges::find_if(_packages, [&name](const RulePackage& package) {
        return package.displayName == name;
    });
    if (duplicateName != _packages.end()) {
        logger::error("[RulePackageStore] A package named '{}' already exists.", name);
        return std::nullopt;
    }

    RulePackage package;
    if (!requestedID.empty()) {
        if (std::ranges::any_of(requestedID, [](const unsigned char ch) {
                return !std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-';
            }) || FindPackage(_packages, requestedID)) {
            logger::error(
                "[RulePackageStore] Invalid or duplicate requested package ID '{}'.",
                requestedID);
            return std::nullopt;
        }
        package.id = requestedID;
    }
    else {
        do {
            package.id = GenerateUUID();
        } while (FindPackage(_packages, package.id));
    }
    package.displayName = name;
    package.path = PackageFolder(fs::path(PACKAGES_DIR), name, package.id);
    package.schemaVersion = SCHEMA_VERSION;
    if (!WriteManifest(package)) {
        return std::nullopt;
    }
    SqliteDb db;
    if (!OpenPackage(package, db)) {
        return std::nullopt;
    }
    _packages.push_back(package);
    std::ranges::sort(_packages, [](const RulePackage& lhs, const RulePackage& rhs) {
        const bool lhsLocal = lhs.id == RulePackageStore::LOCAL_PACKAGE_ID;
        const bool rhsLocal = rhs.id == RulePackageStore::LOCAL_PACKAGE_ID;
        if (lhsLocal != rhsLocal) return lhsLocal;
        return lhs.id < rhs.id;
    });
    return package.id;
}

bool RulePackageStore::CreateSnapshot(
    const std::string_view displayName,
    const std::vector<Rule>& rules,
    const std::map<std::string, std::vector<Rule>>& histories,
    const fs::path& stagingRoot,
    RulePackage& outPackage)
{
    RulePackage package;
    package.id = GenerateUUID();
    package.displayName =
        displayName.empty() ? "EDF Export" : std::string(displayName);
    package.enabled = true;
    package.path =
        PackageFolder(stagingRoot, package.displayName, package.id);
    return WriteSnapshot(
        std::move(package), rules, histories, outPackage);
}

bool RulePackageStore::CreateSnapshot(
    const RulePackage& sourcePackage,
    const std::vector<Rule>& rules,
    const std::map<std::string, std::vector<Rule>>& histories,
    const fs::path& stagingRoot,
    RulePackage& outPackage)
{
    RulePackage package = sourcePackage;
    auto folder = sourcePackage.path.filename();
    if (folder.empty()) {
        folder = PackageFolder(
            fs::path{}, package.displayName, package.id).filename();
    }
    package.path = stagingRoot / folder;
    return WriteSnapshot(
        std::move(package), rules, histories, outPackage);
}

bool RulePackageStore::WriteSnapshot(
    RulePackage package,
    const std::vector<Rule>& rules,
    const std::map<std::string, std::vector<Rule>>& histories,
    RulePackage& outPackage)
{
    if (rules.empty() || package.id.empty()) {
        return false;
    }
    outPackage = std::move(package);
    outPackage.schemaVersion = SCHEMA_VERSION;
    if (!WriteManifest(outPackage)) {
        return false;
    }
    SqliteDb db;
    if (!OpenPackage(outPackage, db) || !Begin(db.handle, "package snapshot")) {
        return false;
    }
    for (const auto& latest : rules) {
        auto exportedLatest = latest;
        exportedLatest.packageID = outPackage.id;
        std::vector<Rule> exportedHistory;
        if (const auto found = histories.find(latest.id); found != histories.end()) {
            exportedHistory = found->second;
            for (auto& version : exportedHistory) {
                version.packageID = outPackage.id;
            }
        } else {
            exportedHistory.push_back(exportedLatest);
        }
        if (!InsertRuleHistory(db.handle, exportedLatest, exportedHistory, "", "package snapshot")) {
            Rollback(db.handle, "package snapshot");
            return false;
        }
    }
    if (!Commit(db.handle, "package snapshot")) {
        Rollback(db.handle, "package snapshot");
        return false;
    }
    Exec(db.handle, "PRAGMA wal_checkpoint(TRUNCATE);", "package snapshot");
    Exec(db.handle, "PRAGMA journal_mode=DELETE;", "package snapshot");
    return true;
}
