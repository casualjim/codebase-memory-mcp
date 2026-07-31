/*
 * cbm_context.h — Explicit Crumbs integration context for codebase-memory.
 *
 * This API lets embedded callers provide per-instance paths and semantic
 * options instead of relying on process-global cache/config defaults.
 */
#ifndef CBM_CRUMBS_CONTEXT_H
#define CBM_CRUMBS_CONTEXT_H

#include <stdbool.h>

#include "cypher/cypher.h"
#include "store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque Crumbs integration context. */
typedef struct cbm_crumbs_context cbm_crumbs_context_t;

/* Options used to construct a Crumbs integration context.
 * All strings are borrowed by the caller and copied by cbm_crumbs_context_new().
 */
typedef struct {
    /* Explicit graph database file path. Takes precedence over database_dir. */
    const char *database_path;

    /* Directory for context-aware project database path resolution. */
    const char *database_dir;

    /* App/global config directory supplied by the embedding application. */
    const char *app_config_dir;

    /* Runtime config/state directory supplied by the embedding application. */
    const char *runtime_config_dir;

    /* Enable semantic/vector behavior for context-aware indexing/query paths. */
    bool semantic_enabled;

    /* Optional semantic score threshold. Negative means backend default. */
    double semantic_threshold;

    /* Optional embedding dimensions. Zero means backend default. */
    int semantic_dimensions;

    /* Optional embedding/model identifier used by semantic paths. */
    const char *semantic_model;
} cbm_crumbs_context_options_t;

/* Return default options with no paths and backend-default semantic settings. */
cbm_crumbs_context_options_t cbm_crumbs_context_options_default(void);

/* Create/free an explicit Crumbs integration context. */
cbm_crumbs_context_t *cbm_crumbs_context_new(const cbm_crumbs_context_options_t *options);
void cbm_crumbs_context_free(cbm_crumbs_context_t *ctx);

/* Borrowed accessors. Returned strings are owned by ctx. */
const char *cbm_crumbs_context_database_path(const cbm_crumbs_context_t *ctx);
const char *cbm_crumbs_context_database_dir(const cbm_crumbs_context_t *ctx);
const char *cbm_crumbs_context_app_config_dir(const cbm_crumbs_context_t *ctx);
const char *cbm_crumbs_context_runtime_config_dir(const cbm_crumbs_context_t *ctx);
bool cbm_crumbs_context_semantic_enabled(const cbm_crumbs_context_t *ctx);
double cbm_crumbs_context_semantic_threshold(const cbm_crumbs_context_t *ctx);
int cbm_crumbs_context_semantic_dimensions(const cbm_crumbs_context_t *ctx);
const char *cbm_crumbs_context_semantic_model(const cbm_crumbs_context_t *ctx);
bool cbm_crumbs_context_uses_memory_database(const cbm_crumbs_context_t *ctx);
cbm_store_t *cbm_crumbs_context_memory_store(cbm_crumbs_context_t *ctx);

/* Resolve a project database path without consulting CBM default cache paths.
 * Returns 0 on success and -1 when ctx/project/buf are invalid, no database path
 * source is configured, or the resolved path would not fit in bufsz.
 */
int cbm_crumbs_context_project_db_path(const cbm_crumbs_context_t *ctx, const char *project,
                                       char *buf, int bufsz);

/* Context-aware database open helpers. */
cbm_store_t *cbm_crumbs_store_open_project(cbm_crumbs_context_t *ctx, const char *project);
cbm_store_t *cbm_crumbs_store_open_project_query(cbm_crumbs_context_t *ctx,
                                                 const char *project);

/* Last native error captured by context-aware helpers. */
const char *cbm_crumbs_context_last_error(const cbm_crumbs_context_t *ctx);
void cbm_crumbs_context_clear_error(cbm_crumbs_context_t *ctx);

/* Context-aware query/search/schema/traversal helpers for Rust FFI callers. */
int cbm_crumbs_query_graph(cbm_crumbs_context_t *ctx, const char *project, const char *query,
                           int max_rows, cbm_cypher_result_t *out);
int cbm_crumbs_search_graph(cbm_crumbs_context_t *ctx, const cbm_search_params_t *params,
                            cbm_search_output_t *out);
int cbm_crumbs_search_graph_bm25(cbm_crumbs_context_t *ctx, const cbm_search_params_t *params,
                                 const char *query, cbm_search_output_t *out);
int cbm_crumbs_get_schema(cbm_crumbs_context_t *ctx, const char *project,
                          cbm_schema_info_t *out);
int cbm_crumbs_trace_bfs(cbm_crumbs_context_t *ctx, const char *project, int64_t start_id,
                         const char *direction, const char **edge_types, int edge_type_count,
                         int max_depth, int max_results, cbm_traverse_result_t *out);
int cbm_crumbs_node_file_path(cbm_crumbs_context_t *ctx, const char *project, int64_t node_id,
                              char **out_file_path);
int cbm_crumbs_list_source_files(cbm_crumbs_context_t *ctx, const char *project,
                                 char ***out_files, int *out_count);
int cbm_crumbs_resolve_snippet_node(cbm_crumbs_context_t *ctx, const char *project,
                                    const char *symbol, cbm_node_t *out_node,
                                    int *out_match_method, int *out_match_count);

/* Free helpers for every allocated result shape exposed to Rust. */
void cbm_crumbs_cypher_result_free(cbm_cypher_result_t *out);
void cbm_crumbs_search_output_free(cbm_search_output_t *out);
void cbm_crumbs_schema_info_free(cbm_schema_info_t *out);
void cbm_crumbs_traverse_result_free(cbm_traverse_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CBM_CRUMBS_CONTEXT_H */
