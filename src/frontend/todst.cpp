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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <iostream>
#include <sstream>
#include <set>

#include "frontend/parser.h"
#include "frontend/expr.h"
#include "frontend/cst.h"
#include "frontend/lexer.h"
#include "frontend/diagnostic.h"
#include "frontend/sums.h"
#include "runtime/value.h"
#include "location.h"

#define ERROR(loc, stream)                   \
  do {                                       \
    std::stringstream sstr;                  \
    sstr << stream;                          \
    reporter->reportError(loc, sstr.str());  \
  } while (0)                                \

static std::string getIdentifier(CSTElement element) {
  assert (element.id() == CST_ID || element.id() == CST_OP);
  TokenInfo ti = element.firstChildElement().content();
  return relex_id(ti.start, ti.end);
}

/*
static bool expectString(Lexer &lex) {
  if (expect(LITERAL, lex)) {
    if (lex.next.expr->type == &Literal::type) {
      Literal *lit = static_cast<Literal*>(lex.next.expr.get());
      HeapObject *obj = lit->value->get();
      if (typeid(*obj) == typeid(String)) {
        return true;
      } else {
        std::ostringstream message;
        message << "Was expecting a String, but got a different literal at "
          << lex.next.location.text();
        reporter->reportError(lex.next.location, message.str());
        lex.fail = true;
        return false;
      }
    } else {
      std::ostringstream message;
      message << "Was expecting a String, but got an interpolated string at "
        << lex.next.location.text();
      reporter->reportError(lex.next.location, message.str());
      lex.fail = true;
      return false;
    }
  } else {
    return false;
  }
}

static Expr *parse_binary(int p, Lexer &lex, bool multiline);
static Expr *parse_block(Lexer &lex, bool multiline);

struct ASTState {
  bool type;  // control ':' reduction
  bool match; // allow literals
  bool topParen;
  std::vector<Expr*> guard;
  ASTState(bool type_, bool match_) : type(type_), match(match_), topParen(false) { }
};

static AST parse_unary_ast(int p, Lexer &lex, ASTState &state);
static AST parse_ast(int p, Lexer &lex, ASTState &state, AST &&lhs);
static AST parse_ast(int p, Lexer &lex, ASTState &state) {
  return parse_ast(p, lex, state, parse_unary_ast(p, lex, state));
}


static int relabel_descend(Expr *expr, int index) {
  if (!(expr->flags & FLAG_TOUCHED)) {
    expr->flags |= FLAG_TOUCHED;
    if (expr->type == &VarRef::type) {
      VarRef *ref = static_cast<VarRef*>(expr);
      if (ref->name != "_") return index;
      ++index;
      ref->name += " ";
      ref->name += std::to_string(index);
      return index;
    } else if (expr->type == &App::type) {
      App *app = static_cast<App*>(expr);
      return relabel_descend(app->val.get(), relabel_descend(app->fn.get(), index));
    } else if (expr->type == &Lambda::type) {
      Lambda *lambda = static_cast<Lambda*>(expr);
      return relabel_descend(lambda->body.get(), index);
    } else if (expr->type == &Match::type) {
      Match *match = static_cast<Match*>(expr);
      for (auto &v : match->args)
        index = relabel_descend(v.get(), index);
      return index;
    } else if (expr->type == &Ascribe::type) {
      Ascribe *ascribe = static_cast<Ascribe*>(expr);
      return relabel_descend(ascribe->body.get(), index);
    }
  }
  // noop for DefMap, Literal, Prim
  return index;
}

static Expr *relabel_anon(Expr *out) {
  int args = relabel_descend(out, 0);
  for (int index = args; index >= 1; --index)
    out = new Lambda(out->location, "_ " + std::to_string(index), out);
  return out;
}

static void precedence_error(Lexer &lex) {
  std::ostringstream message;
  message << "Lower precedence unary operator "
    << lex.id() << " must use ()s at "
    << lex.next.location.file();
  reporter->reportError(lex.next.location, message.str());
  lex.fail = true;
}
static Expr *add_literal_guards(Expr *guard, const ASTState &state) {
  for (size_t i = 0; i < state.guard.size(); ++i) {
    Expr* e = state.guard[i];
    std::string comparison("scmp");
    if (e->type == &Literal::type) {
      Literal *lit = static_cast<Literal*>(e);
      HeapObject *obj = lit->value->get();
      if (typeid(*obj) == typeid(Integer)) comparison = "icmp";
      if (typeid(*obj) == typeid(Double)) comparison = "dcmp_nan_lt";
      if (typeid(*obj) == typeid(RegExp)) comparison = "rcmp";
    }
    if (!guard) guard = new VarRef(e->location, "True@wake");

    Match *match = new Match(e->location);
    match->args.emplace_back(new App(e->location, new App(e->location,
        new Lambda(e->location, "_", new Lambda(e->location, "_", new Prim(e->location, comparison), " ")),
        e), new VarRef(e->location, "_ k" + std::to_string(i))));
    match->patterns.emplace_back(AST(e->location, "LT@wake"), new VarRef(e->location, "False@wake"), nullptr);
    match->patterns.emplace_back(AST(e->location, "GT@wake"), new VarRef(e->location, "False@wake"), nullptr);
    match->patterns.emplace_back(AST(e->location, "EQ@wake"), guard, nullptr);
    guard = match;
  }
  return guard;
}

static Expr *parse_match(int p, Lexer &lex) {
  Location location = lex.next.location;
  op_type op = op_precedence("m");
  if (op.p < p) precedence_error(lex);
  lex.consume();

  Match *out = new Match(location);
  bool repeat;

  repeat = true;
  while (repeat) {
    auto rhs = parse_binary(op.p + op.l, lex, false);
    out->args.emplace_back(rhs);

    switch (lex.next.type) {
      case OPERATOR:
      case MATCH:
      case LAMBDA:
      case ID:
      case LITERAL:
      case PRIM:
      case HERE:
      case SUBSCRIBE:
      case POPEN:
        break;
      case INDENT:
        lex.consume();
        repeat = false;
        break;
      default:
        std::ostringstream message;
        message << "Unexpected end of match definition at " << lex.next.location.text();
        reporter->reportError(lex.next.location, message.str());
        lex.fail = true;
        repeat = false;
        break;
    }
  }

  if (expect(EOL, lex)) lex.consume();

  // Process the patterns
  bool multiarg = out->args.size() > 1;
  repeat = true;
  while (repeat) {
    ASTState state(false, true);
    AST ast = multiarg
      ? parse_ast(APP_PRECEDENCE, lex, state, AST(lex.next.location))
      : parse_ast(0, lex, state);
    if (check_constructors(ast)) lex.fail = true;

    Expr *guard = nullptr;
    if (lex.next.type == IF) {
      lex.consume();
      bool eateol = lex.next.type == INDENT;
      guard = parse_block(lex, false);
      if (eateol && expect(EOL, lex)) lex.consume();
    }

    guard = add_literal_guards(guard, state);

    if (expect(EQUALS, lex)) lex.consume();
    Expr *expr = parse_block(lex, false);
    out->patterns.emplace_back(std::move(ast), expr, guard);

    switch (lex.next.type) {
      case DEDENT:
        repeat = false;
        lex.consume();
        break;
      case EOL:
        lex.consume();
        break;
      default:
        std::ostringstream message;
        message << "Unexpected end of match definition at " << lex.next.location.text();
        reporter->reportError(lex.next.location, message.str());
        lex.fail = true;
        repeat = false;
        break;
    }
  }

  out->location.end = out->patterns.back().expr->location.end;
  return out;
}

static Expr *parse_unary(int p, Lexer &lex, bool multiline) {
  TRACE("UNARY");
  if (lex.next.type == EOL && multiline) lex.consume();
  switch (lex.next.type) {
    // Unary operators
    case OPERATOR: {
      Location location = lex.next.location;
      op_type op = op_precedence(lex.id().c_str());
      if (op.p < p) precedence_error(lex);
      auto opp = new VarRef(lex.next.location, "unary " + lex.id());
      opp->flags |= FLAG_AST;
      lex.consume();
      auto rhs = parse_binary(op.p + op.l, lex, multiline);
      location.end = rhs->location.end;
      App *out = new App(location, opp, rhs);
      out->flags |= FLAG_AST;
      return out;
    }
    case MATCH: {
      return parse_match(p, lex);
    }
    case LAMBDA: {
      op_type op = op_precedence("\\");
      if (op.p < p) precedence_error(lex);
      Location region = lex.next.location;
      lex.consume();
      ASTState state(false, false);
      AST ast = parse_ast(APP_PRECEDENCE+1, lex, state);
      if (check_constructors(ast)) lex.fail = true;
      auto rhs = parse_binary(op.p + op.l, lex, multiline);
      region.end = rhs->location.end;
      Lambda *out;
      if (Lexer::isUpper(ast.name.c_str()) || Lexer::isOperator(ast.name.c_str())) {
        Match *match = new Match(region);
        match->patterns.emplace_back(std::move(ast), rhs, nullptr);
        match->args.emplace_back(new VarRef(ast.region, "_ xx"));
        out = new Lambda(region, "_ xx", match);
      } else if (ast.type) {
        DefMap *dm = new DefMap(region);
        dm->body = std::unique_ptr<Expr>(rhs);
        dm->defs.insert(std::make_pair(ast.name, DefValue(ast.region, std::unique_ptr<Expr>(
          new Ascribe(LOCATION, std::move(*ast.type), new VarRef(LOCATION, "_ typed"), ast.region)))));
        out = new Lambda(region, "_ typed", dm);
      } else {
        out = new Lambda(region, ast.name, rhs);
        out->token = ast.token;
      }
      out->flags |= FLAG_AST;
      return out;
    }
    // Terminals
    case ID: {
      Expr *out = new VarRef(lex.next.location, lex.id());
      out->flags |= FLAG_AST;
      lex.consume();
      return out;
    }
    case LITERAL: {
      Expr *out = lex.next.expr.release();
      lex.consume();
      out->flags |= FLAG_AST;
      return out;
    }
    case PRIM: {
      std::string name;
      Location location = lex.next.location;
      op_type op = op_precedence("p");
      if (op.p < p) precedence_error(lex);
      lex.consume();
      if (expectString(lex)) {
        Literal *lit = static_cast<Literal*>(lex.next.expr.get());
        name = static_cast<String*>(lit->value->get())->as_str();
        location.end = lex.next.location.end;
        lex.consume();
      } else {
        name = "bad_prim";
      }
      Prim *prim = new Prim(location, name);
      prim->flags |= FLAG_AST;
      return prim;
    }
    case HERE: {
      std::string name(lex.next.location.filename);
      std::string::size_type cut = name.find_last_of('/');
      if (cut == std::string::npos) name = "."; else name.resize(cut);
      Expr *out = new Literal(lex.next.location, String::literal(lex.heap, name), &String::typeVar);
      out->flags |= FLAG_AST;
      lex.consume();
      return out;
    }
    case SUBSCRIBE: {
      Location location = lex.next.location;
      op_type op = op_precedence("s");
      if (op.p < p) precedence_error(lex);
      lex.consume();
      auto id = get_arg_loc(lex);
      location.end = id.second.end;
      return new Subscribe(location, id.first);
    }
    case POPEN: {
      Location location = lex.next.location;
      lex.consume();
      bool eateol = lex.next.type == INDENT;
      Expr *out = parse_block(lex, multiline);
      if (eateol && expect(EOL, lex)) lex.consume();
      location.end = lex.next.location.end;
      if (expect(PCLOSE, lex)) lex.consume();
      out->location = location;
      if (out->type == &Lambda::type) out->flags |= FLAG_AST;
      return out;
    }
    case IF: {
      Location l = lex.next.location;
      op_type op = op_precedence("i");
      if (op.p < p) precedence_error(lex);
      lex.consume();
      auto condE = parse_block(lex, multiline);
      if (lex.next.type == EOL && multiline) lex.consume();
      if (expect(THEN, lex)) lex.consume();
      auto thenE = parse_block(lex, multiline);
      if (lex.next.type == EOL && multiline) lex.consume();
      if (expect(ELSE, lex)) lex.consume();
      auto elseE = parse_block(lex, multiline);
      l.end = elseE->location.end;
      Match *out = new Match(l);
      out->args.emplace_back(condE);
      out->patterns.emplace_back(AST(l, "True@wake"),  thenE, nullptr);
      out->patterns.emplace_back(AST(l, "False@wake"), elseE, nullptr);
      out->flags |= FLAG_AST;
      return out;
    }
    default: {
      std::ostringstream message;
      message << "Was expecting an (OPERATOR/LAMBDA/ID/LITERAL/PRIM/POPEN), got a "
        << symbolTable[lex.next.type] << " at "
        << lex.next.location.text();
      reporter->reportError(lex.next.location, message.str());
      lex.fail = true;
      return new Literal(LOCATION, String::literal(lex.heap, "bad unary"), &String::typeVar);
    }
  }
}

static Expr *parse_binary(int p, Lexer &lex, bool multiline) {
  TRACE("BINARY");
  auto lhs = parse_unary(p, lex, multiline);
  for (;;) {
    switch (lex.next.type) {
      case OPERATOR: {
        op_type op = op_precedence(lex.id().c_str());
        if (op.p < p) return lhs;
        auto opp = new VarRef(lex.next.location, "binary " + lex.id());
        opp->flags |= FLAG_AST;
        lex.consume();
        auto rhs = parse_binary(op.p + op.l, lex, multiline);
        Location app1_loc = lhs->location;
        Location app2_loc = lhs->location;
        app1_loc.end = opp->location.end;
        app2_loc.end = rhs->location.end;
        lhs = new App(app2_loc, new App(app1_loc, opp, lhs), rhs);
        lhs->flags |= FLAG_AST;
        break;
      }
      case COLON: {
        op_type op = op_precedence(lex.id().c_str());
        if (op.p < p) return lhs;
        lex.consume();
        ASTState state(true, false);
        AST signature = parse_ast(op.p + op.l, lex, state);
        if (check_constructors(signature)) lex.fail = true;
        Location location = lhs->location;
        location.end = signature.region.end;
        lhs = new Ascribe(location, std::move(signature), lhs, lhs->location);
        break;
      }
      case MATCH:
      case LAMBDA:
      case ID:
      case LITERAL:
      case PRIM:
      case HERE:
      case SUBSCRIBE:
      case IF:
      case POPEN: {
        op_type op = op_precedence("a"); // application
        if (op.p < p) return lhs;
        auto rhs = parse_binary(op.p + op.l, lex, multiline);
        Location location = lhs->location;
        location.end = rhs->location.end;
        lhs = new App(location, lhs, rhs);
        lhs->flags |= FLAG_AST;
        break;
      }
      case EOL: {
        if (multiline) {
          lex.consume();
          break;
        }
      }
      default: {
        return lhs;
      }
    }
  }
}

static void extract_def(std::vector<Definition> &out, long index, AST &&ast, const std::vector<ScopedTypeVar> &typeVars, Expr *body) {
  std::string key = "_ extract " + std::to_string(++index);
  out.emplace_back(key, ast.token, body, std::vector<ScopedTypeVar>(typeVars));
  for (auto &m : ast.args) {
    AST pattern(ast.region, std::string(ast.name));
    pattern.type = std::move(ast.type);
    std::string mname("_" + m.name);
    for (auto &n : ast.args) {
      pattern.args.push_back(AST(m.token, "_"));
      if (&n == &m) {
        AST &back = pattern.args.back();
        back.name = mname;
        back.type = std::move(m.type);
      }
    }
    Match *match = new Match(m.token);
    match->args.emplace_back(new VarRef(body->location, key));
    match->patterns.emplace_back(std::move(pattern), new VarRef(m.token, mname), nullptr);
    if (Lexer::isUpper(m.name.c_str()) || Lexer::isOperator(m.name.c_str())) {
      extract_def(out, index, std::move(m), typeVars, match);
    } else {
      out.emplace_back(m.name, m.token, match, std::vector<ScopedTypeVar>(typeVars));
    }
  }
}

static AST parse_unary_ast(int p, Lexer &lex, ASTState &state) {
  TRACE("UNARY_AST");
  switch (lex.next.type) {
    // Unary operators
    case OPERATOR: {
      op_type op = op_precedence(lex.id().c_str());
      if (op.p < p) precedence_error(lex);
      std::string name = "unary " + lex.id();
      Location token = lex.next.location;
      lex.consume();
      AST rhs = parse_ast(op.p + op.l, lex, state);
      std::vector<AST> args;
      args.emplace_back(std::move(rhs));
      auto out = AST(token, std::move(name), std::move(args));
      out.region.end = out.args.back().region.end;
      state.topParen = false;
      return out;
    }
    // Terminals
    case ID: {
      AST out(lex.next.location, lex.id());
      if (out.name == "_" && state.type) {
        std::ostringstream message;
        message << "Type signatures may not include _ at "
          << lex.next.location.file();
        reporter->reportError(lex.next.location, message.str());
        lex.fail = true;
      }
      lex.consume();
      return out;
    }
    case POPEN: {
      Location region = lex.next.location;
      lex.consume();
      AST out = parse_ast(0, lex, state);
      region.end = lex.next.location.end;
      if (expect(PCLOSE, lex)) lex.consume();
      out.region = region;
      state.topParen = true;
      return out;
    }
    case LITERAL: {
      if (state.match) {
        AST out(lex.next.location, "_ k" + std::to_string(state.guard.size()));
        state.guard.push_back(lex.next.expr.release());
        lex.consume();
        return out;
      }
      // fall through to default
    }
    default: {
      std::ostringstream message;
      message << "Was expecting an (OPERATOR/ID/POPEN), got a "
        << symbolTable[lex.next.type] << " at "
        << lex.next.location.text();
      reporter->reportError(lex.next.location, message.str());
      lex.consume();
      lex.fail = true;
      return AST(lex.next.location);
    }
  }
}

static AST parse_ast(int p, Lexer &lex, ASTState &state, AST &&lhs_) {
  AST lhs(std::move(lhs_));
  TRACE("AST");
  for (;;) {
    switch (lex.next.type) {
      case OPERATOR: {
        op_type op = op_precedence(lex.id().c_str());
        if (op.p < p) return lhs;
        std::string name = "binary " + lex.id();
        Location token = lex.next.location;
        lex.consume();
        auto rhs = parse_ast(op.p + op.l, lex, state);
        Location region = lhs.region;
        region.end = rhs.region.end;
        std::vector<AST> args;
        args.emplace_back(std::move(lhs));
        args.emplace_back(std::move(rhs));
        lhs = AST(token, std::move(name), std::move(args));
        lhs.region = region;
        state.topParen = false;
        break;
      }
      case LITERAL:
      case ID:
      case POPEN: {
        op_type op = op_precedence("a"); // application
        if (op.p < p) return lhs;
        AST rhs = parse_ast(op.p + op.l, lex, state);
        lhs.region.end = rhs.region.end;
        if (Lexer::isOperator(lhs.name.c_str())) {
          std::ostringstream message;
          message << "Cannot supply additional constructor arguments to " << lhs.name
            << " at " << lhs.region.text();
          reporter->reportError(lhs.region, message.str());
          lex.fail = true;
        }
        lhs.args.emplace_back(std::move(rhs));
        state.topParen = false;
        break;
      }
      case COLON: {
        op_type op = op_precedence(lex.id().c_str());
        if (op.p < p) return lhs;
        if (state.type) {
          Location tagloc = lhs.region;
          lex.consume();
          if (!lhs.args.empty() || Lexer::isOperator(lhs.name.c_str())) {
            std::ostringstream message;
            message << "Left-hand-side of COLON must be a simple lower-case identifier, not "
              << lhs.name << " at " << lhs.region.file();
            reporter->reportError(lhs.region, message.str());
            lex.fail = true;
          }
          std::string tag = std::move(lhs.name);
          lhs = parse_ast(op.p + op.l, lex, state);
          lhs.tag = std::move(tag);
          lhs.region.start = tagloc.start;
        } else {
          lex.consume();
          state.type = true;
          lhs.type = optional<AST>(new AST(parse_ast(op.p + op.l, lex, state)));
          state.type = false;
        }
        break;
      }
      default: {
        return lhs;
      }
    }
  }
}

static AST parse_type_def(Lexer &lex) {
  lex.consume();

  ASTState state(false, false);
  AST def = parse_ast(0, lex, state);
  if (check_constructors(def)) lex.fail = true;
  if (!def) return def;

  if (def.name == "_" || Lexer::isLower(def.name.c_str())) {
    std::ostringstream message;
    message << "Type name must be upper-case or operator, not "
      << def.name << " at "
      << def.token.file();
    reporter->reportError(def.token, message.str());
    lex.fail = true;
  }

  std::set<std::string> args;
  for (auto &x : def.args) {
    if (!Lexer::isLower(x.name.c_str())) {
      std::ostringstream message;
      message << "Type argument must be lower-case, not "
        << x.name << " at "
        << x.token.file();
      reporter->reportError(x.token, message.str());
      lex.fail = true;
    }
    if (!args.insert(x.name).second) {
      std::ostringstream message;
      message << "Type argument "
        << x.name << " occurs more than once at "
        << x.token.file();
      reporter->reportError(x.token, message.str());
      lex.fail = true;
    }
  }

  if (expect(EQUALS, lex)) lex.consume();

  return def;
}

static void parse_decl(Lexer &lex, DefMap &map, Symbols *exports, Symbols *globals) {
  switch (lex.next.type) {
    case FROM: {
      parse_from_import(map, lex);
      break;
    }
    case DEF: {
      for (auto &def : parse_def(lex, map.defs.size(), false, false))
         bind_def(lex, map, std::move(def), exports, globals);
      break;
    }
    case TARGET: {
      auto defs = parse_def(lex, 0, true, false);
      auto &def = defs.front();
      Location l = LOCATION;
      std::stringstream s;
      s << def.body->location.text();
      bind_def(lex, map, Definition("table " + def.name, l,
          new App(l, new Lambda(l, "_", new Prim(l, "tnew"), " "),
          new Literal(l, String::literal(lex.heap, s.str()), &String::typeVar))),
        nullptr, nullptr);
      bind_def(lex, map, std::move(def), exports, globals);
      break;
    }
    default: {
      // should be unreachable
      break;
    }
  }
}

static Expr *parse_block_body(Lexer &lex);
static Expr *parse_require(Lexer &lex) {
  Location l = lex.next.location;
  lex.consume();

  ASTState state(false, true);
  AST ast = parse_ast(0, lex, state);
  auto guard = add_literal_guards(nullptr, state);

  expect(EQUALS, lex);
  lex.consume();

  Expr *rhs = parse_block(lex, false);
  bool eol = lex.next.type == EOL;
  if (eol) lex.consume();

  Expr *otherwise = nullptr;
  if (lex.next.type == ELSE) {
    lex.consume();
    otherwise = parse_block(lex, false);
    if (expect(EOL, lex)) lex.consume();
  } else if (!eol) {
    expect(EOL, lex);
  }

  Expr *block = parse_block_body(lex);

  Match *out = new Match(l, true);
  out->args.emplace_back(rhs);
  out->patterns.emplace_back(std::move(ast), block, guard);
  out->location.end = block->location.end;
  out->otherwise = std::unique_ptr<Expr>(otherwise);

  return out;
}

static Expr *parse_block_body(Lexer &lex) {
  DefMap *map = new DefMap(lex.next.location);

  bool repeat = true;
  while (repeat) {
    switch (lex.next.type) {
      case FROM:
      case TARGET:
      case DEF: {
        parse_decl(lex, *map, nullptr, nullptr);
        break;
      }
      default: {
        repeat = false;
        break;
      }
    }
  }

  std::unique_ptr<Expr> body;
  if (lex.next.type == REQUIRE) {
    body = std::unique_ptr<Expr>(parse_require(lex));
  } else {
    body = std::unique_ptr<Expr>(relabel_anon(parse_binary(0, lex, true)));
  }

  if (map->defs.empty() && map->imports.empty()) {
    delete map;
    return body.release();
  } else {
    map->body = std::move(body);

    map->location.end = map->body->location.end;
    map->location.start.bytes -= (map->location.start.column-1);
    map->location.start.column = 1;

    return map;
  }
}

static Expr *parse_block(Lexer &lex, bool multiline) {
  TRACE("BLOCK");

  if (lex.next.type == INDENT) {
    lex.consume();
    if (expect(EOL, lex)) lex.consume();
    Expr *map = parse_block_body(lex);
    if (expect(DEDENT, lex)) lex.consume();
    return map;
  } else {
    return relabel_anon(parse_binary(0, lex, multiline));
  }
}

Expr *parse_expr(Lexer &lex) {
  return parse_binary(0, lex, false);
}

*/

static void parse_package(CSTElement topdef, Package &package) {
  CSTElement child = topdef.firstChildNode();
  std::string id = getIdentifier(child);

  if (id == "builtin") {
    ERROR(child.location(), "package name 'builtin' is illegal.");
  } else if (package.name.empty()) {
    package.name = id;
  } else {
    ERROR(topdef.location(), "package name redefined from '" << package.name << "' to '" << id << "'");
  }
}

struct ImportArity {
  bool unary;
  bool binary;
};

static ImportArity parse_arity(CSTElement &child) {
  ImportArity out;
  out.unary  = false;
  out.binary = false;

  if (child.id() == CST_ARITY) {
    switch (child.firstChildElement().id()) {
    case TOKEN_KW_UNARY:  out.unary  = true; break;
    case TOKEN_KW_BINARY: out.binary = true; break;
    }
    child.nextSiblingNode();
  }

  return out;
}

static void prefix_op(ImportArity ia, std::string &name) {
  if (ia.unary) {
    name = "unary " + name;
  } else if (ia.binary) {
    name = "binary " + name;
  } else {
    name = "op " + name;
  }
}

static void parse_import(CSTElement topdef, Package &package) {
  CSTElement child = topdef.firstChildNode();

  DefMap &map = *package.files.back().content;
  std::string pkgname = getIdentifier(child);
  child.nextSiblingNode();

  const char *kind = "symbol";
  Symbols::SymbolMap *target = &map.imports.mixed;

  if (child.id() == CST_KIND) {
    switch (child.firstChildElement().id()) {
    case TOKEN_KW_DEF:   kind = "definition"; target = &map.imports.defs;   break;
    case TOKEN_KW_TYPE:  kind = "type";       target = &map.imports.types;  break;
    case TOKEN_KW_TOPIC: kind = "topic";      target = &map.imports.topics; break;
    }
    child.nextSiblingNode();
  }

  ImportArity ia = parse_arity(child);

  // Special case for wildcard import
  if (child.empty()) {
    map.imports.import_all.emplace_back(pkgname);
    return;
  }

  for (; !child.empty(); child.nextSiblingNode()) {
    CSTElement ideq = child.firstChildNode();

    uint8_t idop1 = ideq.id(), idop2;
    std::string name = getIdentifier(ideq);
    ideq.nextSiblingNode();

    std::string source;
    if (ideq.empty()) {
      idop2 = idop1;
      source = name + "@" + pkgname;
    } else if ((idop2 = ideq.id()) == idop1 || ia.binary || ia.unary) {
      source = getIdentifier(ideq) + "@" + pkgname;
    } else {
      idop1 = idop2;
      name = getIdentifier(ideq);
      source = name + "@" + pkgname;

      ERROR(child.location(), "keyword 'binary' or 'unary' required when changing symbol type for " << child.content());
    }

    if (idop1 == CST_OP) prefix_op(ia, name);
    if (idop2 == CST_OP) prefix_op(ia, source);

    auto it = target->insert(std::make_pair(std::move(name), SymbolSource(child.location(), source)));
    if (!it.second) ERROR(child.location(), kind << " '" << it.first->first << "' was previously imported at " << it.first->second.location.file());
  }
}

static void parse_export(CSTElement topdef, Package &package) {
  CSTElement child = topdef.firstChildNode();

  std::string pkgname = getIdentifier(child);
  child.nextSiblingNode();

  const char *kind = nullptr;
  Symbols::SymbolMap *exports = nullptr;
  Symbols::SymbolMap *local   = nullptr;

  if (child.id() == CST_KIND) {
    auto &e = package.exports;
    auto &l = package.files.back().local;

    switch (child.firstChildElement().id()) {
    case TOKEN_KW_DEF:   kind = "definition"; exports = &e.defs;   local = &l.defs;   break;
    case TOKEN_KW_TYPE:  kind = "type";       exports = &e.types;  local = &l.types;  break;
    case TOKEN_KW_TOPIC: kind = "topic";      exports = &e.topics; local = &l.topics; break;
    }
    child.nextSiblingNode();
  }

  if (!kind) {
    ERROR(child.location(), "from ... export must be followed by 'def', 'type', or 'topic'");
    return;
  }

  ImportArity ia = parse_arity(child);
  for (; !child.empty(); child.nextSiblingNode()) {
    CSTElement ideq = child.firstChildNode();

    uint8_t idop1 = ideq.id(), idop2;
    std::string name = getIdentifier(ideq);
    ideq.nextSiblingNode();

    std::string source;
    if (ideq.empty()) {
      idop2 = idop1;
      source = name + "@" + pkgname;
    } else {
      idop2 = ideq.id();
      source = getIdentifier(ideq) + "@" + pkgname;
    }

    if ((idop1 == CST_OP || idop2 == CST_OP) && !(ia.unary || ia.binary)) {
      ERROR(child.location(), "export of " << child.content() << " must specify 'unary' or 'binary'");
      continue;
    }

    if (idop1 == CST_OP) prefix_op(ia, name);
    if (idop2 == CST_OP) prefix_op(ia, source);

    exports->insert(std::make_pair(name, SymbolSource(child.location(), source)));
    // duplciates will be detected as file-local

    auto it = local->insert(std::make_pair(name, SymbolSource(child.location(), source)));
    if (!it.second) ERROR(child.location(), kind << " '" << name << "' was previously defined at " << it.first->second.location.file());
  }
}

struct TopFlags {
  bool exportf;
  bool globalf;
};

static TopFlags parse_flags(CSTElement &child) {
  TopFlags out;

  if (child.id() == CST_FLAG_GLOBAL) {
    out.globalf = true;
    child.nextSiblingNode();
  } else {
    out.globalf = false;
  }

  if (child.id() == CST_FLAG_EXPORT) {
    out.exportf = true;
    child.nextSiblingNode();
  } else {
    out.exportf = false;
  }

  return out;
}

static AST parse_type(CSTElement root) {
  switch (root.id()) {
    case CST_BINARY: {
      CSTElement child = root.firstChildNode();
      AST lhs = parse_type(child);
      child.nextSiblingNode();
      std::string op = "binary " + getIdentifier(child);
      Location location = child.location();
      child.nextSiblingNode();
      AST rhs = parse_type(child);
      if (op == "binary :") {
        if (!lhs.args.empty() || lex_kind(lhs.name) == OPERATOR) {
          ERROR(lhs.region, "tag-name for a type must be a simple lower-case identifier, not " << root.firstChildNode().content());
          return rhs;
        } else {
          rhs.tag = std::move(lhs.name);
          rhs.region = root.location();
          return rhs;
        }
      } else {
        std::vector<AST> args;
        args.emplace_back(std::move(lhs));
        args.emplace_back(std::move(rhs));
        AST out(location, std::move(op), std::move(args));
        out.region = root.location();
        return out;
      }
    }
    case CST_UNARY: {
      CSTElement child = root.firstChildNode();
      std::vector<AST> args;
      if (child.id() != CST_OP) {
        args.emplace_back(parse_type(child));
        child.nextSiblingNode();
      }
      std::string op = "unary " + getIdentifier(child);
      Location location = child.location();
      child.nextSiblingNode();
      if (args.empty()) {
        args.emplace_back(parse_type(child));
        child.nextSiblingNode();
      }
      AST out(location, std::move(op), std::move(args));
      out.region = root.location();
      return out;
    }
    case CST_ID: {
      return AST(root.location(), getIdentifier(root));
    }
    case CST_PAREN: {
      AST out = parse_type(root.firstChildNode());
      out.region = root.location();
      return out;
    }
    case CST_APP: {
      CSTElement child = root.firstChildNode();
      AST lhs = parse_type(child);
      child.nextSiblingNode();
      AST rhs = parse_type(child);
      switch (lex_kind(lhs.name)) {
      case LOWER:    ERROR(lhs.token,  "lower-case identifier '" << lhs.name << "' cannot be used as a type constructor"); break;
      case OPERATOR: ERROR(rhs.region, "excess type argument " << child.content() << " supplied to '" << lhs.name << "'"); break;
      default: break;
      }
      lhs.args.emplace_back(std::move(rhs)); 
      lhs.region = root.location();
      return lhs;
    }
    case CST_ERROR: {
      return AST(root.location(), "BadType");
    }
    default: {
      ERROR(root.location(), "type signatures forbid " << root.content());
      return AST(root.location(), "BadType");
    }
  }
}

static void parse_topic(CSTElement topdef, Package &package, Symbols *globals) {
  CSTElement child = topdef.firstChildNode();
  TopFlags flags = parse_flags(child);

  std::string id = getIdentifier(child);
  Location location = child.location();
  if (lex_kind(id) != LOWER) {
    ERROR(child.location(), "topic identifier '" << id << "' is not lower-case");
    return;
  }
  child.nextSiblingNode();

  File &file = package.files.back();
  AST def = parse_type(child);

  // Confirm there are no open type variables
  TypeMap ids;
  TypeVar x;
  x.setDOB();
  def.unify(x, ids);

  auto it = file.topics.insert(std::make_pair(id, Topic(location, std::move(def))));
  if (!it.second) {
    ERROR(location, "topic '" << id << "' was previously defined at " << it.first->second.location.file());
    return;
  }

  if (flags.exportf) package.exports.topics.insert(std::make_pair(id, SymbolSource(location, SYM_LEAF)));
  if (flags.globalf) globals->topics.insert(std::make_pair(id, SymbolSource(location, SYM_LEAF)));
}

struct Definition {
  std::string name;
  Location location;
  std::unique_ptr<Expr> body;
  std::vector<ScopedTypeVar> typeVars;
  Definition(const std::string &name_, const Location &location_, Expr *body_, std::vector<ScopedTypeVar> &&typeVars_)
   : name(name_), location(location_), body(body_), typeVars(std::move(typeVars_)) { }
  Definition(const std::string &name_, const Location &location_, Expr *body_)
   : name(name_), location(location_), body(body_) { }
};

static void bind_global(const Definition &def, Symbols *globals) {
  if (!globals || def.name == "_") return;

  globals->defs.insert(std::make_pair(def.name, SymbolSource(def.location, SYM_LEAF)));
  // Duplicate globals will be detected as file-local conflicts
}

static void bind_export(const Definition &def, Symbols *exports) {
  if (!exports || def.name == "_") return;

  exports->defs.insert(std::make_pair(def.name, SymbolSource(def.location, SYM_LEAF)));
  // Duplicate exports will be detected as file-local conflicts
}

static void bind_def(DefMap &map, Definition &&def, Symbols *exports, Symbols *globals) {
  bind_global(def, globals);
  bind_export(def, exports);

  if (def.name == "_")
    def.name = "_" + std::to_string(map.defs.size()) + " _";

  Location l = def.body->location;
  auto out = map.defs.insert(std::make_pair(std::move(def.name), DefValue(
    def.location, std::move(def.body), std::move(def.typeVars))));

  if (!out.second) ERROR(l, "definition '" << out.first->first << "' was previously defined at " << out.first->second.body->location.file());
}

static void bind_type(Package &package, const std::string &name, const Location &location, Symbols *exports, Symbols *globals) {
  if (globals) globals->types.insert(std::make_pair(name, SymbolSource(location, SYM_LEAF)));
  if (exports) exports->types.insert(std::make_pair(name, SymbolSource(location, SYM_LEAF)));

  auto it = package.package.types.insert(std::make_pair(name, SymbolSource(location, SYM_LEAF)));
  if (!it.second) ERROR(location, "type '" << it.first->first << "' was previously defined at " << it.first->second.location.file());
}

static void parse_data(CSTElement topdef, Package &package, Symbols *globals) {
  CSTElement child = topdef.firstChildNode();
  TopFlags flags = parse_flags(child);

  auto sump = std::make_shared<Sum>(parse_type(child));
  if (sump->args.empty() && lex_kind(sump->name) == LOWER) ERROR(child.location(), "data type '" << sump->name << "' must be upper-case or operator");
  child.nextSiblingNode();

  for (; !child.empty(); child.nextSiblingNode()) {
    AST cons = parse_type(child);
    if (!cons.tag.empty()) ERROR(cons.region, "constructor '" << cons.name << "' should not be tagged with " << cons.tag);
    if (cons.args.empty() && lex_kind(cons.name) == LOWER) ERROR(cons.token, "constructor '" << cons.name << "' must be upper-case or operator");
    sump->addConstructor(std::move(cons));
  }

  Symbols *exports = flags.exportf ? &package.exports : nullptr;
  if (!flags.globalf) globals = nullptr;

  bind_type(package, sump->name, sump->token, exports, globals);
  for (auto &c : sump->members) {
    Expr *construct = new Construct(c.ast.token, sump, &c);
    for (size_t i = 0; i < c.ast.args.size(); ++i)
      construct = new Lambda(c.ast.token, "_", construct);

    bind_def(*package.files.back().content, Definition(c.ast.name, c.ast.token, construct), exports, globals);
  }

  if (package.name == "wake") check_special(sump);
}

static void parse_tuple(CSTElement topdef, Package &package, Symbols *globals) {
  CSTElement child = topdef.firstChildNode();
  TopFlags flags = parse_flags(child);

  auto sump = std::make_shared<Sum>(parse_type(child));
  if (lex_kind(sump->name) != UPPER) ERROR(child.location(), "tuple type '" << sump->name << "' must be upper-case");
  child.nextSiblingNode();

  std::string name = sump->name;

  AST tuple(sump->token, std::string(sump->name));
  tuple.region = sump->region;
  std::vector<TopFlags> members;

  for (; !child.empty(); child.nextSiblingNode()) {
    CSTElement elt = child.firstChildNode();
    members.emplace_back(parse_flags(elt));
    tuple.args.emplace_back(parse_type(elt));
  }

  sump->addConstructor(std::move(tuple));

  Constructor &c = sump->members.back();
  Expr *construct = new Construct(c.ast.token, sump, &c);
  for (size_t i = c.ast.args.size(); i > 0; --i)
    construct = new Lambda(c.ast.token, c.ast.args[i-1].tag, construct);

  DefMap &map = *package.files.back().content;

  Symbols *exports = &package.exports;
  bind_type(package, sump->name, sump->token, flags.exportf?exports:nullptr, flags.globalf?globals:nullptr);
  bind_def(map, Definition(c.ast.name, c.ast.token, construct), flags.exportf?exports:nullptr, flags.globalf?globals:nullptr);

  if (package.name == "wake") check_special(sump);

  // Create get/set/edit helper methods
  size_t outer = 0;
  for (size_t i = 0; i < members.size(); ++i) {
    std::string &mname = c.ast.args[i].tag;
    Location memberToken = c.ast.args[i].region;
    bool globalb = members[i].globalf;
    bool exportb = members[i].exportf;
    if (lex_kind(mname) != UPPER) continue;

    // Implement get methods
    std::string get = "get" + name + mname;
    Expr *getfn = new Lambda(memberToken, "_", new Get(memberToken, sump, &c, i));
    getfn->flags |= FLAG_SYNTHETIC;
    bind_def(map, Definition(get, memberToken, getfn), exportb?exports:nullptr, globalb?globals:nullptr);

    // Implement edit methods
    DefMap *editmap = new DefMap(memberToken);
    editmap->body = std::unique_ptr<Expr>(new Construct(memberToken, sump, &c));
    for (size_t inner = 0; inner < members.size(); ++inner) {
      Expr *select = new Get(memberToken, sump, &c, inner);
      if (inner == outer)
        select =
          new App(memberToken,
            new VarRef(memberToken, "fn" + mname),
            new App(memberToken,
              new Lambda(memberToken, "_", select),
              new VarRef(memberToken, "_ x")));
      std::string x = std::to_string(members.size()-inner);
      std::string name = "_ a" + std::string(4 - x.size(), '0') + x;
      editmap->defs.insert(std::make_pair(name,
        DefValue(memberToken, std::unique_ptr<Expr>(select))));
    }

    std::string edit = "edit" + name + mname;
    Expr *editfn =
      new Lambda(memberToken, "fn" + mname,
        new Lambda(memberToken, "_ x", editmap));

    editfn->flags |= FLAG_SYNTHETIC;
    bind_def(map, Definition(edit, memberToken, editfn), exportb?exports:nullptr, globalb?globals:nullptr);

    // Implement set methods
    DefMap *setmap = new DefMap(memberToken);
    setmap->body = std::unique_ptr<Expr>(new Construct(memberToken, sump, &c));
    for (size_t inner = 0; inner < members.size(); ++inner) {
      std::string x = std::to_string(members.size()-inner);
      std::string name = "_ a" + std::string(4 - x.size(), '0') + x;
      setmap->defs.insert(std::make_pair(name,
        DefValue(memberToken, std::unique_ptr<Expr>(
          (inner == outer)
          ? static_cast<Expr*>(new VarRef(memberToken, mname))
          : static_cast<Expr*>(new Get(memberToken, sump, &c, inner))))));
    }

    std::string set = "set" + name + mname;
    Expr *setfn =
      new Lambda(memberToken, mname,
        new Lambda(memberToken, "_ x", setmap));

    setfn->flags |= FLAG_SYNTHETIC;
    bind_def(map, Definition(set, memberToken, setfn), exportb?exports:nullptr, globalb?globals:nullptr);

    ++outer;
  }
}

/*
static AST parse_pattern(CSTElement root, std::vector<Expr*> &guard) {
  switch (root.id()) {
  case CST_BINARY:
  case CST_UNARY:
  case CST_ID:
  case CST_PAREN:
  case CST_APP:
  case CST_HOLE:
  case CST_LITERAL:
  // INTERPOLATE?
  }
}
*/

static std::vector<Definition> parse_def(CSTElement def, long index, bool target, bool publish) {
  lex.consume();

  ASTState state(false, false);
  AST ast = parse_ast(0, lex, state);
  if (ast.name.empty()) ast.name = "undef";
  std::string name = std::move(ast.name);
  ast.name.clear();
  if (check_constructors(ast)) lex.fail = true;

  bool extract = Lexer::isUpper(name.c_str()) || (state.topParen && Lexer::isOperator(name.c_str()));
  if (extract && (target || publish)) {
    std::ostringstream message;
    message << "Upper-case identifier cannot be used as a target/publish name at "
      << ast.token.text();
    reporter->reportError(ast.token, message.str());
    lex.fail = true;
    extract = false;
  }

  size_t tohash = ast.args.size();
  if (target && lex.next.type == LAMBDA) {
    lex.consume();
    AST sub = parse_ast(APP_PRECEDENCE, lex, state, AST(lex.next.location));
    if (check_constructors(ast)) lex.fail = true;
    for (auto &x : sub.args) ast.args.push_back(std::move(x));
    ast.region.end = sub.region.end;
  }

  Location fn = ast.region;

  expect(EQUALS, lex);
  lex.consume();

  Expr *body = parse_block(lex, false);
  if (expect(EOL, lex)) lex.consume();

  // Record type variables introduced by the def before we rip the ascription appart
  std::vector<ScopedTypeVar> typeVars;
  ast.typeVars(typeVars);

  std::vector<Definition> out;
  if (extract) {
    ast.name = std::move(name);
    extract_def(out, index, std::move(ast), typeVars, body);
    return out;
  }

  // do we need a pattern match? lower / wildcard are ok
  bool pattern = false;
  bool typed = false;
  for (auto &x : ast.args) {
    pattern |= Lexer::isOperator(x.name.c_str()) || Lexer::isUpper(x.name.c_str());
    typed |= x.type;
  }

  optional<AST> type = std::move(ast.type);
  std::vector<std::pair<std::string, Location> > args;
  if (pattern) {
    // bind the arguments to anonymous lambdas and push the whole thing into a pattern
    size_t nargs = ast.args.size();
    Match *match = new Match(fn);
    if (nargs > 1) {
      match->patterns.emplace_back(std::move(ast), body, nullptr);
    } else {
      match->patterns.emplace_back(std::move(ast.args.front()), body, nullptr);
    }
    for (size_t i = 0; i < nargs; ++i) {
      args.emplace_back("_ " + std::to_string(i), LOCATION);
      match->args.emplace_back(new VarRef(fn, "_ " + std::to_string(i)));
    }
    body = match;
  } else if (typed) {
    DefMap *dm = new DefMap(fn);
    dm->body = std::unique_ptr<Expr>(body);
    for (size_t i = 0; i < ast.args.size(); ++i) {
      AST &arg = ast.args[i];
      args.emplace_back(arg.name, arg.token);
      if (arg.type) {
        dm->defs.insert(std::make_pair("_type " + arg.name, DefValue(arg.region, std::unique_ptr<Expr>(
          new Ascribe(LOCATION, std::move(*arg.type), new VarRef(LOCATION, arg.name), arg.token)))));
      }
    }
    body = dm;
  } else {
    // no pattern; simple lambdas for the arguments
    for (auto &x : ast.args) args.emplace_back(x.name, x.token);
  }

  if (type)
    body = new Ascribe(LOCATION, std::move(*type), body, body->location);

  if (target) {
    if (tohash == 0) {
      std::ostringstream message;
      message << "Target definition must have at least one hashed argument "
        << fn.text();
      reporter->reportError(fn, message.str());
      lex.fail = true;
    }
    Location bl = body->location;
    Expr *hash = new Prim(bl, "hash");
    for (size_t i = 0; i < tohash; ++i) hash = new Lambda(bl, "_", hash, " ");
    for (size_t i = 0; i < tohash; ++i) hash = new App(bl, hash, new VarRef(bl, args[i].first));
    Expr *subhash = new Prim(bl, "hash");
    for (size_t i = tohash; i < args.size(); ++i) subhash = new Lambda(bl, "_", subhash, " ");
    for (size_t i = tohash; i < args.size(); ++i) subhash = new App(bl, subhash, new VarRef(bl, args[i].first));
    Lambda *gen = new Lambda(bl, "_", body, " ");
    Lambda *tget = new Lambda(bl, "_fn", new Prim(bl, "tget"), " ");
    body = new App(bl, new App(bl, new App(bl, new App(bl,
      new Lambda(bl, "_target", new Lambda(bl, "_hash", new Lambda(bl, "_subhash", tget))),
      new VarRef(bl, "table " + name)), hash), subhash), gen);
  }

  if (publish && !args.empty()) {
    std::ostringstream message;
    message << "Publish definition may not be a function " << fn.text();
    reporter->reportError(fn, message.str());
    lex.fail = true;
  } else {
    for (auto i = args.rbegin(); i != args.rend(); ++i) {
      Lambda *lambda = new Lambda(fn, i->first, body);
      lambda->token = i->second;
      body = lambda;
    }
  }

  out.emplace_back(name, ast.token, body, std::move(typeVars));
  return out;
}

const char *dst_top(CSTElement root, Top &top) {
  std::unique_ptr<Package> package(new Package);
  package->files.resize(1);
  File &file = package->files.back();
  file.content = std::unique_ptr<DefMap>(new DefMap(root.location()));
  DefMap &map = *file.content;
  Symbols globals;

  for (CSTElement topdef = root.firstChildNode(); !topdef.empty(); topdef.nextSiblingNode()) {
    switch (topdef.id()) {
    case CST_PACKAGE: parse_package(topdef, *package); break;
    case CST_IMPORT:  parse_import (topdef, *package); break;
    case CST_EXPORT:  parse_export (topdef, *package); break;
    // case CST_PUBLISH: parse_publish(topdef, *package); break; // !!! used parse_def
    case CST_TOPIC:   parse_topic  (topdef, *package, &globals); break;
    case CST_DATA:    parse_data   (topdef, *package, &globals); break;
    case CST_TUPLE:   parse_tuple  (topdef, *package, &globals); break;
    // case CST_DEF:     parse_def    (topdef, *package, &globals); break;
    // case CST_TARGET:  parse_target (topdef, *package, &globals); break;
    }
  }

  // Set a default import
  if (file.content->imports.empty())
    file.content->imports.import_all.push_back("wake");

  // Set a default package name
  if (package->name.empty()) {
    package->name = file.content->location.filename;
  }

  package->exports.setpkg(package->name);
  globals.setpkg(package->name);

  top.globals.join(globals, "global");

  // localize all top-level symbols
  DefMap::Defs defs(std::move(map.defs));
  map.defs.clear();
  for (auto &def : defs) {
    auto name = def.first + "@" + package->name;
    auto it = file.local.defs.insert(std::make_pair(def.first, SymbolSource(def.second.location, name, SYM_LEAF)));
    if (!it.second) {
      if (it.first->second.qualified == name) {
        it.first->second.location = def.second.location;
        it.first->second.flags |= SYM_LEAF;
        package->exports.defs.find(def.first)->second.flags |= SYM_LEAF;
      } else {
        ERROR(def.second.location, "definition '" << def.first << "' was previously defined at " << it.first->second.location.file());
      }
    }
    map.defs.insert(std::make_pair(std::move(name), std::move(def.second)));
  }

  // localize all topics
  for (auto &topic : file.topics) {
    auto name = topic.first + "@" + package->name;
    auto it = file.local.topics.insert(std::make_pair(topic.first, SymbolSource(topic.second.location, name, SYM_LEAF)));
    if (!it.second) {
      if (it.first->second.qualified == name) {
        it.first->second.location = topic.second.location;
        it.first->second.flags |= SYM_LEAF;
        package->exports.topics.find(topic.first)->second.flags |= SYM_LEAF;
      } else {
        ERROR(topic.second.location, "topic '" << topic.first << "' was previously defined at " << it.first->second.location.file());
      }
    }
  }

  // localize all types
  for (auto &type : package->package.types) {
    auto name = type.first + "@" + package->name;
    auto it = file.local.types.insert(std::make_pair(type.first, SymbolSource(type.second.location, name, SYM_LEAF)));
    if (!it.second) {
      if (it.first->second.qualified == name) {
        it.first->second.location = type.second.location;
        it.first->second.flags |= SYM_LEAF;
        package->exports.types.find(type.first)->second.flags |= SYM_LEAF;
      } else {
        ERROR(type.second.location, "type '" << type.first << "' was previously defined at " << it.first->second.location.file());
      }
    }
  }

  auto it = top.packages.insert(std::make_pair(package->name, nullptr));
  if (it.second) {
    package->package = file.local;
    it.first->second = std::move(package);
  } else {
    it.first->second->package.join(file.local, "package-local");
    it.first->second->exports.join(package->exports, nullptr);
    // duplicated export already reported as package-local duplicate
    it.first->second->files.emplace_back(std::move(file));
  }

  return it.first->second->name.c_str();
}
