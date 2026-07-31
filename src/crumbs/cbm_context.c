#include "cbm_context.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

enum {
    CRUMBS_BIND_PROJECT = 1,
    CRUMBS_BM25_QUERY_BUF = 1024,
    CRUMBS_INIT_CAP = 16,
    CRUMBS_GROWTH = 2,
    CRUMBS_MAX_BINDS = 32,
    CRUMBS_CONNECTED_LIMIT = 16,
    CRUMBS_COL_ID = 0,
    CRUMBS_COL_PROJECT = 1,
    CRUMBS_COL_LABEL = 2,
    CRUMBS_COL_NAME = 3,
    CRUMBS_COL_QN = 4,
    CRUMBS_COL_FILE = 5,
    CRUMBS_COL_START = 6,
    CRUMBS_COL_END = 7,
    CRUMBS_COL_PROPS = 8,
    CRUMBS_COL_IN_DEG = 9,
    CRUMBS_COL_OUT_DEG = 10,
    CRUMBS_COL_RANK = 11,
};

struct cbm_crumbs_context {
    char *database_path;
    char *database_dir;
    char *app_config_dir;
    char *runtime_config_dir;
    bool semantic_enabled;
    double semantic_threshold;
    int semantic_dimensions;
    char *semantic_model;
    cbm_store_t *memory_store;
    char last_error[512];
};

static void cbm_crumbs_set_error(cbm_crumbs_context_t *ctx, const char *message) {
    if (!ctx) {
        return;
    }
    snprintf(ctx->last_error, sizeof(ctx->last_error), "%s", message ? message : "unknown error");
}

static sqlite3_destructor_type cbm_crumbs_sqlite_transient(void) {
    union {
        intptr_t i;
        sqlite3_destructor_type fn;
    } u;
    u.i = -1;
    return u.fn;
}
#define CBM_CRUMBS_SQLITE_TRANSIENT (cbm_crumbs_sqlite_transient())

typedef struct {
    const char *text;
} cbm_crumbs_bind_t;

static char *cbm_crumbs_strdup(const char *value) {
    if (!value || !value[0]) {
        return NULL;
    }

    size_t len = strlen(value);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len + 1);
    return copy;
}

cbm_crumbs_context_options_t cbm_crumbs_context_options_default(void) {
    cbm_crumbs_context_options_t options;
    memset(&options, 0, sizeof(options));
    options.semantic_threshold = -1.0;
    return options;
}

cbm_crumbs_context_t *cbm_crumbs_context_new(const cbm_crumbs_context_options_t *options) {
    cbm_crumbs_context_options_t defaults = cbm_crumbs_context_options_default();
    const cbm_crumbs_context_options_t *src = options ? options : &defaults;

    cbm_crumbs_context_t *ctx = (cbm_crumbs_context_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->database_path = cbm_crumbs_strdup(src->database_path);
    ctx->database_dir = cbm_crumbs_strdup(src->database_dir);
    ctx->app_config_dir = cbm_crumbs_strdup(src->app_config_dir);
    ctx->runtime_config_dir = cbm_crumbs_strdup(src->runtime_config_dir);
    ctx->semantic_enabled = src->semantic_enabled;
    ctx->semantic_threshold = src->semantic_threshold;
    ctx->semantic_dimensions = src->semantic_dimensions;
    ctx->semantic_model = cbm_crumbs_strdup(src->semantic_model);
    if (src->database_path && strcmp(src->database_path, ":memory:") == 0) {
        ctx->memory_store = cbm_store_open_memory();
        if (!ctx->memory_store) {
            free(ctx->database_path);
            free(ctx->database_dir);
            free(ctx->app_config_dir);
            free(ctx->runtime_config_dir);
            free(ctx->semantic_model);
            free(ctx);
            return NULL;
        }
    }

    return ctx;
}

void cbm_crumbs_context_free(cbm_crumbs_context_t *ctx) {
    if (!ctx) {
        return;
    }

    free(ctx->database_path);
    free(ctx->database_dir);
    free(ctx->app_config_dir);
    free(ctx->runtime_config_dir);
    free(ctx->semantic_model);
    if (ctx->memory_store) {
        cbm_store_close(ctx->memory_store);
    }
    free(ctx);
}

const char *cbm_crumbs_context_database_path(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->database_path : NULL;
}

const char *cbm_crumbs_context_database_dir(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->database_dir : NULL;
}

const char *cbm_crumbs_context_app_config_dir(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->app_config_dir : NULL;
}

const char *cbm_crumbs_context_runtime_config_dir(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->runtime_config_dir : NULL;
}

bool cbm_crumbs_context_semantic_enabled(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->semantic_enabled : false;
}

double cbm_crumbs_context_semantic_threshold(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->semantic_threshold : -1.0;
}

int cbm_crumbs_context_semantic_dimensions(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->semantic_dimensions : 0;
}

const char *cbm_crumbs_context_semantic_model(const cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->semantic_model : NULL;
}

bool cbm_crumbs_context_uses_memory_database(const cbm_crumbs_context_t *ctx) {
    return ctx && ctx->memory_store;
}

cbm_store_t *cbm_crumbs_context_memory_store(cbm_crumbs_context_t *ctx) {
    return ctx ? ctx->memory_store : NULL;
}

static void cbm_crumbs_store_close_if_owned(cbm_crumbs_context_t *ctx, cbm_store_t *store) {
    if (!store || (ctx && ctx->memory_store == store)) {
        return;
    }
    cbm_store_close(store);
}

int cbm_crumbs_context_project_db_path(const cbm_crumbs_context_t *ctx, const char *project,
                                       char *buf, int bufsz) {
    if (!ctx || !project || !project[0] || !buf || bufsz <= 0) {
        return -1;
    }

    int written = -1;
    if (ctx->database_path && ctx->database_path[0]) {
        written = snprintf(buf, (size_t)bufsz, "%s", ctx->database_path);
    } else if (ctx->database_dir && ctx->database_dir[0]) {
        written = snprintf(buf, (size_t)bufsz, "%s/%s.db", ctx->database_dir, project);
    } else {
        return -1;
    }

    if (written < 0 || written >= bufsz) {
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

const char *cbm_crumbs_context_last_error(const cbm_crumbs_context_t *ctx) {
    return ctx && ctx->last_error[0] ? ctx->last_error : NULL;
}

void cbm_crumbs_context_clear_error(cbm_crumbs_context_t *ctx) {
    if (ctx) {
        ctx->last_error[0] = '\0';
    }
}

static int cbm_crumbs_bind_text(sqlite3_stmt *stmt, int col, const char *value) {
    return sqlite3_bind_text(stmt, col, value, -1, CBM_CRUMBS_SQLITE_TRANSIENT);
}

static void cbm_crumbs_add_bind(cbm_crumbs_bind_t *binds, int *bind_count, const char *value) {
    binds[*bind_count].text = value;
    (*bind_count)++;
}

static int cbm_crumbs_append_where(char *where, int where_sz, int wlen, int *clause_count,
                                   const char *clause) {
    if (*clause_count > 0) {
        wlen += snprintf(where + wlen, (size_t)(where_sz - wlen), " AND ");
    }
    wlen += snprintf(where + wlen, (size_t)(where_sz - wlen), "%s", clause);
    (*clause_count)++;
    return wlen;
}

static char *cbm_crumbs_glob_to_like(const char *glob) {
    if (!glob) {
        return NULL;
    }
    size_t len = strlen(glob);
    char *out = (char *)malloc((len * 2) + 1);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (glob[i] == '*') {
            out[pos++] = '%';
        } else if (glob[i] == '?') {
            out[pos++] = '_';
        } else if (glob[i] == '%' || glob[i] == '_') {
            out[pos++] = '\\';
            out[pos++] = glob[i];
        } else {
            out[pos++] = glob[i];
        }
    }
    out[pos] = '\0';
    return out;
}

static int cbm_crumbs_bm25_build_match(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size < 2) {
        return 0;
    }

    size_t pos = 0;
    int tokens = 0;
    const char *p = query;
    while (*p) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *tok_start = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (*p >= '0' && *p <= '9') || *p == '_')) {
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        const char *sep = tokens > 0 ? " OR " : "";
        size_t sep_len = strlen(sep);
        if (pos + sep_len + tok_len + 1 >= out_size) {
            break;
        }
        memcpy(out + pos, sep, sep_len);
        pos += sep_len;
        memcpy(out + pos, tok_start, tok_len);
        pos += tok_len;
        tokens++;
    }
    out[pos] = '\0';
    return tokens;
}

static void cbm_crumbs_node_from_stmt(sqlite3_stmt *stmt, cbm_node_t *node) {
    node->id = sqlite3_column_int64(stmt, CRUMBS_COL_ID);
    node->project = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_PROJECT));
    node->label = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_LABEL));
    node->name = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_NAME));
    node->qualified_name =
        cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_QN));
    node->file_path = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_FILE));
    node->start_line = sqlite3_column_int(stmt, CRUMBS_COL_START);
    node->end_line = sqlite3_column_int(stmt, CRUMBS_COL_END);
    node->properties_json =
        cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, CRUMBS_COL_PROPS));
}

static int cbm_crumbs_fill_connected(cbm_store_t *store, cbm_search_result_t *result) {
    char **callers = NULL;
    char **callees = NULL;
    int caller_count = 0;
    int callee_count = 0;
    if (!result || result->node.id <= 0) {
        return 0;
    }

    if (cbm_store_node_neighbor_names(store, result->node.id, CRUMBS_CONNECTED_LIMIT, &callers,
                                      &caller_count, &callees, &callee_count) != 0) {
        return -1;
    }

    int total = caller_count + callee_count;
    if (total <= 0) {
        free(callers);
        free(callees);
        return 0;
    }

    const char **names = (const char **)malloc((size_t)total * sizeof(char *));
    if (!names) {
        for (int i = 0; i < caller_count; i++) {
            free(callers[i]);
        }
        for (int i = 0; i < callee_count; i++) {
            free(callees[i]);
        }
        free(callers);
        free(callees);
        return -1;
    }

    int count = 0;
    for (int i = 0; i < caller_count; i++) {
        names[count++] = callers[i];
    }
    for (int i = 0; i < callee_count; i++) {
        names[count++] = callees[i];
    }
    free(callers);
    free(callees);
    result->connected_names = names;
    result->connected_count = total;
    return 0;
}

cbm_store_t *cbm_crumbs_store_open_project(cbm_crumbs_context_t *ctx, const char *project) {
    char db_path[4096];
    cbm_crumbs_context_clear_error(ctx);
    if (cbm_crumbs_context_project_db_path(ctx, project, db_path, (int)sizeof(db_path)) != 0) {
        cbm_crumbs_set_error(ctx, "failed to resolve configured project database path");
        return NULL;
    }
    if (ctx->memory_store) {
        return ctx->memory_store;
    }
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_crumbs_set_error(ctx, "failed to open configured project database");
    }
    return store;
}

cbm_store_t *cbm_crumbs_store_open_project_query(cbm_crumbs_context_t *ctx,
                                                 const char *project) {
    char db_path[4096];
    cbm_crumbs_context_clear_error(ctx);
    if (cbm_crumbs_context_project_db_path(ctx, project, db_path, (int)sizeof(db_path)) != 0) {
        cbm_crumbs_set_error(ctx, "failed to resolve configured project database path");
        return NULL;
    }
    if (ctx->memory_store) {
        return ctx->memory_store;
    }
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    if (!store) {
        cbm_crumbs_set_error(ctx, "failed to open configured project database for query");
    }
    return store;
}

int cbm_crumbs_query_graph(cbm_crumbs_context_t *ctx, const char *project, const char *query,
                           int max_rows, cbm_cypher_result_t *out) {
    if (!ctx || !project || !query || !out) {
        cbm_crumbs_set_error(ctx, "invalid query_graph arguments");
        return -1;
    }

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }
    int rc = cbm_cypher_execute(store, query, project, max_rows, out);
    if (rc != 0) {
        cbm_crumbs_set_error(ctx, cbm_store_error(store));
    }
    cbm_crumbs_store_close_if_owned(ctx, store);
    return rc;
}

int cbm_crumbs_search_graph(cbm_crumbs_context_t *ctx, const cbm_search_params_t *params,
                            cbm_search_output_t *out) {
    if (!ctx || !params || !params->project || !out) {
        cbm_crumbs_set_error(ctx, "invalid search_graph arguments");
        return -1;
    }

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, params->project);
    if (!store) {
        return -1;
    }
    int rc = cbm_store_search(store, params, out);
    if (rc == 0 && params->include_connected) {
        for (int i = 0; i < out->count; i++) {
            if (cbm_crumbs_fill_connected(store, &out->results[i]) != 0) {
                cbm_crumbs_set_error(ctx, "failed to expand connected node names");
                cbm_store_search_free(out);
                rc = -1;
                break;
            }
        }
    }
    if (rc != 0 && !cbm_crumbs_context_last_error(ctx)) {
        cbm_crumbs_set_error(ctx, cbm_store_error(store));
    }
    cbm_crumbs_store_close_if_owned(ctx, store);
    return rc;
}

static int cbm_crumbs_search_bm25_where(const cbm_search_params_t *params, char *where,
                                        int where_size, cbm_crumbs_bind_t *binds,
                                        int *bind_count, char **like_pattern) {
    int clause_count = 0;
    int wlen = 0;
    char clause[512];
    *like_pattern = NULL;

    wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count,
                                   "nodes_fts MATCH ?1");
    (*bind_count)++;

    snprintf(clause, sizeof(clause), "n.project = ?%d", *bind_count + 1);
    wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
    cbm_crumbs_add_bind(binds, bind_count, params->project);

    if (params->label) {
        snprintf(clause, sizeof(clause), "n.label = ?%d", *bind_count + 1);
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
        cbm_crumbs_add_bind(binds, bind_count, params->label);
    } else {
        wlen = cbm_crumbs_append_where(
            where, where_size, wlen, &clause_count,
            "n.label NOT IN ('File','Folder','Module','Section','Variable','Project')");
    }

    if (params->name_pattern) {
        if (params->case_sensitive) {
            snprintf(clause, sizeof(clause), "n.name REGEXP ?%d", *bind_count + 1);
        } else {
            snprintf(clause, sizeof(clause), "iregexp(?%d, n.name)", *bind_count + 1);
        }
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
        cbm_crumbs_add_bind(binds, bind_count, params->name_pattern);
    }

    if (params->qn_pattern) {
        if (params->case_sensitive) {
            snprintf(clause, sizeof(clause), "n.qualified_name REGEXP ?%d", *bind_count + 1);
        } else {
            snprintf(clause, sizeof(clause), "iregexp(?%d, n.qualified_name)", *bind_count + 1);
        }
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
        cbm_crumbs_add_bind(binds, bind_count, params->qn_pattern);
    }

    if (params->file_pattern) {
        *like_pattern = cbm_crumbs_glob_to_like(params->file_pattern);
        if (!*like_pattern) {
            return -1;
        }
        snprintf(clause, sizeof(clause), "n.file_path LIKE ?%d", *bind_count + 1);
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
        cbm_crumbs_add_bind(binds, bind_count, *like_pattern);
    }

    if (params->relationship) {
        snprintf(clause, sizeof(clause),
                 "EXISTS(SELECT 1 FROM edges e WHERE "
                 "(e.source_id = n.id OR e.target_id = n.id) AND e.type = ?%d)",
                 *bind_count + 1);
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count, clause);
        cbm_crumbs_add_bind(binds, bind_count, params->relationship);
    }

    if (params->exclude_entry_points) {
        wlen = cbm_crumbs_append_where(where, where_size, wlen, &clause_count,
                                       "NOT (NOT EXISTS(SELECT 1 FROM edges e WHERE "
                                       "e.target_id = n.id AND e.type = 'CALLS') "
                                       "AND EXISTS(SELECT 1 FROM edges e2 WHERE "
                                       "e2.source_id = n.id AND e2.type = 'CALLS'))");
    }

    (void)wlen;
    return 0;
}

static void cbm_crumbs_apply_degree_filter(char *sql, size_t sql_size,
                                           const cbm_search_params_t *params) {
    if (params->min_degree < 0 && params->max_degree < 0) {
        return;
    }

    char inner[4096];
    snprintf(inner, sizeof(inner), "%s", sql);
    if (params->min_degree >= 0 && params->max_degree >= 0) {
        snprintf(sql, sql_size,
                 "SELECT * FROM (%s) WHERE (in_deg + out_deg) >= %d AND "
                 "(in_deg + out_deg) <= %d",
                 inner, params->min_degree, params->max_degree);
    } else if (params->min_degree >= 0) {
        snprintf(sql, sql_size, "SELECT * FROM (%s) WHERE (in_deg + out_deg) >= %d", inner,
                 params->min_degree);
    } else {
        snprintf(sql, sql_size, "SELECT * FROM (%s) WHERE (in_deg + out_deg) <= %d", inner,
                 params->max_degree);
    }
}

static int cbm_crumbs_search_bm25_execute(cbm_crumbs_context_t *ctx, cbm_store_t *store,
                                          const cbm_search_params_t *params,
                                          const char *fts_query, cbm_search_output_t *out) {
    char where[2048] = "";
    char sql[4096];
    char count_sql[4096];
    char *like_pattern = NULL;
    cbm_crumbs_bind_t binds[CRUMBS_MAX_BINDS];
    memset(binds, 0, sizeof(binds));
    binds[0].text = fts_query;
    int bind_count = 0;

    if (cbm_crumbs_search_bm25_where(params, where, (int)sizeof(where), binds, &bind_count,
                                     &like_pattern) != 0) {
        cbm_crumbs_set_error(ctx, "failed to build BM25 search filters");
        free(like_pattern);
        return -1;
    }

    const char *select_cols = "SELECT n.id, n.project, n.label, n.name, n.qualified_name, "
                              "n.file_path, n.start_line, n.end_line, n.properties, "
                              "(SELECT COUNT(*) FROM edges e WHERE e.target_id = n.id AND "
                              "e.type = 'CALLS') AS in_deg, "
                              "(SELECT COUNT(*) FROM edges e WHERE e.source_id = n.id AND "
                              "e.type = 'CALLS') AS out_deg, "
                              "(bm25(nodes_fts) - CASE "
                              "WHEN n.label IN ('Function','Method') THEN 10.0 "
                              "WHEN n.label = 'Route' THEN 8.0 "
                              "WHEN n.label IN ('Class','Interface','Type','Enum') THEN 5.0 "
                              "ELSE 0.0 END) AS rank ";
    snprintf(sql, sizeof(sql), "%s FROM nodes_fts JOIN nodes n ON n.id = nodes_fts.rowid WHERE %s",
             select_cols, where);
    cbm_crumbs_apply_degree_filter(sql, sizeof(sql), params);
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM (%s)", sql);

    int limit = params->limit > 0 ? params->limit : 200;
    int offset = params->offset > 0 ? params->offset : 0;
    char order_limit[128];
    snprintf(order_limit, sizeof(order_limit), " ORDER BY rank LIMIT %d OFFSET %d", limit, offset);
    strncat(sql, order_limit, sizeof(sql) - strlen(sql) - 1);

    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < bind_count; i++) {
            cbm_crumbs_bind_text(count_stmt, i + 1, binds[i].text);
        }
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            out->total = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        cbm_crumbs_set_error(ctx, sqlite3_errmsg(db));
        free(like_pattern);
        return -1;
    }
    for (int i = 0; i < bind_count; i++) {
        cbm_crumbs_bind_text(stmt, i + 1, binds[i].text);
    }

    int cap = CRUMBS_INIT_CAP;
    int count = 0;
    cbm_search_result_t *results = (cbm_search_result_t *)malloc((size_t)cap * sizeof(*results));
    if (!results) {
        sqlite3_finalize(stmt);
        free(like_pattern);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= CRUMBS_GROWTH;
            cbm_search_result_t *grown =
                (cbm_search_result_t *)realloc(results, (size_t)cap * sizeof(*results));
            if (!grown) {
                out->results = results;
                out->count = count;
                cbm_store_search_free(out);
                sqlite3_finalize(stmt);
                free(like_pattern);
                return -1;
            }
            results = grown;
        }
        memset(&results[count], 0, sizeof(results[count]));
        cbm_crumbs_node_from_stmt(stmt, &results[count].node);
        results[count].in_degree = sqlite3_column_int(stmt, CRUMBS_COL_IN_DEG);
        results[count].out_degree = sqlite3_column_int(stmt, CRUMBS_COL_OUT_DEG);
        if (params->include_connected && cbm_crumbs_fill_connected(store, &results[count]) != 0) {
            out->results = results;
            out->count = count + 1;
            cbm_store_search_free(out);
            sqlite3_finalize(stmt);
            free(like_pattern);
            cbm_crumbs_set_error(ctx, "failed to expand connected node names");
            return -1;
        }
        count++;
    }

    sqlite3_finalize(stmt);
    free(like_pattern);
    out->results = results;
    out->count = count;
    return 0;
}

int cbm_crumbs_search_graph_bm25(cbm_crumbs_context_t *ctx, const cbm_search_params_t *params,
                                 const char *query, cbm_search_output_t *out) {
    if (!ctx || !params || !params->project || !query || !out) {
        cbm_crumbs_set_error(ctx, "invalid BM25 search_graph arguments");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char fts_query[CRUMBS_BM25_QUERY_BUF];
    if (cbm_crumbs_bm25_build_match(query, fts_query, sizeof(fts_query)) == 0) {
        return 0;
    }

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, params->project);
    if (!store) {
        return -1;
    }
    int rc = cbm_crumbs_search_bm25_execute(ctx, store, params, fts_query, out);
    cbm_crumbs_store_close_if_owned(ctx, store);
    return rc;
}

int cbm_crumbs_get_schema(cbm_crumbs_context_t *ctx, const char *project,
                          cbm_schema_info_t *out) {
    if (!ctx || !project || !out) {
        cbm_crumbs_set_error(ctx, "invalid get_schema arguments");
        return -1;
    }

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }
    int rc = cbm_store_get_schema(store, project, out);
    if (rc != 0) {
        cbm_crumbs_set_error(ctx, cbm_store_error(store));
    }
    cbm_crumbs_store_close_if_owned(ctx, store);
    return rc;
}

int cbm_crumbs_trace_bfs(cbm_crumbs_context_t *ctx, const char *project, int64_t start_id,
                         const char *direction, const char **edge_types, int edge_type_count,
                         int max_depth, int max_results, cbm_traverse_result_t *out) {
    if (!ctx || !project || !out) {
        cbm_crumbs_set_error(ctx, "invalid trace arguments");
        return -1;
    }

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }
    int rc = cbm_store_bfs(store, start_id, direction, edge_types, edge_type_count, max_depth,
                           max_results, out);
    if (rc != 0) {
        cbm_crumbs_set_error(ctx, cbm_store_error(store));
    }
    cbm_crumbs_store_close_if_owned(ctx, store);
    return rc;
}

int cbm_crumbs_node_file_path(cbm_crumbs_context_t *ctx, const char *project, int64_t node_id,
                              char **out_file_path) {
    if (!ctx || !project || node_id <= 0 || !out_file_path) {
        cbm_crumbs_set_error(ctx, "invalid node file path arguments");
        return -1;
    }
    *out_file_path = NULL;

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }

    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT file_path FROM nodes WHERE project = ?1 AND id = ?2",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cbm_crumbs_set_error(ctx, sqlite3_errmsg(db));
        cbm_crumbs_store_close_if_owned(ctx, store);
        return -1;
    }

    cbm_crumbs_bind_text(stmt, 1, project);
    sqlite3_bind_int64(stmt, 2, node_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        cbm_crumbs_set_error(ctx, "node file path not found");
        sqlite3_finalize(stmt);
        cbm_crumbs_store_close_if_owned(ctx, store);
        return -1;
    }

    *out_file_path = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    cbm_crumbs_store_close_if_owned(ctx, store);
    if (!*out_file_path) {
        cbm_crumbs_set_error(ctx, "node file path allocation failed");
        return -1;
    }
    return 0;
}

int cbm_crumbs_list_source_files(cbm_crumbs_context_t *ctx, const char *project,
                                 char ***out_files, int *out_count) {
    if (!ctx || !project || !out_files || !out_count) {
        cbm_crumbs_set_error(ctx, "invalid source file listing arguments");
        return -1;
    }
    *out_files = NULL;
    *out_count = 0;

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }

    const char *sql =
        "SELECT DISTINCT file_path FROM nodes "
        "WHERE project = ?1 "
        "AND label IN ('File','Function','Method','Class','Interface','Type','Enum','Route') "
        "AND file_path IS NOT NULL AND file_path != '' AND file_path != '{}' "
        "ORDER BY file_path";
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cbm_crumbs_set_error(ctx, sqlite3_errmsg(db));
        cbm_crumbs_store_close_if_owned(ctx, store);
        return -1;
    }
    cbm_crumbs_bind_text(stmt, 1, project);

    int cap = CRUMBS_INIT_CAP;
    int count = 0;
    char **files = (char **)malloc((size_t)cap * sizeof(*files));
    if (!files) {
        sqlite3_finalize(stmt);
        cbm_crumbs_store_close_if_owned(ctx, store);
        cbm_crumbs_set_error(ctx, "source file list allocation failed");
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= CRUMBS_GROWTH;
            char **grown = (char **)realloc(files, (size_t)cap * sizeof(*files));
            if (!grown) {
                for (int i = 0; i < count; i++) {
                    free(files[i]);
                }
                free(files);
                sqlite3_finalize(stmt);
                cbm_crumbs_store_close_if_owned(ctx, store);
                cbm_crumbs_set_error(ctx, "source file list allocation failed");
                return -1;
            }
            files = grown;
        }
        files[count] = cbm_crumbs_strdup((const char *)sqlite3_column_text(stmt, 0));
        if (!files[count]) {
            for (int i = 0; i < count; i++) {
                free(files[i]);
            }
            free(files);
            sqlite3_finalize(stmt);
            cbm_crumbs_store_close_if_owned(ctx, store);
            cbm_crumbs_set_error(ctx, "source file path allocation failed");
            return -1;
        }
        count++;
    }

    if (rc != SQLITE_DONE) {
        for (int i = 0; i < count; i++) {
            free(files[i]);
        }
        free(files);
        cbm_crumbs_set_error(ctx, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        cbm_crumbs_store_close_if_owned(ctx, store);
        return -1;
    }

    sqlite3_finalize(stmt);
    cbm_crumbs_store_close_if_owned(ctx, store);
    if (count == 0) {
        free(files);
        return 0;
    }
    *out_files = files;
    *out_count = count;
    return 0;
}

int cbm_crumbs_resolve_snippet_node(cbm_crumbs_context_t *ctx, const char *project,
                                    const char *symbol, cbm_node_t *out_node,
                                    int *out_match_method, int *out_match_count) {
    if (!ctx || !project || !symbol || !out_node || !out_match_method || !out_match_count) {
        cbm_crumbs_set_error(ctx, "invalid snippet resolution arguments");
        return -1;
    }
    memset(out_node, 0, sizeof(*out_node));
    *out_match_method = -1;
    *out_match_count = 0;

    cbm_store_t *store = cbm_crumbs_store_open_project_query(ctx, project);
    if (!store) {
        return -1;
    }

    char suffix[CRUMBS_BM25_QUERY_BUF];
    snprintf(suffix, sizeof(suffix), "%%.%s", symbol);
    const char *sql =
        "SELECT id, project, label, name, qualified_name, file_path, start_line, end_line, "
        "properties, CASE WHEN qualified_name = ?2 THEN 0 WHEN name = ?2 THEN 1 ELSE 2 END AS match_priority "
        "FROM nodes WHERE project = ?1 AND label IN "
        "('Function','Method','Class','Interface','Type','Enum','Route') "
        "AND file_path NOT LIKE '<%' "
        "AND (qualified_name = ?2 OR name = ?2 OR qualified_name LIKE ?3) "
        "ORDER BY match_priority, qualified_name";
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cbm_crumbs_set_error(ctx, sqlite3_errmsg(db));
        cbm_crumbs_store_close_if_owned(ctx, store);
        return -1;
    }
    cbm_crumbs_bind_text(stmt, 1, project);
    cbm_crumbs_bind_text(stmt, 2, symbol);
    cbm_crumbs_bind_text(stmt, 3, suffix);

    int first_priority = -1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int priority = sqlite3_column_int(stmt, 9);
        if (first_priority < 0) {
            first_priority = priority;
            *out_match_method = priority;
            cbm_crumbs_node_from_stmt(stmt, out_node);
        }
        if (priority == first_priority) {
            (*out_match_count)++;
        } else {
            break;
        }
    }
    sqlite3_finalize(stmt);
    cbm_crumbs_store_close_if_owned(ctx, store);
    return 0;
}

void cbm_crumbs_cypher_result_free(cbm_cypher_result_t *out) {
    cbm_cypher_result_free(out);
}

void cbm_crumbs_search_output_free(cbm_search_output_t *out) {
    cbm_store_search_free(out);
}

void cbm_crumbs_schema_info_free(cbm_schema_info_t *out) {
    cbm_store_schema_free(out);
}

void cbm_crumbs_traverse_result_free(cbm_traverse_result_t *out) {
    cbm_store_traverse_free(out);
}
