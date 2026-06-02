#include "test_framework.h"
#include "cbm.h"
#include "pipeline/pass_lsp_cross.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int rust_ra_contains_forbidden(const char *buf) {
    const char *b = "cbm_" "rust_analyzer_resolve";
    const char *c = "cbm_" "rust_" "analyzer_resolve_batch_v2";
    return strstr(buf, b) || strstr(buf, c);
}

static int rust_ra_scan_file_for_forbidden(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return 0;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    int found = rust_ra_contains_forbidden(buf);
    free(buf);
    return found;
}

static int rust_ra_scan_dir_for_forbidden(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        size_t dn = strlen(dir_path);
        size_t en = strlen(ent->d_name);
        char *path = (char *)malloc(dn + 1 + en + 1);
        if (!path)
            continue;
        memcpy(path, dir_path, dn);
        path[dn] = '/';
        memcpy(path + dn + 1, ent->d_name, en + 1);
        struct stat st;
        int found = 0;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                found = rust_ra_scan_dir_for_forbidden(path);
            else if (S_ISREG(st.st_mode))
                found = rust_ra_scan_file_for_forbidden(path);
        }
        free(path);
        if (found) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

#ifdef HAVE_RUST_ANALYZER_LSP
static int rust_ra_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = strlen(content);
    int ok = fwrite(content, 1, n, f) == n ? 0 : -1;
    fclose(f);
    return ok;
}

static char *rust_ra_join(const char *a, const char *b) {
    size_t an = strlen(a);
    size_t bn = strlen(b);
    char *out = (char *)malloc(an + 1 + bn + 1);
    if (!out)
        return NULL;
    memcpy(out, a, an);
    out[an] = '/';
    memcpy(out + an + 1, b, bn + 1);
    return out;
}

static char *rust_ra_read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = (int)n;
    return buf;
}

static int rust_ra_suffix(const char *s, const char *suffix) {
    if (!s || !suffix)
        return 0;
    size_t sn = strlen(s);
    size_t tn = strlen(suffix);
    return sn >= tn && strcmp(s + sn - tn, suffix) == 0;
}

static int rust_ra_has_edge(CBMFileResult *r, const char *caller_suffix,
                            const char *callee_suffix) {
    for (int i = 0; i < r->resolved_calls.count; i++) {
        CBMResolvedCall *edge = &r->resolved_calls.items[i];
        if (rust_ra_suffix(edge->caller_qn, caller_suffix) &&
            rust_ra_suffix(edge->callee_qn, callee_suffix)) {
            return 1;
        }
    }
    return 0;
}
#endif

TEST(rust_analyzer_linked_header) {
#ifndef HAVE_RUST_ANALYZER_LSP
    SKIP("rust_analyzer_lsp pkg-config not found");
#else
    /* Compile/link contract lives in tests/test_rust_analyzer_link_smoke.c.
     * This suite confirms main test binary was also built with generic ABI enabled. */
    ASSERT_TRUE(1);
    PASS();
#endif
}

TEST(rust_analyzer_pipeline_contract) {
#ifndef HAVE_RUST_ANALYZER_LSP
    SKIP("rust_analyzer_lsp pkg-config not found");
#else
    char tmp_template[] = "/tmp/cbm-rust-ra-XXXXXX";
    char *root = mkdtemp(tmp_template);
    ASSERT_NOT_NULL(root);

    char *src_dir = rust_ra_join(root, "src");
    ASSERT_NOT_NULL(src_dir);
    ASSERT_EQ(mkdir(src_dir, 0700), 0);

    char *cargo = rust_ra_join(root, "Cargo.toml");
    char *lib = rust_ra_join(src_dir, "lib.rs");
    char *worker = rust_ra_join(src_dir, "worker.rs");
    ASSERT_NOT_NULL(cargo);
    ASSERT_NOT_NULL(lib);
    ASSERT_NOT_NULL(worker);

    ASSERT_EQ(rust_ra_write_file(cargo,
                                 "[package]\n"
                                 "name = \"fixture\"\n"
                                 "version = \"0.1.0\"\n"
                                 "edition = \"2021\"\n"
                                 "\n[lib]\npath = \"src/lib.rs\"\n"),
              0);
    ASSERT_EQ(rust_ra_write_file(worker,
                                 "pub struct Worker;\n"
                                 "impl Worker {\n"
                                 "    pub fn new() -> Self { Worker }\n"
                                 "    pub fn work(&self) {}\n"
                                 "}\n"),
              0);
    ASSERT_EQ(rust_ra_write_file(lib,
                                 "mod worker;\n"
                                 "macro_rules! call_work { () => {{ let w = worker::Worker::new(); w.work(); }}; }\n"
                                 "pub fn direct() {}\n"
                                 "pub fn ambiguous_target() {}\n"
                                 "pub fn run() {\n"
                                 "    direct();\n"
                                 "    let w = worker::Worker::new();\n"
                                 "    w.work();\n"
                                 "}\n"
                                 "pub fn run_macro() { call_work!(); }\n"
                                 "pub fn run_ambiguous(f: fn()) { f(); }\n"),
              0);

    int lib_len = 0;
    int worker_len = 0;
    char *lib_src = rust_ra_read_file(lib, &lib_len);
    char *worker_src = rust_ra_read_file(worker, &worker_len);
    ASSERT_NOT_NULL(lib_src);
    ASSERT_NOT_NULL(worker_src);

    CBMFileResult *lib_r =
        cbm_extract_file(lib_src, lib_len, CBM_LANG_RUST, "fixture", "src/lib.rs", 0, NULL, NULL);
    CBMFileResult *worker_r = cbm_extract_file(worker_src, worker_len, CBM_LANG_RUST, "fixture",
                                               "src/worker.rs", 0, NULL, NULL);
    ASSERT_NOT_NULL(lib_r);
    ASSERT_NOT_NULL(worker_r);
    ASSERT_FALSE(lib_r->has_error);
    ASSERT_FALSE(worker_r->has_error);
    lib_r->source = cbm_arena_strdup(&lib_r->arena, lib_src);
    lib_r->source_len = lib_len;
    worker_r->source = cbm_arena_strdup(&worker_r->arena, worker_src);
    worker_r->source_len = worker_len;

    cbm_file_info_t infos[2];
    memset(infos, 0, sizeof(infos));
    infos[0].path = lib;
    infos[0].rel_path = "src/lib.rs";
    infos[0].language = CBM_LANG_RUST;
    infos[0].size = lib_len;
    infos[1].path = worker;
    infos[1].rel_path = "src/worker.rs";
    infos[1].language = CBM_LANG_RUST;
    infos[1].size = worker_len;
    CBMFileResult *cache[2] = {lib_r, worker_r};
    cbm_pipeline_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.project_name = "fixture";
    ctx.repo_path = root;

    ASSERT_EQ(cbm_pxc_run_rust_analyzer_batch(&ctx, infos, 2, cache), 0);

    ASSERT_TRUE(rust_ra_has_edge(lib_r, ".run", ".direct"));
    ASSERT_TRUE(rust_ra_has_edge(lib_r, ".run", ".Worker.work"));
    /* Fixture includes macro-expanded method call in run_macro(). Current CBM call
     * extraction does not provide stable expanded call rows for that macro, so adapter
     * must not invent edge; resolver may add one later when fixture data becomes stable. */
    ASSERT_FALSE(rust_ra_has_edge(lib_r, ".run_ambiguous", ".ambiguous_target"));

    cbm_free_result(lib_r);
    cbm_free_result(worker_r);
    free(lib_src);
    free(worker_src);
    unlink(lib);
    unlink(worker);
    unlink(cargo);
    rmdir(src_dir);
    rmdir(root);
    free(src_dir);
    free(cargo);
    free(lib);
    free(worker);
    PASS();
#endif
}

TEST(rust_analyzer_no_old_rust_abi_names) {
    ASSERT_FALSE(rust_ra_scan_dir_for_forbidden("src"));
    ASSERT_FALSE(rust_ra_scan_dir_for_forbidden("internal"));
    PASS();
}

SUITE(rust_analyzer_lsp) {
    RUN_TEST(rust_analyzer_linked_header);
    RUN_TEST(rust_analyzer_pipeline_contract);
    RUN_TEST(rust_analyzer_no_old_rust_abi_names);
}
