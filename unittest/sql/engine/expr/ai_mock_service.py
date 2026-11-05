#!/usr/bin/env python3
"""Local protocol fixture, not the competition's official Mock AI Service.

The same deterministic algorithms serve arbitrary model names and inputs.
Only localhost is bound by default. No real model or external API is called.
Use ?case=MODE on an endpoint to inject a malformed response for a test.
"""

import argparse
import hashlib
import json
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
from urllib.parse import parse_qs, urlsplit


def embedding(text, dimension):
    seed = hashlib.sha256(text.encode("utf-8")).digest()
    values = [(seed[i % len(seed)] - 127.5) / 127.5 for i in range(dimension)]
    norm = math.sqrt(sum(value * value for value in values)) or 1.0
    return [value / norm for value in values]


def build_response(path, body, dimension=768, case="normal"):
    if case == "non-object-response":
        return []
    if path.endswith("/embeddings") or path.endswith("/api/embed"):
        values = body.get("input")
        if isinstance(values, str):
            values = [values]
        if not isinstance(values, list) or not values or not all(isinstance(v, str) for v in values):
            raise ValueError("input must be a nonempty string array")
        size = body.get("dimensions", dimension)
        if isinstance(size, bool) or not isinstance(size, int) or not 0 < size <= 65536:
            raise ValueError("dimensions must be a positive bounded integer")
        vectors = [embedding(value, size) for value in values]
        if case == "wrong-dimension":
            vectors[0] = vectors[0][:-1]
        elif case == "non-number":
            vectors[0][0] = "invalid"
        elif case == "null-element":
            vectors[0][0] = None
        elif case == "empty-vector":
            vectors[0] = []
        if path.endswith("/api/embed"):
            return {"embeddings": vectors}
        # Deliberately reverse the entries to exercise the engine's index map.
        data = [{"index": i, "embedding": vector} for i, vector in reversed(list(enumerate(vectors)))]
        if case == "duplicate-index" and len(data) > 1:
            data[-1]["index"] = data[0]["index"]
        elif case == "out-of-range":
            data[0]["index"] = len(data)
        elif case == "missing-index":
            data[0].pop("index")
        elif case == "missing-result":
            data.pop()
        elif case == "wrong-envelope":
            return {"data": {}}
        return {"data": data}
    if path.endswith("/rerank"):
        query = body.get("query")
        documents = body.get("documents")
        if not isinstance(query, str) or not isinstance(documents, list) or not all(isinstance(v, str) for v in documents):
            raise ValueError("query and document strings required")
        tokens = set(query.casefold().split())
        scores = [len(tokens.intersection(text.casefold().split())) / max(1, len(tokens)) for text in documents]
        results = [{"index": i, "relevance_score": score} for i, score in reversed(list(enumerate(scores)))]
        if case == "duplicate-index" and len(results) > 1:
            results[-1]["index"] = results[0]["index"]
        elif case == "missing-result":
            results.pop()
        elif case == "invalid-score" and results:
            results[0]["relevance_score"] = "invalid"
        elif case == "out-of-range" and results:
            results[0]["index"] = len(results)
        elif case == "missing-index" and results:
            results[0].pop("index")
        elif case == "wrong-envelope":
            return {"results": {}}
        return {"results": results}
    if path.endswith("/api/generate"):
        if body.get("stream") is not False or not isinstance(body.get("prompt"), str):
            raise ValueError("native non-streaming prompt required")
        options = body.get("options", {})
        content = body["prompt"]
    elif path.endswith("/chat/completions"):
        messages = body.get("messages")
        if not isinstance(messages, list) or not messages:
            raise ValueError("messages required")
        content = next((message.get("content") for message in reversed(messages) if message.get("role") == "user"), None)
        if not isinstance(content, str):
            raise ValueError("user content must be string")
        options = body
    else:
        raise ValueError("unsupported protocol path")
    if "temperature" in options and (isinstance(options["temperature"], bool) or not isinstance(options["temperature"], (int, float))):
        raise ValueError("temperature must be numeric")
    answer = "mock:" + content
    if case == "empty-content":
        answer = ""
    elif case == "null-content":
        answer = None
    elif case == "non-string-content":
        answer = 12
    if path.endswith("/api/generate"):
        return {"response": answer, "done": True}
    return {"choices": [{"message": {"role": "assistant", "content": answer}}]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--dimension", type=int, default=768)
    parser.add_argument("--events", type=Path, required=True)
    args = parser.parse_args()
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.reply(200, {"service": "local-ai-protocol-fixture", "official": False})

        def reply(self, status, value):
            data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 32 * 1024 * 1024:
                    raise ValueError("invalid request size")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict) or not isinstance(body.get("model"), str):
                    raise ValueError("object with model required")
                parsed = urlsplit(self.path)
                case = parse_qs(parsed.query).get("case", ["normal"])[0]
                inputs = body.get("input", body.get("documents", []))
                event = {"path": parsed.path, "case": case, "model": body["model"],
                         "input_count": len(inputs) if isinstance(inputs, list) else 1,
                         "dimensions": body.get("dimensions"), "stream": body.get("stream")}
                with lock, args.events.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event) + "\n")
                if case == "http-error":
                    self.reply(400, {"error": {"message": "injected test failure"}})
                else:
                    self.reply(200, build_response(parsed.path, body, args.dimension, case))
            except (ValueError, TypeError, KeyError) as exc:
                self.reply(400, {"error": {"message": str(exc)}})

    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
