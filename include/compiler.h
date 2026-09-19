#pragma once

#include <ostream>
#include <string>

namespace cx {

// Front-end check only: lex -> parse -> semantic, including import handling.
// Returns false if any diagnostic is emitted.
bool check(const std::string& source, const std::string& filename = "<input>",
           const std::string& lib_dir = "");

// Translate to LLVM IR text; returns false if the front end fails.
bool translate(const std::string& source, const std::string& filename,
               const std::string& lib_dir, std::ostream& out);

} // namespace cx
