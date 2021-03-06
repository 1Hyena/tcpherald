// SPDX-License-Identifier: MIT
#include <iostream>
#include <stdarg.h>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

#include "options.h"
#include "program.h"
#include "signals.h"
#include "sockets.h"

volatile sig_atomic_t
    SIGNALS::sig_alarm{0},
    SIGNALS::sig_pipe {0},
    SIGNALS::sig_int  {0},
    SIGNALS::sig_term {0},
    SIGNALS::sig_quit {0};

size_t PROGRAM::log_size = 0;
bool   PROGRAM::log_time = false;

void PROGRAM::run() {
    if (!options) return bug();

    if (options->exit_flag) {
        status = EXIT_SUCCESS;
        return;
    }

    bool terminated = false;

    int supply_descriptor{
        sockets->listen(std::to_string(get_supply_port()).c_str())
    };

    int demand_descriptor{
        sockets->listen(std::to_string(get_demand_port()).c_str())
    };

    int driver_descriptor = SOCKETS::NO_DESCRIPTOR;

    if (get_driver_port()) {
        driver_descriptor = sockets->listen(
            std::to_string(get_driver_port()).c_str()
        );
    }

    if (supply_descriptor == SOCKETS::NO_DESCRIPTOR
    ||  demand_descriptor == SOCKETS::NO_DESCRIPTOR) {
        terminated = true;
        status = EXIT_FAILURE;
    }
    else {
        status = EXIT_SUCCESS;
        log_time = true;

        if (driver_descriptor == SOCKETS::NO_DESCRIPTOR) {
            log(
                "Listening on ports %d and %d...",
                int(get_supply_port()), int(get_demand_port())
            );
        }
        else {
            log(
                "Listening on ports %d, %d and %d...",
                int(get_supply_port()), int(get_demand_port()),
                int(get_driver_port())
            );
        }
    }

    std::vector<uint8_t> buffer;
    std::unordered_map<int, long long> timestamp_map;
    std::unordered_map<int, int> supply_map;
    std::unordered_map<int, int> demand_map;
    std::unordered_set<int> unmet_supply;
    std::unordered_set<int> unmet_demand;
    std::unordered_set<int> drivers;

    static constexpr const size_t USEC_PER_SEC = 1000000;
    bool alarmed = false;
    set_timer(USEC_PER_SEC);

    do {
        alarmed = false;

        signals->block();
        while (int sig = signals->next()) {
            char *sig_name = strsignal(sig);

            switch (sig) {
                case SIGALRM: {
                    alarmed = true;
                    break;
                }
                case SIGINT :
                case SIGTERM:
                case SIGQUIT: terminated = true; // fall through
                default     : {
                    // Since signals are blocked, we can call fprintf here.
                    fprintf(stderr, "%s", "\n");

                    log(
                        "Caught signal %d (%s).", sig,
                        sig_name ? sig_name : "unknown"
                    );

                    break;
                }
            }
        }

        if (alarmed) set_timer(USEC_PER_SEC);

        signals->unblock();

        if (terminated) {
            sockets->disconnect(demand_descriptor);
            sockets->disconnect(supply_descriptor);
            sockets->disconnect(driver_descriptor);

            continue;
        }

        if (!alarmed && !sockets->serve()) {
            log("%s", "Error while serving the listening descriptors.");
            status = EXIT_FAILURE;
            terminated = true;
        }

        long long timestamp = get_timestamp();

        int d = SOCKETS::NO_DESCRIPTOR;
        while ((d = sockets->next_disconnection()) != SOCKETS::NO_DESCRIPTOR) {
            log(
                "Disconnected %s:%s (descriptor %d).",
                sockets->get_host(d), sockets->get_port(d), d
            );

            if (timestamp_map.count(d)) {
                timestamp_map.erase(d);
            }

            if (drivers.count(d)) {
                drivers.erase(d);
                continue;
            }

            int other_descriptor = SOCKETS::NO_DESCRIPTOR;

            if (supply_map.count(d)) {
                other_descriptor = supply_map[d];
                supply_map.erase(d);
            }
            else if (demand_map.count(d)) {
                other_descriptor = demand_map[d];
                demand_map.erase(d);
            }
            else {
                unmet_supply.erase(d);
                unmet_demand.erase(d);
            }

            if (other_descriptor != SOCKETS::NO_DESCRIPTOR) {
                if (supply_map.count(other_descriptor)) {
                    supply_map[other_descriptor] = SOCKETS::NO_DESCRIPTOR;
                }
                else if (demand_map.count(other_descriptor)) {
                    demand_map[other_descriptor] = SOCKETS::NO_DESCRIPTOR;
                }

                sockets->disconnect(other_descriptor);
            }
        }

        size_t new_demand = 0;

        while ((d = sockets->next_connection()) != SOCKETS::NO_DESCRIPTOR) {
            log(
                "New connection from %s:%s (descriptor %d).",
                sockets->get_host(d), sockets->get_port(d), d
            );

            timestamp_map[d] = timestamp;

            int listener = sockets->get_listener(d);

            if (listener == supply_descriptor) {
                if (unmet_demand.empty()) {
                    unmet_supply.insert(d);
                    sockets->freeze(d);
                }
                else {
                    int other_descriptor = *(unmet_demand.begin());
                    unmet_demand.erase(other_descriptor);
                    supply_map[d] = other_descriptor;
                    demand_map[other_descriptor] = d;
                    sockets->unfreeze(other_descriptor);
                    timestamp_map[other_descriptor] = timestamp;
                }
            }
            else if (listener == demand_descriptor) {
                if (unmet_supply.empty()) {
                    unmet_demand.insert(d);
                    sockets->freeze(d);
                    ++new_demand;
                }
                else {
                    int other_descriptor = *(unmet_supply.begin());
                    unmet_supply.erase(other_descriptor);
                    demand_map[d] = other_descriptor;
                    supply_map[other_descriptor] = d;
                    sockets->unfreeze(other_descriptor);
                    timestamp_map[other_descriptor] = timestamp;
                }
            }
            else if (listener == driver_descriptor
            && driver_descriptor != SOCKETS::NO_DESCRIPTOR) {
                drivers.insert(d);

                timestamp_map[d] = (
                    // Kludge to skip reporting new demand to this driver.
                    timestamp + 1LL
                );

                sockets->writef(d, "%lu\n", unmet_demand.size());
            }
            else log("Forbidden condition met (%s:%d).", __FILE__, __LINE__);
        }

        if (new_demand || alarmed) {
            for (int driver : drivers) {
                if (timestamp_map[driver] > timestamp) {
                    // This is a brand new driver and thus it must have already
                    // received the current number of unmet demand.

                    timestamp_map[driver] = timestamp;
                    continue;
                }

                if (!new_demand) {
                    uint32_t driver_period = get_driver_period();

                    if (!driver_period
                    ||  timestamp - timestamp_map[driver] < driver_period) {
                        continue;
                    }

                    sockets->writef(driver, "%lu\n", unmet_demand.size());
                }
                else {
                    sockets->writef(driver, "%lu\n", new_demand);
                }

                timestamp_map[driver] = timestamp;
            }
        }

        while ((d = sockets->next_incoming()) != SOCKETS::NO_DESCRIPTOR) {
            sockets->swap_incoming(d, buffer);

            if (!drivers.count(d)) {
                int forward_to = SOCKETS::NO_DESCRIPTOR;

                if (supply_map.count(d)) {
                    forward_to = supply_map[d];
                }
                else if (demand_map.count(d)) {
                    forward_to = demand_map[d];
                }

                if (forward_to == SOCKETS::NO_DESCRIPTOR) {
                    log("Forbidden condition met (%s:%d).", __FILE__, __LINE__);
                }
                else {
                    if (is_verbose()) {
                        log(
                            "%lu byte%s from %s:%s %s sent to %s:%s.",
                            buffer.size(), buffer.size() == 1 ? "" : "s",
                            sockets->get_host(d), sockets->get_port(d),
                            buffer.size() == 1 ? "is" : "are",
                            sockets->get_host(forward_to),
                            sockets->get_port(forward_to)
                        );
                    }

                    sockets->append_outgoing(forward_to, buffer);
                    timestamp_map[forward_to] = timestamp;
                }
            }

            buffer.clear();
            timestamp_map[d] = timestamp;
        }

        uint32_t idle_timeout = get_idle_timeout();

        if (idle_timeout > 0 && alarmed) {
            for (const auto &p : timestamp_map) {
                if (timestamp - p.second >= idle_timeout) {
                    int d = p.first;

                    if (is_verbose()) {
                        log(
                            "Connection %s:%s has timed out (descriptor %d).",
                            sockets->get_host(d), sockets->get_port(d), d
                        );
                    }

                    sockets->disconnect(d);
                }
            }
        }
    }
    while (!terminated);

    return;
}

bool PROGRAM::init(int argc, char **argv) {
    signals = new (std::nothrow) SIGNALS(print_log);
    if (!signals) return false;

    if (!signals->init()) {
        return false;
    }

    options = new (std::nothrow) OPTIONS(get_version(), print_log);
    if (!options) return false;

    if (!options->init(argc, argv)) {
        return false;
    }

    sockets = new (std::nothrow) SOCKETS(print_log);
    if (!sockets) return false;

    if (!sockets->init()) {
        return false;
    }

    return true;
}

int PROGRAM::deinit() {
    if (sockets) {
        if (!sockets->deinit()) {
            status = EXIT_FAILURE;
            bug();
        }

        delete sockets;
        sockets = nullptr;
    }

    if (options) {
        delete options;
        options = nullptr;
    }

    if (signals) {
        delete signals;
        signals = nullptr;
    }

    return get_status();
}

int PROGRAM::get_status() const {
    return status;
}

size_t PROGRAM::get_log_size() {
    return PROGRAM::log_size;
}

bool PROGRAM::print_text(FILE *fp, const char *text, size_t len) {
    // Because fwrite may be interrupted by a signal, we block them.

    sigset_t sigset_all;
    sigset_t sigset_orig;

    if (sigfillset(&sigset_all) == -1) {
        return false;
    }
    else if (sigprocmask(SIG_SETMASK, &sigset_all, &sigset_orig) == -1) {
        return false;
    }

    fwrite(text , sizeof(char), len, fp);

    if (sigprocmask(SIG_SETMASK, &sigset_orig, nullptr) == -1) {
        return false;
    }

    return true;
}

void PROGRAM::print_log(const char *origin, const char *p_fmt, ...) {
    va_list ap;
    char *buf = nullptr;
    char *newbuf = nullptr;
    int buffered = 0;
    int	size = 1024;

    if (p_fmt == nullptr) return;
    buf = (char *) malloc (size * sizeof (char));

    while (1) {
        va_start(ap, p_fmt);
        buffered = vsnprintf(buf, size, p_fmt, ap);
        va_end (ap);

        if (buffered > -1 && buffered < size) break;
        if (buffered > -1) size = buffered + 1;
        else               size *= 2;

        if ((newbuf = (char *) realloc (buf, size)) == nullptr) {
            free (buf);
            return;
        } else {
            buf = newbuf;
        }
    }

    std::string logline;
    logline.reserve(size);

    if (PROGRAM::log_time) {
        char timebuf[20];
        struct timeval timeofday;
        gettimeofday(&timeofday, nullptr);

        time_t timestamp = (time_t) timeofday.tv_sec;
        struct tm *tm_ptr = gmtime(&timestamp);

        if (!strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_ptr)) {
            timebuf[0] = '\0';
        }

        logline.append(timebuf);
        logline.append(" :: ");
    }

    if (origin && *origin) {
        logline.append(origin);
        logline.append(": ");
    }

    logline.append(buf);

    if (origin) logline.append("\n");

    PROGRAM::log_size += logline.size();
    print_text(stderr, logline.c_str(), logline.size());
    free(buf);
}

void PROGRAM::log(const char *p_fmt, ...) {
    va_list ap;
    char *buf = nullptr;
    char *newbuf = nullptr;
    int buffered = 0;
    int	size = 1024;

    if (p_fmt == nullptr) return;
    buf = (char *) malloc (size * sizeof (char));

    while (1) {
        va_start(ap, p_fmt);
        buffered = vsnprintf(buf, size, p_fmt, ap);
        va_end (ap);

        if (buffered > -1 && buffered < size) break;
        if (buffered > -1) size = buffered + 1;
        else               size *= 2;

        if ((newbuf = (char *) realloc (buf, size)) == nullptr) {
            free (buf);
            return;
        } else {
            buf = newbuf;
        }
    }

    print_log("", "%s", buf);
    free(buf);
}

void PROGRAM::bug(const char *file, int line) {
    log("Bug on line %d of %s.", line, file);
}

const char *PROGRAM::get_name() const {
    return pname.c_str();
}

const char *PROGRAM::get_version() const {
    return pver.c_str();
}

uint16_t PROGRAM::get_supply_port() const {
    return options->supply_port;
}

uint16_t PROGRAM::get_demand_port() const {
    return options->demand_port;
}

uint16_t PROGRAM::get_driver_port() const {
    return options->driver_port;
}

bool PROGRAM::is_verbose() const {
    return options->verbose;
}

uint32_t PROGRAM::get_idle_timeout() const {
    return options->idle_timeout;
}

uint32_t PROGRAM::get_driver_period() const {
    return options->driver_period;
}

void PROGRAM::set_timer(size_t usec) {
    timer.it_value.tv_sec     = usec / 1000000;
    timer.it_value.tv_usec    = usec % 1000000;
    timer.it_interval.tv_sec  = 0;
    timer.it_interval.tv_usec = 0;

    setitimer(ITIMER_REAL, &timer, nullptr);
}

long long PROGRAM::get_timestamp() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
