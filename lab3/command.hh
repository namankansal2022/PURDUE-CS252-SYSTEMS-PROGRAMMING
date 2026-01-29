#ifndef command_hh
#define command_hh
#include "simpleCommand.hh"
#include <set>

struct Command {
    std::vector<SimpleCommand *> _simpleCommands;
    std::string * _outFile;
    std::string * _inFile;
    std::string * _errFile;
    bool _background;
    bool _isSubshell;
    bool _appendOut;
    bool _appendErr;
    
    static std::set<pid_t> _backgroundPids;
    static int _lastReturnCode;
    static pid_t _lastBackgroundPid;
    static std::string _lastArgument;
    static std::string _shellPath;
    
    Command();
    void insertSimpleCommand(SimpleCommand *simpleCommand);
    void clear();
    void print();
    void execute();
    
    static SimpleCommand *_currentSimpleCommand;
};

#endif