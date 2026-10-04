#pragma once

// Packaging assets without opening the editor window. See README for the options.

#include <span>
#include <string>

// True if the arguments ask for something to be done from the command line, rather than for the editor to be opened.
bool isCommandLineRequest(std::span<const std::string> args);

// args does not include the name of the program. Returns the exit code of the process.
int runCommandLine(std::span<const std::string> args);
