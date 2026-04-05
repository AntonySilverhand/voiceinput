#include "voiceinput.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// SQLite History Implementation
// ============================================================================

static const char *CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS transcriptions ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "text TEXT NOT NULL, "
    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");";

static const char *INSERT_SQL =
    "INSERT INTO transcriptions (text) VALUES (?);";

static const char *SELECT_BY_INDEX_SQL =
    "SELECT text FROM transcriptions ORDER BY id DESC LIMIT 1 OFFSET ?;";

static const char *DELETE_OLD_SQL =
    "DELETE FROM transcriptions WHERE id NOT IN ("
    "SELECT id FROM transcriptions ORDER BY id DESC LIMIT ?"
    ");";

int vi_history_init(vi_history_ctx_t *ctx, const char *db_path, int max_entries) {
    if (!ctx || !db_path) return -1;

    memset(ctx, 0, sizeof(vi_history_ctx_t));
    ctx->max_entries = max_entries;
    ctx->enabled = (max_entries > 0);

    if (!ctx->enabled) {
        return 0;
    }

    // Open database
    int rc = sqlite3_open(db_path, (sqlite3 **)&ctx->sqlite_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open history database: %s\n", sqlite3_errmsg((sqlite3 *)ctx->sqlite_db));
        sqlite3_close((sqlite3 *)ctx->sqlite_db);
        ctx->sqlite_db = NULL;
        return -1;
    }

    // Create table
    char *err_msg = NULL;
    rc = sqlite3_exec((sqlite3 *)ctx->sqlite_db, CREATE_TABLE_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to create history table: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close((sqlite3 *)ctx->sqlite_db);
        ctx->sqlite_db = NULL;
        return -1;
    }

    return 0;
}

int vi_history_add(vi_history_ctx_t *ctx, const char *text) {
    if (!ctx || !ctx->sqlite_db || !text || strlen(text) == 0) return -1;
    if (!ctx->enabled) return 0;

    sqlite3 *db = (sqlite3 *)ctx->sqlite_db;
    sqlite3_stmt *stmt;

    // Insert new entry
    int rc = sqlite3_prepare_v2(db, INSERT_SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare insert statement: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert history entry: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    // Delete old entries if over limit
    if (ctx->max_entries > 0) {
        rc = sqlite3_prepare_v2(db, DELETE_OLD_SQL, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, ctx->max_entries);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return 0;
}

int vi_history_get(vi_history_ctx_t *ctx, int index, char **text) {
    if (!ctx || !ctx->sqlite_db || !text || index < 0) return -1;
    if (!ctx->enabled) return -1;

    sqlite3 *db = (sqlite3 *)ctx->sqlite_db;
    sqlite3_stmt *stmt;

    *text = NULL;

    int rc = sqlite3_prepare_v2(db, SELECT_BY_INDEX_SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare select statement: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, index);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *result = (const char *)sqlite3_column_text(stmt, 0);
        if (result) {
            *text = strdup(result);
        }
    }

    sqlite3_finalize(stmt);

    return (*text != NULL) ? 0 : -1;
}

void vi_history_cleanup(vi_history_ctx_t *ctx) {
    if (!ctx || !ctx->sqlite_db) return;

    sqlite3_close((sqlite3 *)ctx->sqlite_db);
    ctx->sqlite_db = NULL;
    ctx->enabled = false;
}
