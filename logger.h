#pragma once

#include <string>
#include <format>
#include <syslog.h>

using std::format;
using std::format_string;

class SysLogger {
public:
    SysLogger(int options = LOG_PID, int facility = LOG_USER);
    ~SysLogger();

    template<typename... Args>
    void critical(format_string<Args...> fmt, Args&&... args) {
        log(LOG_CRIT, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void error(format_string<Args...> fmt, Args&&... args) {
        log(LOG_ERR, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warning(format_string<Args...> fmt, Args&&... args) {
        log(LOG_WARNING, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void info(format_string<Args...> fmt, Args&&... args) {
        log(LOG_INFO, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void debug(format_string<Args...> fmt, Args&&... args) {
        log(LOG_DEBUG, format(fmt, std::forward<Args>(args)...));
    }

private:
    void log(int priority, const std::string& message);
};
