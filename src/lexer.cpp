#include "../include/lexer.h"

std::string IdentifierStr;
double NumVal;

int CurLine = 1;
int CurCol = 0;

int gettok() {
  static int LastChar = ' ';

  while (isspace(LastChar)) {
    if (LastChar == '\n') {
      CurLine++;
      CurCol = 0;
    }
    LastChar = getchar();
    CurCol++;
  }

  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;
    while (isalnum((LastChar = getchar()))) {
      IdentifierStr += LastChar;
      CurCol++;
    }

    if (IdentifierStr == "fun")
      return tok_def;
    if (IdentifierStr == "extern")
      return tok_extern;
    if (IdentifierStr == "struct")
      return tok_struct;
    if (IdentifierStr == "if")
      return tok_if;
    if (IdentifierStr == "then")
      return tok_then;
    if (IdentifierStr == "else")
      return tok_else;
    if (IdentifierStr == "for")
      return tok_for;
    if (IdentifierStr == "in")
      return tok_in;
    if (IdentifierStr == "int")
      return tok_int;
    if (IdentifierStr == "double")
      return tok_double;
    if (IdentifierStr == "char")
      return tok_char;
    if (IdentifierStr == "asm")
      return tok_asm;
    if (IdentifierStr == "void")
      return tok_void;
    return tok_identifier;
  }

  if (LastChar == '.') {
    LastChar = getchar();
    return '.';
  }

  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    bool hasDot = false;
    do {
      if (LastChar == '.') {
        if (hasDot)
          break;
        hasDot = true;
      }
      NumStr += LastChar;
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  if (LastChar == '"') {
    IdentifierStr = "";
    while (true) {
      LastChar = getchar();
      CurCol++;

      if (LastChar == EOF)
        break;

      if (LastChar == '"') {
        break;
      }

      if (LastChar == '\\') {
        IdentifierStr += LastChar;

        LastChar = getchar();
        CurCol++;
        if (LastChar == EOF)
          break;

        IdentifierStr += LastChar;
      } else {
        IdentifierStr += LastChar;
      }
    }

    if (LastChar == '"') {
      LastChar = getchar();
      CurCol++;
    }
    return tok_string;
  }

  if (LastChar == '#') {
    do
      LastChar = getchar();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok();
  }

  if (LastChar == EOF)
    return tok_eof;

  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}
