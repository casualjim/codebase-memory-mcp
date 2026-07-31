/*
 * test_crumbs_pipeline.c — Crumbs embedding integration regressions.
 *
 * These tests exercise the native pipeline through the Crumbs-specific
 * constructor and cleanup path. They intentionally run a full parallel index
 * against real files so ownership bugs are caught by the C test suite instead
 * of by `crumbs index` at runtime.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <crumbs/cbm_context.h>
#include <pipeline/pipeline.h>
#include <store/store.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_generated_rust_project(const char *repo) {
    if (th_write_file(TH_PATH(repo, "Cargo.toml"),
                      "[package]\n"
                      "name = \"crumbs-native-test\"\n"
                      "version = \"0.1.0\"\n"
                      "edition = \"2024\"\n"
                      "\n"
                      "[dependencies]\n"
                      "serde = \"1\"\n") != 0) {
        return -1;
    }

    if (th_write_file(TH_PATH(repo, ".crumbsignore"), "target/\n") != 0) {
        return -1;
    }

    for (int i = 0; i < 64; i++) {
        char rel[128];
        char body[2048];
        snprintf(rel, sizeof(rel), "src/module_%02d.rs", i);
        snprintf(body, sizeof(body),
                 "pub struct Config%02d {\n"
                 "    pub database_url: String,\n"
                 "    pub retry_count: usize,\n"
                 "}\n"
                 "\n"
                 "pub fn load_database_url_%02d(value: &str) -> String {\n"
                 "    normalize_database_url_%02d(value)\n"
                 "}\n"
                 "\n"
                 "pub fn normalize_database_url_%02d(value: &str) -> String {\n"
                 "    value.trim().to_owned()\n"
                 "}\n"
                 "\n"
                 "pub fn retry_count_%02d(input: usize) -> usize {\n"
                 "    input.saturating_add(%d)\n"
                 "}\n",
                 i, i, i, i, i, i);
        if (th_write_file(TH_PATH(repo, rel), body) != 0) {
            return -1;
        }
    }

    return 0;
}

TEST(crumbs_full_parallel_pipeline_run_and_free) {
    char *tmp = th_mktempdir("cbm_crumbs_pipeline");
    ASSERT_NOT_NULL(tmp);

    char repo[512];
    char db_path[512];
    snprintf(repo, sizeof(repo), "%s/repo", tmp);
    snprintf(db_path, sizeof(db_path), "%s/crumbs-codebasememory.db", tmp);
    ASSERT_EQ(th_mkdir_p(repo), 0);
    ASSERT_EQ(write_generated_rust_project(repo), 0);

    cbm_crumbs_context_options_t opts = cbm_crumbs_context_options_default();
    opts.database_path = db_path;
    cbm_crumbs_context_t *ctx = cbm_crumbs_context_new(&opts);
    ASSERT_NOT_NULL(ctx);

    cbm_pipeline_t *pipeline = cbm_pipeline_new_with_crumbs_context(repo, ctx, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);

    int rc = cbm_pipeline_run(pipeline);
    ASSERT_EQ(rc, 0);

    cbm_pipeline_free(pipeline);
    cbm_crumbs_context_free(ctx);

    cbm_store_t *store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(store);
    char *project = cbm_project_name_from_path(repo);
    ASSERT_NOT_NULL(project);
    ASSERT_GT(cbm_store_count_nodes(store, project), 64);
    ASSERT_GT(cbm_store_count_edges(store, project), 64);
    free(project);
    cbm_store_close(store);

    th_cleanup(tmp);
    PASS();
}

SUITE(crumbs_pipeline) {
    RUN_TEST(crumbs_full_parallel_pipeline_run_and_free);
}
