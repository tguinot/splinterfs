#include "logger.h"

SysLogger::SysLogger(int options, int facility) {
    openlog("splinterfs", options, facility);
}

SysLogger::~SysLogger() {
    closelog();
}

void SysLogger::log(int priority, const std::string& message) {
    syslog(priority, "%s", message.c_str());
}
