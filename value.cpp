#include "value.h"
#include "expr.h"
#include "heap.h"
#include <iostream>
#include <sstream>
#include <cassert>

Value::~Value() { }
const char *String::type = "String";
const char *Integer::type = "Integer";
const char *Closure::type = "Closure";
const char *Exception::type = "Exception";

Integer::~Integer() {
  mpz_clear(value);
}

std::string Value::to_str() const {
  std::stringstream str;
  str << this;
  return str.str();
}

std::ostream & operator << (std::ostream &os, const Value *value) {
  if (value->type == String::type) {
    const String *str = reinterpret_cast<const String*>(value);
    return os << "String(" << str->value << ")";
  } else if (value->type == Integer::type) {
    const Integer *integer = reinterpret_cast<const Integer*>(value);
    return os << "Integer(" << integer->str() << ")";
  } else if (value->type == Closure::type) {
    const Closure *closure = reinterpret_cast<const Closure*>(value);
    return os << "Closure(" << closure->body->location << ")";
  } else if (value->type == Exception::type) {
    const Exception *exception = reinterpret_cast<const Exception*>(value);
    os << "Exception(" << std::endl;
    for (auto &i : exception->causes) {
      os << "  " << i->reason << std::endl;
      for (auto &j : i->stack) {
        os << "    from " << j << std::endl;
      }
    }
    return os << ")" << std::endl;
  } else {
    assert(0 /* unreachable */);
    return os;
  }
}

Cause::Cause(const std::string &reason_, std::vector<Location> &&stack_)
 : reason(reason_), stack(std::move(stack_)) { }

Exception::Exception(const std::string &reason, const std::shared_ptr<Binding> &binding) : Value(type) {
  causes.emplace_back(std::make_shared<Cause>(reason, Binding::stack_trace(binding)));
}

std::string Integer::str(int base) const {
  char buffer[mpz_sizeinbase(value, base) + 2];
  mpz_get_str(buffer, base, value);
  return buffer;
}
