#pragma once
// commands.hpp — one register_* function per command group, one .cpp each.
// Adding a group (e.g. cmd_cluster.cpp for ring/node commands later) means a
// new file + one line in shell.cpp, nothing else.

namespace shell {

void register_db_commands();   // put / get / del / dbstat
void register_sys_commands();  // mem / reboot

}  // namespace shell
