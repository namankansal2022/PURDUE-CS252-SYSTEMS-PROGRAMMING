#ifndef simplecommand_hh
#define simplecommand_hh
#include <string>
#include <vector>

struct SimpleCommand {
  std::vector<std::string *> _arguments;
  SimpleCommand();
  ~SimpleCommand();
  void insertArgument( std::string * argument );
  void print();
};

#endif