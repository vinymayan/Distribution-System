#include "WIYT/Store.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <sqlite3.h>

#include <fstream>
#include <sstream>

namespace WIYT
{
    namespace
    {
        constexpr auto kPackagesRoot =
            "Data/Viny Mods/WIYT/Packages";

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
            sqlite3* a_database,
            const std::string_view a_sql,
            const std::string_view a_context)
        {
            char* error = nullptr;
            if (sqlite3_exec(
                    a_database,
                    a_sql.data(),
                    nullptr,
                    nullptr,
                    &error) == SQLITE_OK) {
                return true;
            }
            logger::error(
                "[WIYT Store] {} failed: {}",
                a_context,
                error ? error : sqlite3_errmsg(a_database));
            sqlite3_free(error);
            return false;
        }

        bool Prepare(
            sqlite3* a_database,
            const std::string_view a_sql,
            Statement& a_statement,
            const std::string_view a_context)
        {
            if (sqlite3_prepare_v2(
                    a_database,
                    a_sql.data(),
                    static_cast<int>(a_sql.size()),
                    &a_statement.handle,
                    nullptr) == SQLITE_OK) {
                return true;
            }
            logger::error(
                "[WIYT Store] {} prepare failed: {}",
                a_context,
                sqlite3_errmsg(a_database));
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
            Database& a_database,
            const bool a_create = true)
        {
            if (sqlite3_open_v2(
                    a_path.string().c_str(),
                    &a_database.handle,
                    SQLITE_OPEN_READWRITE |
                        (a_create ? SQLITE_OPEN_CREATE : 0),
                    nullptr) != SQLITE_OK) {
                logger::error(
                    "[WIYT Store] Could not open '{}': {}",
                    a_path.string(),
                    a_database.handle ?
                        sqlite3_errmsg(a_database.handle) :
                        "unknown");
                return false;
            }
            sqlite3_busy_timeout(a_database.handle, 5000);
            return Exec(
                       a_database.handle,
                       "PRAGMA foreign_keys=ON",
                       "foreign keys") &&
                Exec(
                       a_database.handle,
                       "PRAGMA journal_mode=WAL",
                       "WAL") &&
                Exec(
                       a_database.handle,
                       "PRAGMA synchronous=NORMAL",
                       "synchronous");
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

        std::string ColumnText(
            sqlite3_stmt* a_statement,
            const int a_column)
        {
            const auto* value =
                sqlite3_column_text(a_statement, a_column);
            return value ?
                reinterpret_cast<const char*>(value) :
                std::string{};
        }

        std::string SanitizeFolder(const std::string_view a_name)
        {
            std::string result;
            result.reserve(a_name.size());
            for (const unsigned char character : a_name) {
                if (std::isalnum(character)) {
                    result.push_back(static_cast<char>(character));
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

        bool IsValidPublicGlobalEditorID(
            const std::string_view a_editorID)
        {
            return !a_editorID.empty() &&
                a_editorID.size() < 128 &&
                std::ranges::all_of(
                    a_editorID,
                    [](const unsigned char a_character) {
                        return std::isalnum(a_character) != 0 ||
                            a_character == '_';
                    });
        }

        bool ValidateTitleDefinition(
            const TitleDefinition& a_title)
        {
            if (a_title.id.empty() || a_title.name.empty() ||
                !IsValidPublicGlobalEditorID(
                    a_title.publicGlobalEditorID)) {
                logger::error(
                    "[WIYT Store] Title '{}' has an invalid identity, "
                    "name, or public Global EditorID.",
                    a_title.id);
                return false;
            }
            std::set<std::string> requirementIDs;
            for (const auto& requirement : a_title.requirements) {
                if (requirement.id.empty() ||
                    !requirementIDs.emplace(
                        requirement.id).second ||
                    !std::isfinite(requirement.targetAmount) ||
                    requirement.targetAmount <= 0.0f) {
                    logger::error(
                        "[WIYT Store] Title '{}' has an invalid or "
                        "duplicate requirement.",
                        a_title.id);
                    return false;
                }
            }
            for (const auto& group : a_title.rewardGroups) {
                if (!std::isfinite(group.chanceGroup) ||
                    group.chanceGroup < 0.0f ||
                    group.chanceGroup > 100.0f) {
                    return false;
                }
                for (const auto& reward : group.rewards) {
                    if (!std::isfinite(reward.chanceReward) ||
                        reward.chanceReward < 0.0f ||
                        reward.chanceReward > 100.0f) {
                        return false;
                    }
                }
            }
            return true;
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
            const auto domain = getString("domain");
            const auto database = getString("database");
            if (const auto found = document.FindMember("enabled");
                found != document.MemberEnd() &&
                found->value.IsBool()) {
                a_package.enabled = found->value.GetBool();
            }
            if (const auto found =
                    document.FindMember("schemaVersion");
                found != document.MemberEnd() &&
                found->value.IsInt()) {
                a_package.schemaVersion = found->value.GetInt();
            }
            return !a_package.id.empty() &&
                !a_package.displayName.empty() &&
                (database.empty() || database == "package.db") &&
                (domain.empty() || domain == "wiyt");
        }

        bool WriteManifest(const Package& a_package)
        {
            rapidjson::Document document;
            document.SetObject();
            auto& allocator = document.GetAllocator();
            const auto addString = [&](
                                       const char* a_name,
                                       const std::string_view a_value) {
                rapidjson::Value value;
                value.SetString(
                    a_value.data(),
                    static_cast<rapidjson::SizeType>(a_value.size()),
                    allocator);
                document.AddMember(
                    rapidjson::Value(a_name, allocator),
                    value,
                    allocator);
            };
            document.AddMember(
                "schemaVersion",
                Store::kSchemaVersion,
                allocator);
            addString("id", a_package.id);
            addString("displayName", a_package.displayName);
            addString("domain", "wiyt");
            document.AddMember(
                "enabled",
                a_package.enabled,
                allocator);
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
CREATE TABLE IF NOT EXISTS titles(
    title_id TEXT PRIMARY KEY,
    current_version INTEGER NOT NULL CHECK(current_version >= 0),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS title_versions(
    title_id TEXT NOT NULL,
    version INTEGER NOT NULL CHECK(version >= 0),
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    public_global_editor_id TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),
    PRIMARY KEY(title_id, version),
    FOREIGN KEY(title_id) REFERENCES titles(title_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS requirements(
    title_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    position INTEGER NOT NULL,
    requirement_id TEXT NOT NULL,
    name TEXT NOT NULL,
    source_type INTEGER NOT NULL CHECK(source_type BETWEEN 0 AND 3),
    activity_type INTEGER NOT NULL CHECK(activity_type BETWEEN 0 AND 11),
    tracking_mode INTEGER NOT NULL CHECK(tracking_mode BETWEEN 0 AND 2),
    aggregation INTEGER NOT NULL CHECK(aggregation BETWEEN 0 AND 3),
    target_amount REAL NOT NULL CHECK(target_amount > 0),
    statistic_name TEXT NOT NULL,
    reference_form_id TEXT NOT NULL,
    reference_editor_id TEXT NOT NULL,
    graph_variable_name TEXT NOT NULL,
    graph_variable_type INTEGER NOT NULL CHECK(graph_variable_type BETWEEN 0 AND 2),
    filters_require_all INTEGER NOT NULL CHECK(filters_require_all IN(0,1)),
    prerequisite_mode INTEGER NOT NULL DEFAULT 0 CHECK(prerequisite_mode BETWEEN 0 AND 1),
    allow_follower_actions INTEGER NOT NULL DEFAULT 1 CHECK(allow_follower_actions IN(0,1)),
    allow_summon_actions INTEGER NOT NULL DEFAULT 1 CHECK(allow_summon_actions IN(0,1)),
    PRIMARY KEY(title_id, version, position),
    UNIQUE(title_id, version, requirement_id),
    FOREIGN KEY(title_id, version)
        REFERENCES title_versions(title_id, version) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS requirement_filters(
    title_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    requirement_position INTEGER NOT NULL,
    scope INTEGER NOT NULL CHECK(scope BETWEEN 0 AND 3),
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
    PRIMARY KEY(
        title_id, version, requirement_position, scope, position),
    FOREIGN KEY(title_id, version, requirement_position)
        REFERENCES requirements(title_id, version, position)
        ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS reward_groups(
    title_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    exclusive_group INTEGER NOT NULL CHECK(exclusive_group IN(0,1)),
    chance REAL NOT NULL,
    PRIMARY KEY(title_id, version, position),
    FOREIGN KEY(title_id, version)
        REFERENCES title_versions(title_id, version) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS rewards(
    title_id TEXT NOT NULL,
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
    PRIMARY KEY(title_id, version, group_position, position),
    FOREIGN KEY(title_id, version, group_position)
        REFERENCES reward_groups(title_id, version, position)
        ON DELETE CASCADE
);
)sql";

        bool MigratePackage(Package& a_package)
        {
            if (a_package.schemaVersion == Store::kSchemaVersion) {
                return true;
            }
            if (a_package.schemaVersion != 1) {
                logger::error(
                    "[WIYT Store] Unsupported schema version {} in '{}'.",
                    a_package.schemaVersion,
                    a_package.path.string());
                return false;
            }
            Database database;
            if (!OpenDatabase(
                    a_package.path / "package.db",
                    database)) {
                return false;
            }
            Statement currentVersion;
            if (!Prepare(
                    database.handle,
                    "PRAGMA user_version",
                    currentVersion,
                    "read migration version") ||
                sqlite3_step(currentVersion.handle) != SQLITE_ROW) {
                return false;
            }
            const auto databaseVersion =
                sqlite3_column_int(currentVersion.handle, 0);
            sqlite3_finalize(currentVersion.handle);
            currentVersion.handle = nullptr;
            if (databaseVersion == Store::kSchemaVersion) {
                a_package.schemaVersion = Store::kSchemaVersion;
                return WriteManifest(a_package);
            }
            if (
                !Exec(database.handle, "BEGIN IMMEDIATE", "begin v2 migration") ||
                !Exec(
                    database.handle,
                    "ALTER TABLE requirements ADD COLUMN "
                    "prerequisite_mode INTEGER NOT NULL DEFAULT 0 "
                    "CHECK(prerequisite_mode BETWEEN 0 AND 1)",
                    "add prerequisite mode") ||
                !Exec(
                    database.handle,
                    "ALTER TABLE requirements ADD COLUMN "
                    "allow_follower_actions INTEGER NOT NULL DEFAULT 1 "
                    "CHECK(allow_follower_actions IN(0,1))",
                    "add follower authorship") ||
                !Exec(
                    database.handle,
                    "ALTER TABLE requirements ADD COLUMN "
                    "allow_summon_actions INTEGER NOT NULL DEFAULT 1 "
                    "CHECK(allow_summon_actions IN(0,1))",
                    "add summon authorship") ||
                !Exec(
                    database.handle,
                    "UPDATE metadata SET value='2' "
                    "WHERE key='schema_version'",
                    "update schema metadata") ||
                !Exec(
                    database.handle,
                    "PRAGMA user_version=2",
                    "update user version") ||
                !Exec(database.handle, "COMMIT", "commit v2 migration")) {
                Exec(database.handle, "ROLLBACK", "rollback v2 migration");
                return false;
            }
            a_package.schemaVersion = Store::kSchemaVersion;
            if (!WriteManifest(a_package)) {
                logger::error(
                    "[WIYT Store] Database migrated but manifest update "
                    "failed for '{}'.",
                    a_package.path.string());
                return false;
            }
            logger::info(
                "[WIYT Store] Migrated package '{}' from schema 1 to 2.",
                a_package.displayName);
            return true;
        }

        bool BindAndStepFilter(
            sqlite3_stmt* a_statement,
            const TitleDefinition& a_title,
            const int a_version,
            const std::size_t a_requirementPosition,
            const FilterScope a_scope,
            const std::size_t a_position,
            const BlacklistFilter& a_filter)
        {
            sqlite3_reset(a_statement);
            sqlite3_clear_bindings(a_statement);
            BindText(a_statement, 1, a_title.id);
            sqlite3_bind_int(a_statement, 2, a_version);
            sqlite3_bind_int(
                a_statement,
                3,
                static_cast<int>(a_requirementPosition));
            sqlite3_bind_int(
                a_statement,
                4,
                static_cast<int>(a_scope));
            sqlite3_bind_int(
                a_statement,
                5,
                static_cast<int>(a_position));
            BindText(a_statement, 6, a_filter.type);
            BindText(a_statement, 7, a_filter.formIDStr);
            BindText(a_statement, 8, a_filter.editorID);
            BindText(a_statement, 9, a_filter.actorValueName);
            sqlite3_bind_int(
                a_statement,
                10,
                a_filter.optionMode);
            sqlite3_bind_int(
                a_statement,
                11,
                a_filter.optionValue);
            BindText(a_statement, 12, a_filter.optionText);
            sqlite3_bind_int(
                a_statement,
                13,
                static_cast<int>(a_filter.actorValueMode));
            sqlite3_bind_int(
                a_statement,
                14,
                static_cast<int>(a_filter.comparison));
            sqlite3_bind_double(
                a_statement,
                15,
                a_filter.minimumValue);
            sqlite3_bind_double(
                a_statement,
                16,
                a_filter.maximumValue);
            sqlite3_bind_int(
                a_statement,
                17,
                a_filter.isNot ? 1 : 0);
            return sqlite3_step(a_statement) == SQLITE_DONE;
        }
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
                "[WIYT Store] Could not create '{}': {}",
                a_package.path.string(),
                error.message());
            return false;
        }
        Database database;
        if (!OpenDatabase(
                a_package.path / "package.db",
                database) ||
            !Exec(
                database.handle,
                "BEGIN IMMEDIATE",
                "begin schema") ||
            !Exec(
                database.handle,
                kCreateSchema,
                "create schema")) {
            return false;
        }
        Statement metadata;
        if (!Prepare(
                database.handle,
                "INSERT OR REPLACE INTO metadata(key,value) VALUES"
                "('package_id',?1),('schema_version','2')",
                metadata,
                "metadata")) {
            Exec(database.handle, "ROLLBACK", "rollback schema");
            return false;
        }
        BindText(metadata.handle, 1, a_package.id);
        if (sqlite3_step(metadata.handle) != SQLITE_DONE ||
            !Exec(
                database.handle,
                "PRAGMA user_version=2",
                "user version") ||
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
        _titles.clear();
        _titleIndices.clear();
        _titlesToDelete.clear();
        _packagesToDelete.clear();

        std::error_code error;
        std::filesystem::create_directories(kPackagesRoot, error);
        if (error) {
            logger::error(
                "[WIYT Store] Could not create package root: {}",
                error.message());
            return false;
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(kPackagesRoot, error)) {
            if (error || !entry.is_directory()) {
                continue;
            }
            Package package;
            package.path = entry.path();
            if (ReadManifest(
                    entry.path() / "manifest.json",
                    package) &&
                package.enabled &&
                package.schemaVersion > 0 &&
                package.schemaVersion <= kSchemaVersion) {
                if (MigratePackage(package)) {
                    _packages.push_back(std::move(package));
                }
            }
        }
        std::ranges::sort(
            _packages,
            {},
            [](const Package& a_package) {
                return a_package.path.generic_string();
            });
        std::set<std::string> packageIDs;
        std::erase_if(
            _packages,
            [&](const Package& a_package) {
                if (packageIDs.insert(a_package.id).second) {
                    return false;
                }
                logger::error(
                    "[WIYT Store] Duplicate package ID '{}' at '{}'.",
                    a_package.id,
                    a_package.path.string());
                return true;
            });

        if (!std::ranges::any_of(
                _packages,
                [](const Package& a_package) {
                    return a_package.id == kLocalPackageID;
                })) {
            Package local;
            local.id = kLocalPackageID;
            local.displayName = "Local Titles";
            local.path =
                std::filesystem::path(kPackagesRoot) / "Local_Titles";
            if (!EnsurePackage(local)) {
                return false;
            }
            _packages.push_back(std::move(local));
        }
        std::ranges::sort(
            _packages,
            {},
            &Package::id);

        bool success = true;
        std::set<std::string> titleIDs;
        std::set<std::string> publicGlobals;
        for (const auto& package : _packages) {
            const auto before = _titles.size();
            if (!LoadPackage(package)) {
                success = false;
                continue;
            }
            for (auto index = before; index < _titles.size();) {
                const auto& title = _titles[index];
                const auto uniqueTitle =
                    titleIDs.insert(title.id).second;
                const auto uniqueGlobal =
                    title.publicGlobalEditorID.empty() ||
                    publicGlobals.insert(
                        title.publicGlobalEditorID).second;
                if (uniqueTitle && uniqueGlobal) {
                    ++index;
                    continue;
                }
                logger::error(
                    "[WIYT Store] Duplicate {} '{}' in package '{}'; "
                    "the deterministic first occurrence is authoritative.",
                    uniqueTitle ? "public Global" : "title ID",
                    uniqueTitle ?
                        title.publicGlobalEditorID :
                        title.id,
                    package.id);
                _titles.erase(_titles.begin() +
                    static_cast<std::ptrdiff_t>(index));
            }
        }
        RebuildIndices();
        logger::info(
            "[WIYT Store] Loaded {} titles from {} packages.",
            _titles.size(),
            _packages.size());
        return success;
    }

    bool Store::LoadPackage(const Package& a_package)
    {
        Database database;
        if (!OpenDatabase(
                a_package.path / "package.db",
                database,
                false)) {
            return false;
        }
        if (!EnsureColumn(database.handle, "requirement_filters", "is_not",
                "INTEGER NOT NULL DEFAULT 0 CHECK(is_not IN(0,1))")) {
            return false;
        }
        Statement metadata;
        if (!Prepare(
                database.handle,
                "SELECT "
                "(SELECT value FROM metadata WHERE key='package_id'),"
                "(SELECT value FROM metadata WHERE key='schema_version'),"
                "(SELECT user_version FROM pragma_user_version)",
                metadata,
                "validate package") ||
            sqlite3_step(metadata.handle) != SQLITE_ROW ||
            ColumnText(metadata.handle, 0) != a_package.id ||
            ColumnText(metadata.handle, 1) !=
                std::to_string(kSchemaVersion) ||
            sqlite3_column_int(metadata.handle, 2) !=
                kSchemaVersion) {
            logger::error(
                "[WIYT Store] Manifest/database identity or schema "
                "mismatch at '{}'.",
                a_package.path.string());
            return false;
        }
        if (sqlite3_exec(
                database.handle,
                "PRAGMA quick_check",
                nullptr,
                nullptr,
                nullptr) != SQLITE_OK) {
            return false;
        }

        Statement titles;
        if (!Prepare(
                database.handle,
                "SELECT t.title_id,t.current_version,v.name,v.description,"
                "v.public_global_editor_id,v.enabled "
                "FROM titles t JOIN title_versions v ON "
                "v.title_id=t.title_id AND v.version=t.current_version "
                "ORDER BY t.title_id",
                titles,
                "load titles")) {
            return false;
        }
        while (sqlite3_step(titles.handle) == SQLITE_ROW) {
            TitleDefinition title;
            title.id = ColumnText(titles.handle, 0);
            title.packageID = a_package.id;
            title.version = sqlite3_column_int(titles.handle, 1);
            title.name = ColumnText(titles.handle, 2);
            title.description = ColumnText(titles.handle, 3);
            title.publicGlobalEditorID =
                ColumnText(titles.handle, 4);
            title.enabled =
                sqlite3_column_int(titles.handle, 5) != 0;

            Statement requirements;
            if (!Prepare(
                    database.handle,
                    "SELECT position,requirement_id,name,source_type,"
                    "activity_type,tracking_mode,aggregation,target_amount,"
                    "statistic_name,reference_form_id,reference_editor_id,"
                    "graph_variable_name,graph_variable_type,"
                    "filters_require_all,prerequisite_mode,"
                    "allow_follower_actions,allow_summon_actions "
                    "FROM requirements "
                    "WHERE title_id=?1 AND version=?2 ORDER BY position",
                    requirements,
                    "load requirements")) {
                return false;
            }
            BindText(requirements.handle, 1, title.id);
            sqlite3_bind_int(
                requirements.handle,
                2,
                title.version);
            while (sqlite3_step(requirements.handle) == SQLITE_ROW) {
                const auto requirementPosition =
                    sqlite3_column_int(requirements.handle, 0);
                Requirement requirement;
                requirement.id = ColumnText(requirements.handle, 1);
                requirement.name = ColumnText(requirements.handle, 2);
                requirement.source = static_cast<ProgressSource>(
                    sqlite3_column_int(requirements.handle, 3));
                requirement.activity = static_cast<ActivityType>(
                    sqlite3_column_int(requirements.handle, 4));
                requirement.trackingMode = static_cast<TrackingMode>(
                    sqlite3_column_int(requirements.handle, 5));
                requirement.aggregation = static_cast<Aggregation>(
                    sqlite3_column_int(requirements.handle, 6));
                requirement.targetAmount =
                    static_cast<float>(
                        sqlite3_column_double(
                            requirements.handle,
                            7));
                requirement.statisticName =
                    ColumnText(requirements.handle, 8);
                requirement.referenceFormID =
                    ColumnText(requirements.handle, 9);
                requirement.referenceEditorID =
                    ColumnText(requirements.handle, 10);
                requirement.graphVariableName =
                    ColumnText(requirements.handle, 11);
                requirement.graphVariableType =
                    sqlite3_column_int(requirements.handle, 12);
                requirement.filtersRequireAll =
                    sqlite3_column_int(requirements.handle, 13) != 0;
                requirement.prerequisiteMode =
                    static_cast<PrerequisiteMode>(
                        sqlite3_column_int(requirements.handle, 14));
                requirement.allowFollowerActions =
                    sqlite3_column_int(requirements.handle, 15) != 0;
                requirement.allowSummonActions =
                    sqlite3_column_int(requirements.handle, 16) != 0;

                Statement filters;
                if (!Prepare(
                        database.handle,
                        "SELECT scope,type,form_id,editor_id,"
                        "actor_value_name,option_mode,option_value,"
                        "option_text,actor_value_mode,comparison,"
                        "minimum_value,maximum_value,is_not "
                        "FROM requirement_filters WHERE title_id=?1 AND "
                        "version=?2 AND requirement_position=?3 "
                        "ORDER BY scope,position",
                        filters,
                        "load requirement filters")) {
                    return false;
                }
                BindText(filters.handle, 1, title.id);
                sqlite3_bind_int(filters.handle, 2, title.version);
                sqlite3_bind_int(
                    filters.handle,
                    3,
                    requirementPosition);
                while (sqlite3_step(filters.handle) == SQLITE_ROW) {
                    const auto scope = static_cast<FilterScope>(
                        sqlite3_column_int(filters.handle, 0));
                    BlacklistFilter filter;
                    filter.type = ColumnText(filters.handle, 1);
                    filter.formIDStr = ColumnText(filters.handle, 2);
                    filter.editorID = ColumnText(filters.handle, 3);
                    filter.actorValueName =
                        ColumnText(filters.handle, 4);
                    filter.optionMode =
                        sqlite3_column_int(filters.handle, 5);
                    filter.optionValue =
                        sqlite3_column_int(filters.handle, 6);
                    filter.optionText =
                        ColumnText(filters.handle, 7);
                    filter.actorValueMode =
                        static_cast<ActorValueMode>(
                            sqlite3_column_int(filters.handle, 8));
                    filter.comparison =
                        static_cast<NumericComparison>(
                            sqlite3_column_int(filters.handle, 9));
                    filter.minimumValue = static_cast<float>(
                        sqlite3_column_double(filters.handle, 10));
                    filter.maximumValue = static_cast<float>(
                        sqlite3_column_double(filters.handle, 11));
                    filter.isNot =
                        sqlite3_column_int(filters.handle, 12) != 0;
                    switch (scope) {
                    case FilterScope::kPlayerPrerequisite:
                        requirement.playerPrerequisiteFilters.push_back(
                            std::move(filter));
                        break;
                    case FilterScope::kTargetActor:
                        requirement.targetActorFilters.push_back(
                            std::move(filter));
                        break;
                    case FilterScope::kSourceForm:
                        requirement.sourceFormFilters.push_back(
                            std::move(filter));
                        break;
                    case FilterScope::kEnvironment:
                        requirement.environmentFilters.push_back(
                            std::move(filter));
                        break;
                    }
                }
                title.requirements.push_back(
                    std::move(requirement));
            }

            Statement groups;
            if (!Prepare(
                    database.handle,
                    "SELECT position,name,exclusive_group,chance "
                    "FROM reward_groups WHERE title_id=?1 AND version=?2 "
                    "ORDER BY position",
                    groups,
                    "load reward groups")) {
                return false;
            }
            BindText(groups.handle, 1, title.id);
            sqlite3_bind_int(groups.handle, 2, title.version);
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
                        "WHERE title_id=?1 AND version=?2 AND "
                        "group_position=?3 ORDER BY position",
                        rewards,
                        "load rewards")) {
                    return false;
                }
                BindText(rewards.handle, 1, title.id);
                sqlite3_bind_int(rewards.handle, 2, title.version);
                sqlite3_bind_int(
                    rewards.handle,
                    3,
                    groupPosition);
                while (sqlite3_step(rewards.handle) == SQLITE_ROW) {
                    Reward reward;
                    reward.typeReward =
                        ColumnText(rewards.handle, 0);
                    reward.formIDStr =
                        ColumnText(rewards.handle, 1);
                    reward.editorID =
                        ColumnText(rewards.handle, 2);
                    reward.amount = static_cast<std::uint32_t>(
                        sqlite3_column_int(rewards.handle, 3));
                    reward.chanceReward = static_cast<float>(
                        sqlite3_column_double(rewards.handle, 4));
                    reward.functionOnType =
                        sqlite3_column_int(rewards.handle, 5);
                    reward.isPersistent =
                        sqlite3_column_int(rewards.handle, 6) != 0;
                    group.rewards.push_back(std::move(reward));
                }
                title.rewardGroups.push_back(std::move(group));
            }
            title.lastSavedHash = title.CalculateHash();
            _titles.push_back(std::move(title));
        }
        return true;
    }

    bool Store::SaveTitle(TitleDefinition& a_title)
    {
        const auto package = std::ranges::find(
            _packages,
            a_title.packageID,
            &Package::id);
        if (package == _packages.end()) {
            return false;
        }
        Database database;
        if (!OpenDatabase(
                package->path / "package.db",
                database) ||
            !Exec(
                database.handle,
                "BEGIN IMMEDIATE",
                "begin title save")) {
            return false;
        }
        const auto rollback = [&]() {
            Exec(database.handle, "ROLLBACK", "rollback title save");
        };
        Statement identity;
        if (!Prepare(
                database.handle,
                "INSERT INTO titles(title_id,current_version) VALUES(?1,0) "
                "ON CONFLICT(title_id) DO NOTHING",
                identity,
                "title identity")) {
            rollback();
            return false;
        }
        BindText(identity.handle, 1, a_title.id);
        if (sqlite3_step(identity.handle) != SQLITE_DONE) {
            rollback();
            return false;
        }

        Statement current;
        if (!Prepare(
                database.handle,
                "SELECT current_version FROM titles WHERE title_id=?1",
                current,
                "current title version")) {
            rollback();
            return false;
        }
        BindText(current.handle, 1, a_title.id);
        if (sqlite3_step(current.handle) != SQLITE_ROW) {
            rollback();
            return false;
        }
        const auto newVersion =
            sqlite3_column_int(current.handle, 0) + 1;

        Statement version;
        if (!Prepare(
                database.handle,
                "INSERT INTO title_versions VALUES(?1,?2,?3,?4,?5,?6)",
                version,
                "title version")) {
            rollback();
            return false;
        }
        BindText(version.handle, 1, a_title.id);
        sqlite3_bind_int(version.handle, 2, newVersion);
        BindText(version.handle, 3, a_title.name);
        BindText(version.handle, 4, a_title.description);
        BindText(
            version.handle,
            5,
            a_title.publicGlobalEditorID);
        sqlite3_bind_int(version.handle, 6, a_title.enabled);
        if (sqlite3_step(version.handle) != SQLITE_DONE) {
            rollback();
            return false;
        }

        Statement requirementStatement;
        if (!Prepare(
                database.handle,
                "INSERT INTO requirements("
                "title_id,version,position,requirement_id,name,source_type,"
                "activity_type,tracking_mode,aggregation,target_amount,"
                "statistic_name,reference_form_id,reference_editor_id,"
                "graph_variable_name,graph_variable_type,filters_require_all,"
                "prerequisite_mode,allow_follower_actions,allow_summon_actions) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
                "?15,?16,?17,?18,?19)",
                requirementStatement,
                "requirements")) {
            rollback();
            return false;
        }
        Statement filterStatement;
        if (!Prepare(
                database.handle,
                "INSERT INTO requirement_filters VALUES("
                "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17)",
                filterStatement,
                "requirement filters")) {
            rollback();
            return false;
        }
        for (std::size_t requirementIndex = 0;
             requirementIndex < a_title.requirements.size();
             ++requirementIndex) {
            auto& requirement =
                a_title.requirements[requirementIndex];
            if (requirement.id.empty()) {
                requirement.id = GenerateUUID();
            }
            requirement.targetAmount =
                std::max(0.0001f, requirement.targetAmount);
            sqlite3_reset(requirementStatement.handle);
            sqlite3_clear_bindings(requirementStatement.handle);
            BindText(requirementStatement.handle, 1, a_title.id);
            sqlite3_bind_int(
                requirementStatement.handle,
                2,
                newVersion);
            sqlite3_bind_int(
                requirementStatement.handle,
                3,
                static_cast<int>(requirementIndex));
            BindText(
                requirementStatement.handle,
                4,
                requirement.id);
            BindText(
                requirementStatement.handle,
                5,
                requirement.name);
            sqlite3_bind_int(
                requirementStatement.handle,
                6,
                static_cast<int>(requirement.source));
            sqlite3_bind_int(
                requirementStatement.handle,
                7,
                static_cast<int>(requirement.activity));
            sqlite3_bind_int(
                requirementStatement.handle,
                8,
                static_cast<int>(requirement.trackingMode));
            sqlite3_bind_int(
                requirementStatement.handle,
                9,
                static_cast<int>(requirement.aggregation));
            sqlite3_bind_double(
                requirementStatement.handle,
                10,
                requirement.targetAmount);
            BindText(
                requirementStatement.handle,
                11,
                requirement.statisticName);
            BindText(
                requirementStatement.handle,
                12,
                requirement.referenceFormID);
            BindText(
                requirementStatement.handle,
                13,
                requirement.referenceEditorID);
            BindText(
                requirementStatement.handle,
                14,
                requirement.graphVariableName);
            sqlite3_bind_int(
                requirementStatement.handle,
                15,
                requirement.graphVariableType);
            sqlite3_bind_int(
                requirementStatement.handle,
                16,
                requirement.filtersRequireAll);
            sqlite3_bind_int(
                requirementStatement.handle,
                17,
                static_cast<int>(requirement.prerequisiteMode));
            sqlite3_bind_int(
                requirementStatement.handle,
                18,
                requirement.allowFollowerActions);
            sqlite3_bind_int(
                requirementStatement.handle,
                19,
                requirement.allowSummonActions);
            if (sqlite3_step(requirementStatement.handle) !=
                SQLITE_DONE) {
                rollback();
                return false;
            }
            const auto saveFilters = [&](
                                         const FilterScope a_scope,
                                         const auto& a_filters) {
                for (std::size_t index = 0;
                     index < a_filters.size();
                     ++index) {
                    if (!BindAndStepFilter(
                            filterStatement.handle,
                            a_title,
                            newVersion,
                            requirementIndex,
                            a_scope,
                            index,
                            a_filters[index])) {
                        return false;
                    }
                }
                return true;
            };
            if (!saveFilters(
                    FilterScope::kPlayerPrerequisite,
                    requirement.playerPrerequisiteFilters) ||
                !saveFilters(
                    FilterScope::kTargetActor,
                    requirement.targetActorFilters) ||
                !saveFilters(
                    FilterScope::kSourceForm,
                    requirement.sourceFormFilters) ||
                !saveFilters(
                    FilterScope::kEnvironment,
                    requirement.environmentFilters)) {
                rollback();
                return false;
            }
        }

        Statement groupStatement;
        Statement rewardStatement;
        if (!Prepare(
                database.handle,
                "INSERT INTO reward_groups VALUES(?1,?2,?3,?4,?5,?6)",
                groupStatement,
                "reward groups") ||
            !Prepare(
                database.handle,
                "INSERT INTO rewards VALUES("
                "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                rewardStatement,
                "rewards")) {
            rollback();
            return false;
        }
        for (std::size_t groupIndex = 0;
             groupIndex < a_title.rewardGroups.size();
             ++groupIndex) {
            const auto& group = a_title.rewardGroups[groupIndex];
            sqlite3_reset(groupStatement.handle);
            sqlite3_clear_bindings(groupStatement.handle);
            BindText(groupStatement.handle, 1, a_title.id);
            sqlite3_bind_int(groupStatement.handle, 2, newVersion);
            sqlite3_bind_int(
                groupStatement.handle,
                3,
                static_cast<int>(groupIndex));
            BindText(groupStatement.handle, 4, group.name);
            sqlite3_bind_int(
                groupStatement.handle,
                5,
                group.isExclusive);
            sqlite3_bind_double(
                groupStatement.handle,
                6,
                group.chanceGroup);
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
                BindText(rewardStatement.handle, 1, a_title.id);
                sqlite3_bind_int(rewardStatement.handle, 2, newVersion);
                sqlite3_bind_int(
                    rewardStatement.handle,
                    3,
                    static_cast<int>(groupIndex));
                sqlite3_bind_int(
                    rewardStatement.handle,
                    4,
                    static_cast<int>(rewardIndex));
                BindText(
                    rewardStatement.handle,
                    5,
                    reward.typeReward);
                BindText(
                    rewardStatement.handle,
                    6,
                    reward.formIDStr);
                BindText(
                    rewardStatement.handle,
                    7,
                    reward.editorID);
                sqlite3_bind_int(
                    rewardStatement.handle,
                    8,
                    static_cast<int>(reward.amount));
                sqlite3_bind_double(
                    rewardStatement.handle,
                    9,
                    reward.chanceReward);
                sqlite3_bind_int(
                    rewardStatement.handle,
                    10,
                    reward.functionOnType);
                sqlite3_bind_int(
                    rewardStatement.handle,
                    11,
                    reward.isPersistent);
                if (sqlite3_step(rewardStatement.handle) !=
                    SQLITE_DONE) {
                    rollback();
                    return false;
                }
            }
        }

        Statement update;
        if (!Prepare(
                database.handle,
                "UPDATE titles SET current_version=?2,"
                "updated_at=CURRENT_TIMESTAMP WHERE title_id=?1",
                update,
                "update title") ) {
            rollback();
            return false;
        }
        BindText(update.handle, 1, a_title.id);
        sqlite3_bind_int(update.handle, 2, newVersion);
        if (sqlite3_step(update.handle) != SQLITE_DONE) {
            rollback();
            return false;
        }
        Statement prune;
        if (!Prepare(
                database.handle,
                "DELETE FROM title_versions WHERE title_id=?1 AND "
                "version NOT IN (SELECT version FROM title_versions "
                "WHERE title_id=?1 ORDER BY version DESC LIMIT 100)",
                prune,
                "prune versions")) {
            rollback();
            return false;
        }
        BindText(prune.handle, 1, a_title.id);
        if (sqlite3_step(prune.handle) != SQLITE_DONE ||
            !Exec(database.handle, "COMMIT", "commit title")) {
            rollback();
            return false;
        }
        a_title.version = newVersion;
        a_title.lastSavedHash = a_title.CalculateHash();
        return true;
    }

    bool Store::SaveAll()
    {
        std::scoped_lock lock(_lock);
        std::set<std::string> publicGlobals;
        for (const auto& title : _titles) {
            if (_titlesToDelete.contains(title.id) ||
                _packagesToDelete.contains(title.packageID)) {
                continue;
            }
            std::string normalized = title.publicGlobalEditorID;
            std::ranges::transform(
                normalized,
                normalized.begin(),
                [](const unsigned char a_character) {
                    return static_cast<char>(
                        std::tolower(a_character));
                });
            if (!ValidateTitleDefinition(title) ||
                !publicGlobals.emplace(
                    std::move(normalized)).second) {
                logger::error(
                    "[WIYT Store] Save cancelled because title '{}' "
                    "does not pass package-wide validation.",
                    title.id);
                return false;
            }
        }
        bool success = true;
        for (auto& title : _titles) {
            if (_titlesToDelete.contains(title.id) ||
                _packagesToDelete.contains(title.packageID)) {
                continue;
            }
            if (title.IsModified() && !SaveTitle(title)) {
                success = false;
            }
        }
        for (const auto& titleID :
             std::vector<std::string>(
                 _titlesToDelete.begin(),
                 _titlesToDelete.end())) {
            if (!DeleteTitleNow(titleID)) {
                success = false;
            }
        }
        for (const auto& packageID :
             std::vector<std::string>(
                 _packagesToDelete.begin(),
                 _packagesToDelete.end())) {
            if (!DeletePackageNow(packageID)) {
                success = false;
            }
        }
        RebuildIndices();
        return success;
    }

    std::optional<std::string> Store::CreatePackage(
        const std::string_view a_displayName)
    {
        std::scoped_lock lock(_lock);
        std::string name(a_displayName);
        if (name.empty()) {
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
        std::ranges::sort(_packages, {}, &Package::id);
        return id;
    }

    TitleDefinition& Store::CreateTitle(
        const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        TitleDefinition title;
        title.id = GenerateUUID();
        const auto package = std::ranges::find(
            _packages,
            a_packageID,
            &Package::id);
        title.packageID = package != _packages.end() ?
            std::string(a_packageID) :
            std::string(kLocalPackageID);
        title.publicGlobalEditorID =
            SanitizePublicGlobalName(title.name);
        if (std::ranges::any_of(
                _titles,
                [&](const TitleDefinition& a_existing) {
                    return a_existing.publicGlobalEditorID ==
                        title.publicGlobalEditorID;
                })) {
            title.publicGlobalEditorID += "_" +
                title.id.substr(0, 8);
        }
        title.lastSavedHash.clear();
        _titles.push_back(std::move(title));
        RebuildIndices();
        return _titles.back();
    }

    bool Store::MarkTitleForDeletion(
        const std::string_view a_titleID)
    {
        std::scoped_lock lock(_lock);
        return _titleIndices.contains(std::string(a_titleID)) &&
            _titlesToDelete.emplace(a_titleID).second;
    }

    bool Store::CancelTitleDeletion(
        const std::string_view a_titleID)
    {
        std::scoped_lock lock(_lock);
        return _titlesToDelete.erase(std::string(a_titleID)) > 0;
    }

    bool Store::MarkPackageForDeletion(
        const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        if (a_packageID.empty() ||
            a_packageID == kLocalPackageID) {
            return false;
        }
        return std::ranges::any_of(
                   _packages,
                   [&](const Package& a_package) {
                       return a_package.id == a_packageID;
                   }) &&
            _packagesToDelete.emplace(a_packageID).second;
    }

    bool Store::CancelPackageDeletion(
        const std::string_view a_packageID)
    {
        std::scoped_lock lock(_lock);
        return _packagesToDelete.erase(
                   std::string(a_packageID)) >
            0;
    }

    bool Store::IsTitlePendingDeletion(
        const std::string_view a_titleID) const
    {
        std::scoped_lock lock(_lock);
        return _titlesToDelete.contains(std::string(a_titleID));
    }

    bool Store::IsPackagePendingDeletion(
        const std::string_view a_packageID) const
    {
        std::scoped_lock lock(_lock);
        return _packagesToDelete.contains(std::string(a_packageID));
    }

    bool Store::DeleteTitleNow(const std::string_view a_titleID)
    {
        const auto found = _titleIndices.find(std::string(a_titleID));
        if (found == _titleIndices.end()) {
            _titlesToDelete.erase(std::string(a_titleID));
            return true;
        }
        const auto package = std::ranges::find(
            _packages,
            _titles[found->second].packageID,
            &Package::id);
        if (package == _packages.end()) {
            return false;
        }
        Database database;
        if (!OpenDatabase(
                package->path / "package.db",
                database)) {
            return false;
        }
        Statement statement;
        if (!Prepare(
                database.handle,
                "DELETE FROM titles WHERE title_id=?1",
                statement,
                "delete title")) {
            return false;
        }
        BindText(statement.handle, 1, a_titleID);
        if (sqlite3_step(statement.handle) != SQLITE_DONE) {
            return false;
        }
        _titles.erase(
            _titles.begin() +
            static_cast<std::ptrdiff_t>(found->second));
        _titlesToDelete.erase(std::string(a_titleID));
        RebuildIndices();
        return true;
    }

    bool Store::DeletePackageNow(
        const std::string_view a_packageID)
    {
        if (a_packageID.empty() ||
            a_packageID == kLocalPackageID) {
            return false;
        }
        const auto package = std::ranges::find(
            _packages,
            a_packageID,
            &Package::id);
        if (package == _packages.end()) {
            _packagesToDelete.erase(std::string(a_packageID));
            return true;
        }
        std::error_code error;
        const auto root =
            std::filesystem::weakly_canonical(kPackagesRoot, error);
        const auto target =
            std::filesystem::weakly_canonical(package->path, error);
        if (error ||
            target == root ||
            target.parent_path() != root) {
            logger::error(
                "[WIYT Store] Refusing to delete package outside '{}'.",
                root.string());
            return false;
        }
        std::filesystem::remove_all(target, error);
        if (error) {
            logger::error(
                "[WIYT Store] Could not delete '{}': {}",
                target.string(),
                error.message());
            return false;
        }
        std::erase_if(
            _titles,
            [&](const TitleDefinition& a_title) {
                return a_title.packageID == a_packageID;
            });
        _packages.erase(package);
        _packagesToDelete.erase(std::string(a_packageID));
        RebuildIndices();
        return true;
    }

    void Store::RebuildIndices()
    {
        _titleIndices.clear();
        for (std::size_t index = 0;
             index < _titles.size();
             ++index) {
            _titleIndices.emplace(_titles[index].id, index);
        }
    }

    TitleDefinition* Store::FindTitle(
        const std::string_view a_titleID)
    {
        const auto found =
            _titleIndices.find(std::string(a_titleID));
        return found != _titleIndices.end() ?
            std::addressof(_titles[found->second]) :
            nullptr;
    }

    const TitleDefinition* Store::FindTitle(
        const std::string_view a_titleID) const
    {
        const auto found =
            _titleIndices.find(std::string(a_titleID));
        return found != _titleIndices.end() ?
            std::addressof(_titles[found->second]) :
            nullptr;
    }
}
