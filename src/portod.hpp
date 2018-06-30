#pragma once

class TEpollLoop;
class TEventQueue;

extern std::unique_ptr<TEpollLoop> EpollLoop;
extern std::unique_ptr<TEventQueue> EventQueue;

extern std::string PreviousVersion;
extern bool PortodFrozen;
extern bool ShutdownPortod;

void ReopenMasterLog();
void CheckPortoSocket();
