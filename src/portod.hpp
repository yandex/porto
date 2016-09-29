#pragma once

class TEpollLoop;
class TEventQueue;
class TPidFile;

extern TPidFile PortoMasterPid;
extern TPidFile PortoSlavePid;

extern std::unique_ptr<TEpollLoop> EpollLoop;
extern std::unique_ptr<TEventQueue> EventQueue;
