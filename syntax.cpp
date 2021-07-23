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

#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <sstream>

#include "lexer.h"
#include "parser.h"
#include "syntax.h"
#include "file.h"
#include "reporter.h"

#define STATE_IDLE	0
#define STATE_NL	1
#define STATE_NL_WS	2

void ingest(ParseInfo pi) {
    struct TokenInfo tinfo, tindent;

    std::vector<int> indent_stack;
    std::string indent;

    // Processing whitespace needs some state
    Token token, nl, ws;
    int state = STATE_IDLE;

    void *parser = ParseAlloc(malloc);
    //ParseTrace(stderr, "");

    token.end = pi.fcontent->start;
    do {
        tinfo.start = token.end;

        // Grab the next token from the input file.

        // A '}' might signal resuming either a String, a RegExp, or an {} expression.
        // This sort of parser-context aware lexing is supported by fancier parser generators.
        // However, it's easy enough to do here by peeking into lemon's state.
        if (*token.end == '}') {
            if (ParseShifts(parser, TOKEN_STR_CLOSE)) {
                token = lex_dstr(token.end, pi.fcontent->end);
            } else if (ParseShifts(parser, TOKEN_REG_CLOSE)) {
                token = lex_rstr(token.end, pi.fcontent->end);
            } else {
                token = lex_wake(token.end, pi.fcontent->end);
            }
        } else {
            token = lex_wake(token.end, pi.fcontent->end);
        }

        tinfo.end = token.end;
        if (!token.ok) {
            // Complain about illegal token
            std::stringstream ss;
            ss << "syntax error; found illegal token " << tinfo
               << ", but handling it like:\n    " << symbolExample(token.id);
            pi.reporter->report(REPORT_ERROR, tinfo.location(*pi.fcontent), ss.str());
        }

        // Whitespace-induced lexical scope is inherently not context-free.
        // We need to post-process these NL WS sequences for a CFG parser generator.
        // The basic scheme is to inject INDENT/DEDENT tokens at the first WS after a NL.
        // However, we don't want to treat empty or comment-only lines as indent changes.
        switch (state) {
        case STATE_IDLE:
            if (token.id == TOKEN_WS || token.id == TOKEN_COMMENT) {
                // Do not attempt to parse whitespace or comments; discard it.
                // Whitespace wastes the lookahead token, making the grammar LR(2).
                continue;
            } else if (token.id == TOKEN_NL) {
                // Enter indent processing state machine
                pi.fcontent->newline(token.end);
                nl = token;
                tindent = tinfo;
                state = STATE_NL;
                continue;
            } else {
                break;
            }
        case STATE_NL:
            if (token.id == TOKEN_WS) {
                // Record the whitespace to process later.
                ws = token;
                state = STATE_NL_WS;
                continue;
            } else {
                ws = nl;
            }
            // no break here; fall through in the no whitespace case
        case STATE_NL_WS:
            switch (token.id) {
            case TOKEN_NL:
                // We just processed a completely empty line. Do not adjust indentation level!
                // Discard prior NL WS? sequence, and restart indentation processing at this NL.
                pi.fcontent->newline(token.end);
                nl = token;
                state = STATE_NL;
                continue;

            case TOKEN_COMMENT:
                // We just processed a comment-only line. Do not adjust indentation level!
                // Discard the entire NL WS? COMMENT sequence; following NL token restarts this FSM.
                state = STATE_IDLE;
                continue;

            default:
                // Process the whitespace for a change in indentation.
                state = STATE_IDLE;
                std::string newdent(reinterpret_cast<const char*>(nl.end), ws.end-nl.end);

                // Pop indent scope until indent is a prefix of newdent
                while (newdent.compare(0, indent.size(), indent) != 0) {
                    Parse(parser, TOKEN_DEDENT, tindent, pi);
                    indent.resize(indent_stack.back());
                    indent_stack.pop_back();
                }

                // If newdent is longer, insert an INDENT token.
                if (newdent.size() > indent.size()) {
                    Parse(parser, TOKEN_INDENT, tindent, pi);
                    indent_stack.push_back(indent.size());
                    std::swap(indent, newdent);
                }

                // Newlines are whitespace (and thus a pain to parse in LR(1)).
                // However, some constructs in wake are terminated by a newline.
                // Check if the parser can shift a newline. If so, provide it.
                if (ParseShifts(parser, TOKEN_NL)) {
                    Parse(parser, TOKEN_NL, tindent, pi);
                }

                // Fall through to normal handling of the token.
                break;
            }
            break;
        }

        if (token.id == TOKEN_EOF) {
            while (!indent_stack.empty()) {
                Parse(parser, TOKEN_DEDENT, tinfo, pi);
                indent_stack.pop_back();
            }
            if (ParseShifts(parser, TOKEN_NL)) {
                Parse(parser, TOKEN_NL, tinfo, pi);
            }
        }

        Parse(parser, token.id, tinfo, pi);
    } while (token.id != TOKEN_EOF);

    ParseFree(parser, free);

    //return globals.treeRoot;
}

Location TokenInfo::location(FileContent &fcontent) const {
    return Location(fcontent.filename.c_str(), fcontent.coordinates(start), fcontent.coordinates(end!=start?end-1:end));
}

std::ostream & operator << (std::ostream &os, TokenInfo tinfo) {
    Token token, next;

    os << "'";

    long codepoints = 0;
    for (token.end = tinfo.start; token.end < tinfo.end; token = lex_printable(token.end, tinfo.end))
        ++codepoints;

    // At most 10 chars at start and 10 chars at end
    long skip_start = (codepoints > 20) ? 9 : codepoints;
    long skip_end   = (codepoints > 20) ? codepoints-9 : codepoints;

    long codepoint = 0;
    for (token.end = tinfo.start; token.end < tinfo.end; token = next) {
        next = lex_printable(token.end, tinfo.end);
        if (codepoint < skip_start || codepoint >= skip_end) {
            if (next.ok) {
                os.write(reinterpret_cast<const char*>(token.end), next.end - token.end);
            } else {
                int code;
                switch (next.end - token.end) {
                case 1:
                    code = token.end[0];
                    break;
                case 2:
                    code =
                        ((int)token.end[0] & 0x1f) << 6 |
                        ((int)token.end[1] & 0x3f) << 0 ;
                    break;
                case 3:
                    code =
                        ((int)token.end[0] & 0x0f) << 12 |
                        ((int)token.end[1] & 0x3f) <<  6 |
                        ((int)token.end[2] & 0x3f) <<  0 ;
                    break;
                default:
                    code =
                        ((int)token.end[0] & 0x07) << 18 |
                        ((int)token.end[1] & 0x3f) << 12 |
                        ((int)token.end[2] & 0x3f) <<  6 |
                        ((int)token.end[3] & 0x3f) <<  0 ;
                    break;
                }
                if (code > 0xffff) {
                    os << "\\U" << std::hex << std::setw(8) << std::setfill('0') << code;
                } else if (code > 0xff) {
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << code;
                } else switch (code) {
                    case '\a': os << "\\a"; break;
                    case '\b': os << "\\b"; break;
                    case '\f': os << "\\f"; break;
                    case '\n': os << "\\n"; break;
                    case '\r': os << "\\r"; break;
                    case '\t': os << "\\t"; break;
                    case '\v': os << "\\v"; break;
                    default:
                        os << "\\x" << std::hex << std::setw(2) << std::setfill('0') << code;
                }
            }
        } else if (codepoint == skip_start) {
            os << "..";
        }
        ++codepoint;
    }

    os << "'";

    return os;
}

const char *symbolExample(int symbol) {
    switch (symbol) {
    case TOKEN_WS:           return "whitespace";
    case TOKEN_COMMENT:      return "#-comment";
    case TOKEN_P_BOPEN:      return "{";
    case TOKEN_P_BCLOSE:     return "}";
    case TOKEN_P_SOPEN:      return "[";
    case TOKEN_P_SCLOSE:     return "]";
    case TOKEN_KW_PACKAGE:   return "package";
    case TOKEN_ID:           return "identifier";
    case TOKEN_NL:           return "newline";
    case TOKEN_KW_FROM:      return "from";
    case TOKEN_KW_IMPORT:    return "import";
    case TOKEN_P_HOLE:       return "_";
    case TOKEN_KW_EXPORT:    return "export";
    case TOKEN_KW_DEF:       return "def";
    case TOKEN_KW_TYPE:      return "type";
    case TOKEN_KW_TOPIC:     return "topic";
    case TOKEN_KW_UNARY:     return "unary";
    case TOKEN_KW_BINARY:    return "binary";
    case TOKEN_P_EQUALS:     return "=";
    case TOKEN_OP_DOT:       return ".";
    case TOKEN_OP_QUANT:     return "quantifier";
    case TOKEN_OP_EXP:       return "^";
    case TOKEN_OP_MULDIV:    return "*/%";
    case TOKEN_OP_ADDSUB:    return "+-~";
    case TOKEN_OP_COMPARE:   return "<>";
    case TOKEN_OP_INEQUAL:   return "!=";
    case TOKEN_OP_AND:       return "&";
    case TOKEN_OP_OR:        return "|";
    case TOKEN_OP_DOLLAR:    return "$";
    case TOKEN_OP_LRARROW:   return "left-arrow";
    case TOKEN_OP_EQARROW:   return "equal-arrow";
    case TOKEN_OP_COMMA:     return ",;";
    case TOKEN_KW_GLOBAL:    return "global";
    case TOKEN_P_COLON:      return ":";
    case TOKEN_KW_PUBLISH:   return "publish";
    case TOKEN_KW_DATA:      return "data";
    case TOKEN_INDENT:       return "increased-indentation";
    case TOKEN_DEDENT:       return "decreased-indentation";
    case TOKEN_KW_TUPLE:     return "tuple";
    case TOKEN_KW_TARGET:    return "target";
    case TOKEN_P_POPEN:      return "(";
    case TOKEN_P_PCLOSE:     return ")";
    case TOKEN_STR_RAW:      return "'string'";
    case TOKEN_STR_SINGLE:   return "\"string\"";
    case TOKEN_STR_OPEN:     return "\"string{";
    case TOKEN_STR_CLOSE:    return "}string\"";
    case TOKEN_STR_MID:      return "}string{";
    case TOKEN_REG_SINGLE:   return "`regexp`";
    case TOKEN_REG_OPEN:     return "`regexp${";
    case TOKEN_REG_CLOSE:    return "}regexp`";
    case TOKEN_REG_MID:      return "}regexp{";
    case TOKEN_DOUBLE:       return "3.1415";
    case TOKEN_INTEGER:      return "42";
    case TOKEN_KW_HERE:      return "here";
    case TOKEN_KW_SUBSCRIBE: return "subscribe";
    case TOKEN_KW_PRIM:      return "prim";
    case TOKEN_KW_MATCH:     return "match";
    case TOKEN_KW_IF:        return "if";
    case TOKEN_P_BSLASH:     return "\\";
    case TOKEN_KW_THEN:      return "then";
    case TOKEN_KW_ELSE:      return "else";
    case TOKEN_KW_REQUIRE:   return "require";
    default:                 return "???";
    }
}

class ConsoleReporter : public Reporter {
  void report(int severity, Location location, const std::string &message);
};

void ConsoleReporter::report(int severity, Location location, const std::string &message) {
  std::cerr << location << ": " << message << std::endl;
}

int main(int argc, const char **argv) {
    ConsoleReporter reporter;
    ExternalFile file(reporter, argv[1]);
    ingest(ParseInfo(&file, &reporter));
    return 0;
}
