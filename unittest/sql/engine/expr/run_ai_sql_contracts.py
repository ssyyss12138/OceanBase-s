#!/usr/bin/env python3
"""Exercise a rebuilt seekdb SQL engine against the local protocol fixture.

This is a test client. All AI function execution stays inside seekdb.
The dedicated test database and model names are generated per run.
"""

import argparse
import json
from pathlib import Path
import sys
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2883)
    parser.add_argument("--python-path", type=Path)
    parser.add_argument("--mock-url", default="http://127.0.0.1:8000")
    parser.add_argument("--events", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.port == 2881:
        raise SystemExit("Use a separate test instance, not the existing 2881 deployment")
    if args.python_path:
        sys.path.insert(0, str(args.python_path))
    import pymysql

    prefix = "ai_contract_" + uuid.uuid4().hex[:12]
    conn = pymysql.connect(host="127.0.0.1", port=args.port, user="root", password="",
                           autocommit=True, charset="utf8mb4", connect_timeout=5,
                           read_timeout=30, write_timeout=10)
    results = []
    created = []

    def query(sql, parameters=None):
        with conn.cursor() as cursor:
            cursor.execute(sql, parameters)
            return cursor.fetchall()

    def scalar(sql, parameters=None):
        return query(sql, parameters)[0][0]

    def check(name, fn):
        try:
            fn()
            results.append({"name": name, "passed": True})
            print("PASS", name, flush=True)
        except Exception as exc:
            results.append({"name": name, "passed": False, "error": str(exc)[:1000]})
            print("FAIL", name, str(exc)[:250], flush=True)

    def equal(actual, expected):
        if actual != expected:
            raise AssertionError("expected %r, got %r" % (expected, actual))

    def error(sql, parameters=None):
        try:
            query(sql, parameters)
        except pymysql.MySQLError:
            return
        raise AssertionError("query unexpectedly succeeded")

    def model(kind, provider, path, suffix, include_key=True):
        name = prefix + "_" + suffix
        endpoint = name + "_ep"
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
              (name, json.dumps({"type": kind, "model_name": "arbitrary-" + kind})))
        created.append((name, endpoint))
        definition = {"ai_model_name": name, "scope": "all", "url": args.mock_url + path,
                      "provider": provider}
        if include_key:
            definition["access_key"] = "" if provider.casefold() == "ollama" else "mock-key"
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
              (endpoint, json.dumps(definition)))
        return name

    try:
        query("SET ob_query_timeout=20000000")
        query("CREATE DATABASE `" + prefix + "`")
        query("USE `" + prefix + "`")
        version = scalar("SELECT VERSION()")
        check("ordinary SQL arithmetic and JSON remain available", lambda: equal(
            query("SELECT 2+3,JSON_UNQUOTE(JSON_EXTRACT(JSON_OBJECT('k','value'),'$.k'))"),
            ((5, "value"),)))
        complete = model("completion", "openai", "/compatible-mode/v1/chat/completions", "complete")
        embed = model("dense_embedding", "openai", "/compatible-mode/v1/embeddings", "embed")
        rerank = model("rerank", "siliconflow", "/v1/rerank", "rerank")
        ollama = model("completion", "oLlAmA", "/api/generate", "ollama")
        ollama_embed = model("dense_embedding", "OLLAMA", "/api/embed", "ollama_embed")
        ollama_no_key = model("completion", "ollama", "/api/generate", "ollama_no_key", include_key=False)

        check("prompt preserves JSON arrays and objects", lambda: equal(
            json.loads(scalar("SELECT AI_PROMPT('{0}/{0}/{1}',JSON_ARRAY(1,TRUE),JSON_OBJECT('k',2))"))["args"],
            [[1, True], {"k": 2}]))
        query("CREATE TABLE prompt_lob(template LONGTEXT, argument TEXT)")
        query("INSERT INTO prompt_lob VALUES(%s,%s)", ("{0}/{0}", "你好" * 10000))
        check("prompt preserves long LOB arguments", lambda: equal(
            json.loads(scalar("SELECT AI_PROMPT(template,argument) FROM prompt_lob"))["args"][0], "你好" * 10000))
        check("completion accepts SQL NULL config", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'hello',NULL)", (complete,)), "mock:hello"))
        check("completion accepts JSON config and nested prompt", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,AI_PROMPT('outer {0}',AI_PROMPT('inner {0}','你好')),"
                   "JSON_OBJECT('temperature',0.2,'max_tokens',16))", (complete,)), "mock:outer inner 你好"))
        check("completion expands repeated placeholders", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,AI_PROMPT('{0}/{0}/{0}',%s))", (complete, "x" * 4096)),
            "mock:" + "/".join(["x" * 4096] * 3)))
        check("completion reads long text from a table", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,argument) FROM prompt_lob", (complete,)),
            "mock:" + "你好" * 10000))
        check("prompt expands Unicode and escaped characters", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,AI_PROMPT(%s,%s))", (complete, "你好 {0}\\n", '"line\n\\tail"')),
            'mock:你好 "line\n\\tail"\\n'))
        check("completion rejects overflowing placeholder", lambda: error(
            "SELECT AI_COMPLETE(%s,AI_PROMPT('{999999999999999999999999}','x'))", (complete,)))
        check("embedding default dimension", lambda: equal(
            len(json.loads(scalar("SELECT AI_EMBED(%s,'one')", (embed,)))), 768))
        check("embedding explicit dimension", lambda: equal(
            len(json.loads(scalar("SELECT AI_EMBED(%s,'one',3)", (embed,)))), 3))
        for dimension in (0, -1, 1.5, None):
            check("embedding invalid dimension %r" % dimension,
                  lambda dimension=dimension: error("SELECT AI_EMBED(%s,'one',%s)", (embed, dimension)))
        check("embedding NULL input", lambda: equal(scalar("SELECT AI_EMBED(%s,NULL)", (embed,)), None))

        query("CREATE TABLE batch_rows(id INT PRIMARY KEY,content TEXT)")
        rows = [(i, None if i % 5 == 0 else "input-%d" % i) for i in range(200)]
        with conn.cursor() as cursor:
            cursor.executemany("INSERT INTO batch_rows VALUES(%s,%s)", rows)
        before = len(args.events.read_text().splitlines())
        def batch_check():
            output = query("SELECT id,AI_EMBED(%s,content,3) FROM batch_rows ORDER BY id", (embed,))
            equal(len(output), len(rows))
            from ai_mock_service import embedding
            for (index, vector), (expected_index, content) in zip(output, rows):
                equal(index, expected_index)
                if content is None:
                    equal(vector, None)
                else:
                    actual = json.loads(vector)
                    expected = embedding(content, 3)
                    equal(len(actual), len(expected))
                    if any(abs(a - b) > 1e-6 for a, b in zip(actual, expected)):
                        raise AssertionError("embedding assigned to wrong original row %d" % index)
        check("embedding restores rows across NULLs and multiple batches", batch_check)
        def batching_check():
            events = [json.loads(line) for line in args.events.read_text().splitlines()[before:]]
            if not any(event["input_count"] > 1 for event in events):
                raise AssertionError("no multi-input HTTP request observed")
            if any(event["input_count"] > 64 for event in events):
                raise AssertionError("batch bound exceeded")
        check("embedding actually batches HTTP requests", batching_check)
        def filtered_check():
            output = query("SELECT id,AI_EMBED(%s,content,3) FROM batch_rows WHERE MOD(id,3)<>0 ORDER BY id LIMIT 17", (embed,))
            expected = [(i, value) for i, value in rows if i % 3 != 0][:17]
            equal([row[0] for row in output], [row[0] for row in expected])
            for (_, vector), (_, content) in zip(output, expected):
                equal(vector is None, content is None)
                if content is not None:
                    from ai_mock_service import embedding
                    if any(abs(a-b) > 1e-6 for a, b in zip(json.loads(vector), embedding(content, 3))):
                        raise AssertionError("filtered embedding assigned to the wrong row")
        check("embedding filtered scan and LIMIT", filtered_check)
        def varying_dimensions():
            output = query("SELECT id,AI_EMBED(%s,content,3+MOD(id,2)) FROM batch_rows ORDER BY id LIMIT 19", (embed,))
            for index, vector in output:
                equal(None if vector is None else len(json.loads(vector)), None if index % 5 == 0 else 3 + index % 2)
        check("embedding groups varying row dimensions", varying_dimensions)

        documents = ["apple" if i % 7 == 0 else "other" for i in range(47)]
        def rerank_strings():
            output = json.loads(scalar("SELECT AI_RERANK(%s,'apple',%s,NULL)", (rerank, json.dumps(documents))))
            equal([entry["index"] for entry in output], sorted(range(47), key=lambda i: (documents[i] != "apple", i)))
        check("rerank strings global indices and stable score order across batches", rerank_strings)
        def rerank_objects():
            objects = [{"id": str(i), "text": value} for i, value in enumerate(documents)]
            output = json.loads(scalar("SELECT AI_RERANK(%s,'apple',%s,'text')", (rerank, json.dumps(objects))))
            equal([entry["id"] for entry in output], [str(i) for i in sorted(range(47), key=lambda i: (documents[i] != "apple", i))])
        check("rerank object original ID mapping", rerank_objects)
        check("ollama native non-streaming completion", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'hello')", (ollama,)), "mock:hello"))
        check("ollama native embedding", lambda: equal(
            len(json.loads(scalar("SELECT AI_EMBED(%s,'hello')", (ollama_embed,)))), 768))
        check("ollama accepts an omitted API key", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'without key')", (ollama_no_key,)), "mock:without key"))
        query("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)", (ollama + "_ep", '{"provider":"Ollama"}'))
        check("ollama provider alteration preserves operation", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'changed')", (ollama,)), "mock:changed"))
        query("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
              (ollama + "_ep", json.dumps({"url": args.mock_url + "/api/generate?case=normal"})))
        check("ollama repeated alteration preserves its empty key", lambda: equal(
            scalar("SELECT AI_COMPLETE(%s,'changed again')", (ollama,)), "mock:changed again"))

        for case in ("wrong-dimension", "non-number", "null-element", "empty-vector", "missing-index", "out-of-range", "missing-result", "wrong-envelope", "non-object-response", "http-error"):
            name = model("dense_embedding", "openai", "/compatible-mode/v1/embeddings?case=" + case, "bad_" + case.replace("-", "_"))
            check("reject embedding response " + case,
                  lambda name=name: error("SELECT AI_EMBED(%s,'one',3)", (name,)))
        duplicate = model("dense_embedding", "openai", "/compatible-mode/v1/embeddings?case=duplicate-index", "bad_duplicate")
        check("reject duplicate embedding indices in a batch", lambda: error(
            "SELECT AI_EMBED(%s,content,3) FROM batch_rows WHERE content IS NOT NULL ORDER BY id LIMIT 2", (duplicate,)))
        for case in ("empty-content", "null-content", "non-string-content", "non-object-response"):
            name = model("completion", "openai", "/compatible-mode/v1/chat/completions?case=" + case, "bad_complete_" + case.replace("-", "_"))
            check("reject completion response " + case,
                  lambda name=name: error("SELECT AI_COMPLETE(%s,'hello')", (name,)))
        for case in ("duplicate-index", "missing-index", "out-of-range", "missing-result", "invalid-score", "wrong-envelope", "non-object-response"):
            name = model("rerank", "siliconflow", "/v1/rerank?case=" + case, "bad_rerank_" + case.replace("-", "_"))
            check("reject rerank response " + case,
                  lambda name=name: error("SELECT AI_RERANK(%s,'apple','[\"apple\",\"other\"]',NULL)", (name,)))
    finally:
        for name, endpoint in reversed(created):
            for action, value in (("DROP_AI_MODEL_ENDPOINT", endpoint), ("DROP_AI_MODEL", name)):
                try:
                    query("CALL DBMS_AI_SERVICE." + action + "(%s)", (value,))
                except pymysql.MySQLError:
                    pass
        try:
            query("DROP DATABASE `" + prefix + "`")
        finally:
            conn.close()
        args.report.write_text(json.dumps({"engine_version": locals().get("version"), "checks": results,
                                          "passed": sum(item["passed"] for item in results),
                                          "failed": sum(not item["passed"] for item in results)}, indent=2) + "\n")
    if not results or any(not item["passed"] for item in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
