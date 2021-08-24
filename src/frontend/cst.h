/*
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CST_H
#define CST_H

#include <vector>
#include <ostream>
#include <stdint.h>

#include "rank.h"
#include "location.h"

#define CST_APP		128
#define CST_ARITY	129
#define CST_BINARY	130
#define CST_BLOCK	131
#define CST_CASE	132
#define CST_DATA	133
#define CST_DEF		134
#define CST_EXPORT	135
#define CST_FLAG_EXPORT	136
#define CST_FLAG_GLOBAL	137
#define CST_GUARD	138
#define CST_HOLE	139
#define CST_ID		140
#define CST_IDEQ	141
#define CST_IF		142
#define CST_IMPORT	143
#define CST_INTERPOLATE	144
#define CST_KIND	145
#define CST_LAMBDA	146
#define CST_LITERAL	147
#define CST_MATCH	148
#define CST_OP		149
#define CST_PACKAGE	150
#define CST_PAREN	151
#define CST_PRIM	152
#define CST_PUBLISH	153
#define CST_REQUIRE	154
#define CST_SUBSCRIBE	155
#define CST_TARGET	156
#define CST_TOP		157
#define CST_TOPIC	158
#define CST_TUPLE	159
#define CST_TUPLE_ELT	160
#define CST_UNARY	161

#define CST_ERROR	255

class FileContent;
class CSTElement;
class CSTIterator;

struct TokenInfo {
    const uint8_t *start;
    const uint8_t *end;

    size_t size() const { return end - start; }
    Location location(const FileContent &fcontent) const;
};

std::ostream & operator << (std::ostream &os, TokenInfo token);

struct CSTNode {
    // CST_* Identifier
    unsigned id   : 8;
    // Number of NonTerminals to skip to reach sibling (always >= 1)
    unsigned size : 24;
    // Byte range covered by this node
    uint32_t begin, end;

    CSTNode(uint8_t id_, uint32_t size_, uint32_t begin_, uint32_t end_);
};

class CSTBuilder {
public:
    CSTBuilder(const FileContent &fcontent);

    void addToken(uint8_t id, TokenInfo token);

    void addNode(uint8_t id, TokenInfo begin);
    void addNode(uint8_t id, uint32_t children);
    void addNode(uint8_t id, TokenInfo begin, uint32_t children);
    void addNode(uint8_t id, uint32_t children, TokenInfo end);
    void addNode(uint8_t id, TokenInfo begin, uint32_t childen, TokenInfo end);

    void delNodes(size_t num);

private:
    const FileContent *file;
    std::vector<uint8_t> token_ids;
    std::vector<CSTNode> nodes;
    RankBuilder token_starts;

friend class CST;
};

class CST {
public:
    CST(CSTBuilder &&builder);
    CSTElement root() const;

private:
    RankSelect1Map token_starts;
    const FileContent *file;
    std::vector<uint8_t> token_ids;
    std::vector<CSTNode> nodes;

friend class CSTElement;
};

class CSTElement {
public:
    bool empty() const;
    bool isNode() const;

    uint8_t id() const;
    TokenInfo content() const;
    Location location() const;

    void nextSiblingElement();
    void nextSiblingNode();

    CSTElement firstChildElement() const;
    CSTElement firstChildNode() const;

private:
    const CST *cst;
    uint32_t node, limit;
    uint32_t token, end; // in bytes

friend class CST;
};

struct Top;
const char *dst_top(CSTElement root, Top &top);

#endif
