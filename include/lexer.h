#ifndef LEXER_H
#define LEXER_H

#include <regex>
#include <string>

enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,

  // conditionals
  tok_if = -6,
  tok_then = -7,
  tok_else = -8,

  // for loop
  tok_for = -9,
  tok_in = -10,

  // numbers
  tok_int = -11,
  tok_double = -12,

  // bro, just read...
  tok_struct = -13,

  // ascii friends
  tok_char = -14,
  tok_string = -15,
};

extern std::string IdentifierStr;
extern double NumVal;
int gettok();

#endif // !LEXER_H
