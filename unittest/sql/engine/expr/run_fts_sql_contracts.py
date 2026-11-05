#!/usr/bin/env python3
"""Compare real seekdb TOKENIZE/MATCH behavior before and after an optimization.

Creates and drops only a uniquely named scratch database. The snapshot records
results, including error codes, from the unmodified engine; it is not a mock.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import random
import uuid

import pymysql


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2883)
    parser.add_argument('--user', default='root')
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--concurrency', type=int, default=0)
    args = parser.parse_args()
    dbname = 'fts_contract_' + uuid.uuid4().hex[:12]
    conn = pymysql.connect(host=args.host, port=args.port, user=args.user,
                           password=os.environ.get('MYSQL_PWD', ''),
                           charset='utf8mb4', autocommit=True,
                           read_timeout=600, write_timeout=600)
    results = {}

    def record(name, sql, params=()):
        try:
            with conn.cursor() as cur:
                cur.execute(sql, params)
                rows = cur.fetchall()
                # Decimal/bytes are represented deterministically; JSON strings
                # retain their array order to catch token ordering regressions.
                results[name] = {'rows': json.loads(json.dumps(rows, default=str))}
        except pymysql.MySQLError as exc:
            results[name] = {'error': exc.args[0]}

    rng = random.Random(20261105)
    texts = ['', None, 'alpha alpha beta, Gamma! a an the',
             'A ab abc ABC abc123 123 12.34 x-y foo_bar',
             '数据库全文索引与中文分词，中华人民共和国人民大会堂。',
             'OceanBase 数据库 alpha ALPHA 稳定 database',
             '１２３ＡＢＣ 全角　空格\tline\nnext\r\n',
             '日本語テスト 한글 테스트 😀🙂 𠮷 𠀀',
             'École école naïve CAFÉ Straße STRASSE',
             'alpha\x00beta\x01gamma\x7fdelta',
             'a' * 83 + ' ' + 'b' * 84 + ' ' + 'c' * 85,
             '数据库 alpha ' * 300]
    vocabulary = ['alpha', 'beta', 'database', 'performance', 'audit',
                  '索引', '分词器', '稳定', 'OceanBase', '３４５',
                  '😀', 'école', '中国', 'test_case', 'the', 'a']
    separators = [' ', ',', '.', '-', '_', '\t', '\n', '，', '。', '\x00']
    for _ in range(48):
        texts.append(''.join(rng.choice(vocabulary) + rng.choice(separators)
                             for _ in range(rng.randrange(1, 28))))

    try:
        with conn.cursor() as cur:
            cur.execute('SET ob_query_timeout=600000000')
            cur.execute('CREATE DATABASE ' + dbname)
            cur.execute('USE ' + dbname)
        for i, text in enumerate(texts):
            for parser_name in ['ik', 'beng', 'space', 'ngram']:
                for output in ['default', 'all']:
                    record(f'token/{i}/{parser_name}/{output}',
                           'SELECT TOKENIZE(%s,%s,%s) AS tokens',
                           (text, parser_name, json.dumps([{'output': output}])))
        stopwords = ('a about an are as at be by com de en for from how i in is it la '
                     'of on or that the this to was what when where who will with und www').split()
        extra_texts = [' '.join(stopwords), ' '.join(w.upper() for w in stopwords),
                       ' '.join(w + 'x' for w in stopwords),
                       ' '.join(w + 'abcdefghijk' for w in stopwords),
                       'thé théſ ｗｉｔｈ www WWW 𝘵𝘩𝘦 ß Straße ÉCOLE æ ø ı',
                       'aA bB zZ ALPHA Alpha alpha\twith\nthe www\x00the']
        boundaries = [0x20, 0x30, 0x3a, 0x41, 0x5b, 0x61, 0x7b, 0x80,
                      0x300, 0x600, 0x3040, 0x30a0, 0x3400, 0x4e00,
                      0x9fff, 0xac00, 0xd7ff, 0xe000, 0xff10, 0xff21,
                      0xff41, 0x10000, 0x1f600, 0x20000, 0x2ffff, 0x10ffff]
        extra_texts += [' '.join(chr(c) for c in range(max(1, b-2), min(0x110000, b+3))
                                if not 0xd800 <= c <= 0xdfff) for b in boundaries]
        for i, text in enumerate(extra_texts):
            for parser_name in ['ik', 'beng', 'space', 'ngram']:
                record(f'edges/{i}/{parser_name}', 'SELECT TOKENIZE(%s,%s,%s) AS tokens',
                       (text, parser_name, '[{"output":"all"}]'))
        for charset in ['utf16', 'utf16le', 'gbk', 'latin1']:
            for parser_name in ['ik', 'beng', 'space']:
                record(f'charset/{charset}/{parser_name}',
                       'SELECT TOKENIZE(CONVERT(%s USING ' + charset + '),%s) AS tokens',
                       ('Alpha BETA 数据库 123, gamma!', parser_name))
        for mode in ['smart', 'max_word']:
            for i in [4, 5, 7, 11]:
                record(f'ik/{mode}/{i}', 'SELECT TOKENIZE(%s,%s,%s) AS tokens',
                       (texts[i], 'ik', json.dumps([
                           {'output': 'all'}, {'additional_args': [{'ik_mode': mode}]}])))
        for minimum, maximum in [(1, 84), (3, 84), (5, 10)]:
            record(f'beng/size/{minimum}/{maximum}', 'SELECT TOKENIZE(%s,%s,%s) AS tokens',
                   (texts[2] + ' ' + texts[3], 'beng', json.dumps([
                       {'output': 'all'}, {'additional_args': [
                           {'min_token_size': minimum}, {'max_token_size': maximum}]}])))

        with conn.cursor() as cur:
            cur.execute('CREATE TABLE docs (id INT PRIMARY KEY, body TEXT, '
                        'FULLTEXT INDEX fi_ik(body) WITH PARSER ik)')
            cur.execute('CREATE TABLE english_docs (id INT PRIMARY KEY, body TEXT, '
                        'FULLTEXT INDEX fi_beng(body) WITH PARSER beng)')
            for table in ['docs', 'english_docs']:
                cur.executemany('INSERT INTO ' + table + ' VALUES (%s,%s)',
                                list(enumerate(texts)))
        queries = ['alpha', 'database performance audit', '全文索引 分词器',
                   'OceanBase 稳定 database', 'missingnonexistenttoken',
                   '数据库', 'the a an', 'école', 'alpha alpha beta', '']
        for stage in ['inserted', 'mutated']:
            if stage == 'mutated':
                with conn.cursor() as cur:
                    for table in ['docs', 'english_docs']:
                        cur.execute('UPDATE ' + table + ' SET body=%s WHERE id=2',
                                    ('beta beta database 全文索引',))
                        cur.execute('DELETE FROM ' + table + ' WHERE id IN (3,4,5)')
                        cur.execute('INSERT INTO ' + table + ' VALUES (1000,%s)',
                                    ('alpha performance 数据库',))
            for table in ['docs', 'english_docs']:
                for i, query in enumerate(queries):
                    prefix = f'match/{stage}/{table}/{i}'
                    record(prefix + '/ids', 'SELECT id FROM ' + table +
                           ' WHERE MATCH(body) AGAINST(%s) ORDER BY id', (query,))
                    record(prefix + '/count', 'SELECT COUNT(*) FROM ' + table +
                           ' WHERE MATCH(body) AGAINST(%s)', (query,))
                    record(prefix + '/limit', 'SELECT COUNT(*) FROM (SELECT id FROM ' +
                           table + ' WHERE MATCH(body) AGAINST(%s) LIMIT 3) t', (query,))
                    record(prefix + '/rank', 'SELECT id,ROUND(MATCH(body) AGAINST(%s),8) '
                           'AS score FROM ' + table +
                           ' WHERE MATCH(body) AGAINST(%s) ORDER BY score DESC,id LIMIT 10',
                           (query, query))
                for predicate in ['> 0', '>= 0', '= 0', '< 0.2', '> 0.2', 'IS NULL']:
                    record(f'filter/{stage}/{table}/{predicate}', 'SELECT id FROM ' + table +
                           ' WHERE MATCH(body) AGAINST(%s) ' + predicate + ' ORDER BY id',
                           ('alpha beta',))
                record(f'filter/{stage}/{table}/or', 'SELECT id FROM ' + table +
                       ' WHERE MATCH(body) AGAINST(%s) OR MOD(id,3)=0 ORDER BY id', ('alpha beta',))
                for query in ['+alpha -beta', 'alpha beta', '"alpha beta"', 'alpha*']:
                    record(f'boolean/{stage}/{table}/{query}', 'SELECT id FROM ' +
                           table + ' WHERE MATCH(body) AGAINST(%s IN BOOLEAN MODE) ORDER BY id',
                           (query,))
    finally:
        with conn.cursor() as cur:
            cur.execute('DROP DATABASE IF EXISTS ' + dbname)
        conn.close()

    if args.concurrency:
        def concurrent_worker(worker):
            concurrent_conn = pymysql.connect(host=args.host, port=args.port,
                user=args.user, password=os.environ.get('MYSQL_PWD', ''), charset='utf8mb4', autocommit=True)
            checked = 0
            try:
                with concurrent_conn.cursor() as cur:
                    for turn in range(64):
                        i = [2, 4, 7, 8][(turn + worker) % 4]
                        for parser_name in ['ik', 'beng', 'space', 'ngram']:
                            cur.execute('SELECT TOKENIZE(%s,%s,%s) AS tokens',
                                (texts[i], parser_name, '[{"output":"all"}]'))
                            actual = json.loads(json.dumps(cur.fetchall(), default=str))
                            assert {'rows': actual} == results[f'token/{i}/{parser_name}/all']
                            checked += 1
            finally:
                concurrent_conn.close()
            return checked
        with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
            checked = sum(pool.map(concurrent_worker, range(args.concurrency)))
        print(f'PASS: {checked} concurrent TOKENIZE queries across {args.concurrency} connections')

    args.snapshot.parent.mkdir(parents=True, exist_ok=True)
    args.snapshot.write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    for name, value in results.items():
        if name.endswith('/ids') and 'rows' in value:
            prefix = name[:-4]
            count = results[prefix + '/count']
            limit = results[prefix + '/limit']
            assert count.get('rows') == [[len(value['rows'])]], prefix
            assert limit.get('rows') == [[min(3, len(value['rows']))]], prefix
    errors = sum('error' in value for value in results.values())
    print(f'{len(results)} checks recorded; {errors} returned engine error codes')
    if args.compare:
        before = json.loads(args.compare.read_text())
        differences = [name for name in sorted(before.keys() | results.keys())
                       if before.get(name) != results.get(name)]
        if differences:
            for name in differences[:20]:
                print(name, '\nbefore:', before.get(name), '\nafter:', results.get(name))
            raise SystemExit(f'{len(differences)} behavior differences')
        print('PASS: all results and error codes match the unmodified engine')


if __name__ == '__main__':
    main()
