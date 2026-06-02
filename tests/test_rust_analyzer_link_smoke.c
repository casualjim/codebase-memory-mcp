#include <rust_analyzer_lsp.h>

int main(void) {
    RustAnalyzerResolvedCallArray out = {0};
    rust_analyzer_free_resolved_call_array(&out);
    void *symbol = (void *)&rust_analyzer_resolve_batch;
    return symbol ? 0 : 1;
}
