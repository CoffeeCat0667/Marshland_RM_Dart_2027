// Entry.cpp
// Marshland -- RoboMaster 2027 dart system gimbal control program.
//
// This file provides the process entry point only.
//
// Process lifecycle and startup coordination (reading/parsing the JSON config,
// validating required entries, initialising CAN/GPIO/IIC/SBUS/vision modules,
// starting HTTP and WebSocket services, and switching the system into STANDBY or
// INIT_FAILED) is specified by Design/Atomic/ATOM-01_Runtime.md. Per the
// project working rules that functionality is out of scope for this project,
// so main() intentionally stays a minimal skeleton and implements no startup
// logic, no module construction and no state machine.

int main()
{
    return 0;
}
