%code requires {
#include <string>
#if __cplusplus > 199711L
#define register
#endif
}
%union {
  char        *string_val;
  std::string *cpp_string;
}
%token <cpp_string> WORD
%token GREAT GREATGREAT GREATAMPERSAND GREATGREATAMPERSAND
%token LESS PIPE AMPERSAND NEWLINE
%token LPAREN RPAREN
%{
#include <cstdio>
#include <iostream>
#include <stdlib.h>
#include "shell.hh"
void yyerror(const char * s);
int yylex();
%}
%%
goal:
  command_list
  ;
command_list:
    command_line
  | command_list command_line
  ;
command_line:
    pipe_list io_modifier_list background_opt NEWLINE {
      Shell::_currentCommand.execute();
    }
  | subshell NEWLINE {
      Shell::_currentCommand.execute();
    }
  | NEWLINE
  | error NEWLINE { yyerrok; }
  ;
pipe_list:
    pipe_list PIPE command_and_args
  | command_and_args
  | subshell
  ;
subshell:
    '(' command_list ')' {
        Shell::_currentCommand._isSubshell = true;
    }
  ;
command_and_args:
    command_word arg_list {
        Shell::_currentCommand.insertSimpleCommand(Command::_currentSimpleCommand);
    }
  ;
command_word:
    WORD {
        Command::_currentSimpleCommand = new SimpleCommand();
        Command::_currentSimpleCommand->insertArgument($1);
    }
  ;
arg_list:
    arg_list WORD {
        Command::_currentSimpleCommand->insertArgument($2);
    }
  | /* empty */
  ;
io_modifier_list:
    io_modifier_list io_modifier
  | /* empty */
  ;
io_modifier:
    GREAT WORD {
      if (Shell::_currentCommand._outFile) {
          fprintf(stderr, "Ambiguous output redirect.\n");
      } else {
          Shell::_currentCommand._outFile = new std::string(*$2);
          Shell::_currentCommand._appendOut = false;
      }
      delete $2;
    }
  | GREATGREAT WORD {
      if (Shell::_currentCommand._outFile) {
          fprintf(stderr, "Ambiguous output redirect.\n");
      } else {
          Shell::_currentCommand._outFile = new std::string(*$2);
          Shell::_currentCommand._appendOut = true;
      }
      delete $2;
    }
  | GREATAMPERSAND WORD {
      if (Shell::_currentCommand._outFile || Shell::_currentCommand._errFile) {
          fprintf(stderr, "Ambiguous output redirect.\n");
      } else {
          Shell::_currentCommand._outFile = new std::string(*$2);
          Shell::_currentCommand._errFile = new std::string(*$2);
          Shell::_currentCommand._appendOut = false;
          Shell::_currentCommand._appendErr = false;
      }
      delete $2;
    }
  | GREATGREATAMPERSAND WORD {
      if (Shell::_currentCommand._outFile || Shell::_currentCommand._errFile) {
          fprintf(stderr, "Ambiguous output redirect.\n");
      } else {
          Shell::_currentCommand._outFile = new std::string(*$2);
          Shell::_currentCommand._errFile = new std::string(*$2);
          Shell::_currentCommand._appendOut = true;
          Shell::_currentCommand._appendErr = true;
      }
      delete $2;
    }
  | LESS WORD {
      if (Shell::_currentCommand._inFile) {
          fprintf(stderr, "Ambiguous input redirect.\n");
      } else {
          Shell::_currentCommand._inFile = new std::string(*$2);
      }
      delete $2;
    }
  ;
background_opt:
    AMPERSAND {
      Shell::_currentCommand._background = 1;
    }
  | /* empty */
  ;
%%
void yyerror(const char * s)
{
  fprintf(stderr,"%s", s);
}
#if 0
int main() { yyparse(); }
#endif