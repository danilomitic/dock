#pragma once

namespace dx {

// Logs to the debugger, and to %LOCALAPPDATA%\DockX\dockx.log when file logging is on.
void EnableFileLog(bool on);
void Log(const wchar_t* fmt, ...);

}  // namespace dx
