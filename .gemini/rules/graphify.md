# Graphify Knowledge Graph Lookup Rule

Before running broad `grep`, `rg`, or file-by-file searches across `reference/` (`SP-1-dev`, `sp1-tape-looper`, `Wakes`) or vendor/Zephyr SDK trees, **consult the local Graphify AST knowledge graph first**.

## Mandatory Lookup Workflow

1. **Broad architecture & cross-repo orientation:** Read `graphify-out/GRAPH_REPORT.md` to locate communities, god nodes, and cross-repository relationships across `sp1-drum`, `SP-1-dev`, `sp1-tape-looper`, and `Wakes`.
2. **Symbol / peripheral / ISR lookup (CLI or MCP):**
   - If the Graphify MCP server is attached (`python -m graphify.serve graphify-out/graph.json` or `graphify-mcp`), query it directly for symbol callers, callees, and shortest paths.
   - Otherwise, use the local `graphify` CLI (`~/.local/bin/graphify` or `graphify` on `PATH`):
     ```bash
     graphify query "<symbol-or-topic>" --graph graphify-out/graph.json
     graphify explain "<node-name>" --graph graphify-out/graph.json
     graphify path "<node-A>" "<node-B>" --graph graphify-out/graph.json
     ```
3. **Targeted file inspection:** Open only the specific source files and line ranges identified by the graph query.

## Keeping the Graph Fresh

Whenever new C/C++ modules are added under `src/` or `reference/` is updated, regenerate the deterministic AST graph (no LLM API key required; `--no-gitignore` ensures gitignored `reference/` remains indexed while `.graphifyignore` excludes build/binary artifacts):

```bash
graphify extract . --code-only --no-gitignore
graphify cluster-only .
graphify export html --graph graphify-out/graph.json
```
