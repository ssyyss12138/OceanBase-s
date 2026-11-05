# AI function regression checks

These checks exercise the production SQL engine and provider implementations.
`ai_mock_service.py` is a local protocol fixture, **not the competition's official
Mock AI Service**. It uses arbitrary model names and deterministic input-dependent
responses. Passing these checks does not establish a competition score.

## C++ contracts

After configuring and building seekdb with the pinned SDK, build its shared
implementation and run the GoogleTests:

```sh
cmake --build build_release --target oceanbase --parallel 4
python3 unittest/sql/engine/expr/run_ai_cpp_contracts.py \
  --build-dir build_release --output-dir /path/to/private/cpp-results
```

The runner reuses the production compiler options and links the actual engine
library and SDK GoogleTest libraries. It writes compiler output, test output and
GoogleTest XML into the specified output directory.

Coverage includes structured/nested prompt arguments, repeated placeholders,
overflowing indices, embedding response ordering and malformed envelopes,
completion response validation, native Ollama request bodies and config types.

## SQL contracts

Start a separate seekdb instance and the fixture. Keep the fixture bound to
localhost; the default SQL test port is 2883.

```sh
python3 unittest/sql/engine/expr/ai_mock_service.py \
  --events /path/to/private/mock-events.jsonl
python3 unittest/sql/engine/expr/run_ai_sql_contracts.py \
  --port 2883 --events /path/to/private/mock-events.jsonl \
  --report /path/to/private/sql-contracts.json
```

The SQL runner requires PyMySQL; `--python-path` can select an existing private
installation. It creates unique database/model/endpoint names, performs all AI
execution inside seekdb, and removes its own SQL objects after the run. It refuses
port 2881 to avoid accidentally using the existing deployment during development.

The fixture reverses indexed embedding responses and rerank results to test
mapping independently of HTTP response order. The SQL checks cover long text,
JSON config, Unicode, NULL propagation, genuine multi-input requests, multiple
batches, varying dimensions, filtering/LIMIT, stable global reranking, object ID
mapping, Ollama provider changes, malformed responses and ordinary SQL.

The existing `ai_prompt.test` and `ai_function_enhancement.test` mysqltest files
and their expected results remain unchanged. They should also be run against the
rebuilt engine. Use the organizer's service when available for official validation.
