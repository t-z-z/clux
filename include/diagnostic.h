#pragma once
#include "token.h"

#include <string>
#include <vector>
#include <optional>

namespace cx{

enum class DiagLevel{Error, Warning, Note, Help};

struct Diagnostic{
    DiagLevel lvl;
    std::string code;
    std::string msg;
    Location loc;
    Diagnostic(DiagLevel level,const std::string &message,const Location &location):
        lvl(level), msg(message), loc(location){}
    Diagnostic() = default;
    std::vector<Location> secondary_locations;
    std::vector<Diagnostic> children;
    std::optional<std::string> suggestion;
};

void emit_diagnostic(const Diagnostic &diag);

}// namespace cx