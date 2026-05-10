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
    if (IdentifierStr == "while")
      return tok_while;
    return tok_identifier;
  }

  if (LastChar == '.') {
    LastChar = getchar();
    return '.';
  }

  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    bool hasDot = false;

    if (LastChar == '0') {
      NumStr += LastChar;
      LastChar = getchar();
      if (LastChar == 'x' || LastChar == 'X') {
        NumStr += LastChar;
        LastChar = getchar();
        while (isxdigit(LastChar)) {
          NumStr += LastChar;
          LastChar = getchar();
        }
        CurCol += NumStr.length();
        NumVal = (double)strtoll(NumStr.c_str(), nullptr, 16);
        return tok_number;
      }
    }

    while (isdigit(LastChar) || LastChar == '.') {
      if (LastChar == '.') {
        if (hasDot)
          break;
        hasDot = true;
      }
      NumStr += LastChar;
      LastChar = getchar();
    }

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

  if (LastChar == '!') {
    LastChar = getchar();
    if (LastChar == '=') {
      LastChar = getchar();
      CurCol++;
      return tok_neq;
    }
    return '!';
  }

  if (LastChar == '=') {
    LastChar = getchar();
    if (LastChar == '=') {
      LastChar = getchar();
      CurCol++;
      return tok_eqeq;
    }
    return '=';
  }

  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}
