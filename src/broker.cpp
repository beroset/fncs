/* autoconf header */
#include "config.h"

/* C++ standard headers */
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <vector>

/* 3rd party headers */
#include "czmq.h"

/* fncs headers */
#include "echo.hpp"
#include "fncs.hpp"
#include "fncs_internal.hpp"

using namespace ::std;
using fncs::Echo;

#define TRACE echo << "TRACE: "
#define WARN  echo << "INFO:  "
#define FATAL echo << "FATAL: "


class SimulatorState {
    public:
        SimulatorState()
            : name("")
            , time_requested(0)
            , time_delta(0)
            , time_last_processed(0)
            , processing(true)
            , messages_pending(false)
        {}

        string name;
        fncs::time time_requested;
        fncs::time time_delta;
        fncs::time time_last_processed;
        bool processing;
        bool messages_pending;
        set<string> subscription_values;
};

typedef map<string,size_t> SimIndex;
typedef vector<SimulatorState> SimVec;

static fncs::time time_real_start;
static fncs::time time_real;
static ofstream trace; /* the trace stream, if requested */

static inline void broker_die(const SimVec &simulators, zsock_t *server) {
    /* repeat the fatal die to all connected sims */
    for (size_t i=0; i<simulators.size(); ++i) {
        zstr_sendm(server, simulators[i].name.c_str());
        zstr_send(server, fncs::DIE);
    }
    zsock_destroy(&server);
    if (trace.is_open()) {
        trace.close();
    }
    exit(EXIT_FAILURE);
}

static void time_real_update(void)
{
    time_real = fncs::timer_ft() - time_real_start;
}




int main(int argc, char **argv)
{
    /* declare all variables */
    unsigned int n_sims = 0;    /* how many sims will connect */
    set<string> byes;           /* which sims have disconnected */
    int n_processing = 0;       /* how many sims are processing a time step */
    const char *endpoint = NULL;/* broker location */
    SimVec simulators;          /* vector of connected simulator state */
    SimIndex name_to_index;     /* quickly lookup sim state index */
    fncs::time time_granted = 0;/* global clock */
    zsock_t *server = NULL;     /* the broker socket */
    bool do_trace = false;      /* whether to dump all received messages */
    Echo echo;
    fncs::time realtime_interval = 0;

    fncs::start_logging(echo);

    /* how many simulators are connecting? */
    if (argc > 3) {
        FATAL << "too many command line args" << endl;
        exit(EXIT_FAILURE);
    }
    if (argc < 2) {
        FATAL << "missing command line arg for number of simulators" << endl;
        exit(EXIT_FAILURE);
    }
    if (argc >= 2) {
        int n_sims_signed = 0;
        istringstream iss(argv[1]);
        iss >> n_sims_signed;
        TRACE << "n_sims_signed = " << n_sims_signed << endl;
        if (n_sims_signed <= 0) {
            FATAL << "number of simulators arg must be >= 1" << endl;
            exit(EXIT_FAILURE);
        }
        n_sims = static_cast<unsigned int>(n_sims_signed);
    }
    if (argc == 3) {
        realtime_interval = fncs::parse_time(argv[2]);
        TRACE << "realtime_interval = " << realtime_interval << " ns" << endl;
    }

    {
        const char *env_do_trace = getenv("FNCS_TRACE");
        if (env_do_trace) {
            if (env_do_trace[0] == 'Y'
                    || env_do_trace[0] == 'y'
                    || env_do_trace[0] == 'T'
                    || env_do_trace[0] == 't') {
                do_trace = true;
            }
        }
    }

    if (do_trace) {
        TRACE << "tracing of all published messages enabled" << endl;
        trace.open("broker_trace.txt");
        if (!trace) {
            FATAL << "Could not open trace file 'broker_trace.txt'" << endl;
            exit(EXIT_FAILURE);
        }
        trace << "#nanoseconds\ttopic\tvalue" << endl;
    }

    /* broker endpoint may come from env var */
    endpoint = getenv("FNCS_BROKER");
    if (!endpoint) {
        endpoint = "tcp://*:5570";
    }

    server = zsock_new_router(endpoint);
    if (!server) {
        FATAL << "socket creation failed" << endl;
        exit(EXIT_FAILURE);
    }
    if (!(zsock_resolve(server) != server)) {
        FATAL << "socket failed to resolve" << endl;
        exit(EXIT_FAILURE);
    }
    TRACE << "broker socket bound to " << endpoint << endl;

    /* begin event loop */
    zmq_pollitem_t items[] = { { zsock_resolve(server), 0, ZMQ_POLLIN, 0 } };
    while (true) {
        int rc = 0;
        
        TRACE << "entering blocking poll" << endl;
        rc = zmq_poll(items, 1, -1);
        if (rc == -1) {
            FATAL << "broker polling error: " << strerror(errno) << endl;
            broker_die(simulators, server); /* interrupted */
        }

        if (items[0].revents & ZMQ_POLLIN) {
            zmsg_t *msg = NULL;
            zframe_t *frame = NULL;
            string sender;
            string message_type;

            TRACE << "incoming message" << endl;
            msg = zmsg_recv(server);
            if (!msg) {
                FATAL << "null message received" << endl;
                broker_die(simulators, server);
            }

            /* first frame is sender */
            frame = zmsg_first(msg);
            if (!frame) {
                FATAL << "message missing sender" << endl;
                broker_die(simulators, server);
            }
            sender = fncs::to_string(frame);

            /* next frame is message type identifier */
            frame = zmsg_next(msg);
            if (!frame) {
                FATAL << "message missing type identifier" << endl;
                broker_die(simulators, server);
            }
            message_type = fncs::to_string(frame);
            
            /* dispatcher */
            if (fncs::HELLO == message_type) {
                SimulatorState state;
                zchunk_t *chunk = NULL;
                zconfig_t *config = NULL;
                zconfig_t *config_values = NULL;
                const char * time_delta = NULL;

                TRACE << "HELLO received" << endl;

                /* check for duplicate sims */
                if (name_to_index.count(sender) != 0) {
                    FATAL << "simulator '" << sender << "' already connected" << endl;
                    broker_die(simulators, server);
                }
                TRACE << "registering client '" << sender << "'" << endl;

                /* next frame is config chunk */
                frame = zmsg_next(msg);
                if (!frame) {
                    FATAL << "HELLO message missing config frame" << endl;
                    broker_die(simulators, server);
                }

                /* copy config frame into chunk */
                chunk = zchunk_new(zframe_data(frame), zframe_size(frame));
                if (!chunk) {
                    FATAL << "HELLO message zconfig bad config chunk" << endl;
                    broker_die(simulators, server);
                }

                /* parse config chunk */
                config = zconfig_chunk_load(chunk);
                if (!config) {
                    FATAL << "HELLO message bad config" << endl;
                    broker_die(simulators, server);
                }

                /* done with chunk */
                zchunk_destroy(&chunk);

                /* get time delta from config */
                time_delta = zconfig_resolve(config, "/time_delta", NULL);
                if (!time_delta) {
                    WARN << sender << " config does not contain 'time_delta'" << endl;
                    WARN << sender << " time_delta defaulting to 1s" << endl;
                    time_delta = "1s";
                }

                /* parse subscription values */
                set<string> subscription_values;
                config_values = zconfig_locate(config, "/values");
                if (config_values) {
                    vector<fncs::Subscription> subs =
                        fncs::parse_values(config_values);
                    for (size_t i=0; i<subs.size(); ++i) {
                        TRACE << "adding value '" << subs[i].topic << "'" << endl;
                        subscription_values.insert(subs[i].topic);
                    }
                }
                else {
                    TRACE << "no subscription values" << endl;
                }

                /* populate sim state object */
                state.name = sender;
                state.time_delta = fncs::parse_time(time_delta);
                state.time_requested = 0;
                state.time_last_processed = 0;
                state.processing = false;
                state.messages_pending = false;
                state.subscription_values = subscription_values;
                name_to_index[sender] = simulators.size();
                simulators.push_back(state);

                TRACE << "simulators.size() = " << simulators.size() << endl;

                /* if all sims have connected, send the go-ahead */
                if (simulators.size() == n_sims) {
                    time_real_start = fncs::timer_ft();
                    time_real = 0;
                    if (realtime_interval) {
                        struct itimerval it_val;  /* for setting itimer */

                        /* setitimer call needs seconds and useconds */
                        if (signal(SIGALRM, (void (*)(int)) time_real_update) == SIG_ERR) {
                            perror("Unable to catch SIGALRM");
                            exit(EXIT_FAILURE);
                        }
                        it_val.it_value.tv_sec = realtime_interval/1000000000UL;
                        TRACE << "realtime_sec = " << it_val.it_value.tv_sec << endl;
                        it_val.it_value.tv_usec = realtime_interval/1000 % 1000000;
                        TRACE << "realtime_usec = " << it_val.it_value.tv_usec << endl;
                        it_val.it_interval = it_val.it_value;
                        if (setitimer(ITIMER_REAL, &it_val, NULL) == -1) {
                            broker_die(simulators, server);
                        }
                    }
                    /* easier to keep a counter than iterating over states */
                    n_processing = n_sims;
                    /* send ACK to all registered sims */
                    for (size_t i=0; i<n_sims; ++i) {
                        simulators[i].processing = true;
                        zstr_sendm(server, simulators[i].name.c_str());
                        zstr_sendm(server, fncs::ACK);
                        zstr_sendfm(server, "%llu", (unsigned long long)i);
                        zstr_sendf(server, "%llu", (unsigned long long)n_sims);
                        TRACE << "ACK sent to '" << simulators[i].name << endl;
                    }
                }
            }
            else if (fncs::TIME_REQUEST == message_type
                    || fncs::BYE == message_type) {
                size_t index = 0; /* index of sim state */
                fncs::time time_requested;

                if (fncs::TIME_REQUEST == message_type) {
                    TRACE << "TIME_REQUEST received" << endl;
                }
                else if (fncs::BYE == message_type) {
                    TRACE << "BYE received" << endl;
                }

                /* did we receive message from a connected sim? */
                if (name_to_index.count(sender) == 0) {
                    FATAL << "simulator '" << sender << "' not connected" << endl;
                    broker_die(simulators, server);
                }

                /* index of sim state */
                index = name_to_index[sender];

                if (fncs::BYE == message_type) {
                    /* soft error if muliple byes received */
                    if (byes.count(sender)) {
                        WARN << "duplicate BYE from '" << sender << "'" << endl;
                    }

                    /* add sender to list of leaving sims */
                    byes.insert(sender);

                    /* if all byes received, then exit */
                    if (byes.size() == n_sims) {
                        /* let all sims know that globally we are finished */
                        for (size_t i=0; i<n_sims; ++i) {
                            zstr_sendm(server, simulators[i].name.c_str());
                            zstr_send(server, fncs::BYE);
                            TRACE << "BYE sent to '" << simulators[i].name << endl;
                        }
                        /* need to delete msg since we are breaking from loop */
                        zmsg_destroy(&msg);
                        break;
                    }

                    /* update sim state */
                    simulators[index].time_requested = ULLONG_MAX;
                }
                else if (fncs::TIME_REQUEST == message_type) {
                    /* next frame is time */
                    frame = zmsg_next(msg);
                    if (!frame) {
                        FATAL << "TIME_REQUEST message missing time frame" << endl;
                        broker_die(simulators, server);
                    }
                    /* convert time string */
                    {
                        istringstream iss(fncs::to_string(frame));
                        iss >> time_requested;
                    }

                    /* update sim state */
                    simulators[index].time_requested = time_requested;
                }

                /* update sim state */
                simulators[index].time_last_processed = time_granted;
                simulators[index].processing = false;

                --n_processing;

                /* if all sims are done, determine next time step */
                if (0 == n_processing) {
                    vector< fncs::time> time_actionable(n_sims);
                    for (size_t i=0; i<n_sims; ++i) {
                        if (simulators[i].messages_pending) {
                            time_actionable[i] = 
                                  simulators[i].time_last_processed
                                + simulators[i].time_delta;
                        }
                        else {
                            time_actionable[i] = simulators[i].time_requested;
                        }
                    }
                    time_granted = *min_element(time_actionable.begin(),
                                                time_actionable.end());
                    TRACE << "time_granted = " << time_granted << endl;
                    if (realtime_interval) {
                        TRACE << "time_real = " << time_real << endl;
                        while (time_granted > time_real) {
                            useconds_t u = (time_granted-time_real)/1000;
                            TRACE << "usleep(" << u << ")" << endl;
                            usleep(u);
                        }
                        TRACE << "time_real = " << time_real << endl;
                    }
                    for (size_t i=0; i<n_sims; ++i) {
                        if (time_granted == time_actionable[i]) {
                            TRACE << "granting " << time_granted
                                << " to " << simulators[i].name << endl;
                            ++n_processing;
                            simulators[i].processing = true;
                            simulators[i].messages_pending = false;
                            zstr_sendm(server, simulators[i].name.c_str());
                            zstr_sendm(server, fncs::TIME_REQUEST);
                            zstr_sendf(server, "%llu", time_granted);
                        }
                        else {
                            /* fast forward time last processed */
                            fncs::time jump = (time_granted - simulators[i].time_last_processed) / simulators[i].time_delta;
                            simulators[i].time_last_processed += simulators[i].time_delta * jump;
                        }
                    }
                }
            }
            else if (fncs::PUBLISH == message_type) {
                string topic = "";
                bool found_one = false;

                TRACE << "PUBLISH received" << endl;

                /* did we receive message from a connected sim? */
                if (name_to_index.count(sender) == 0) {
                    FATAL << "simulator '" << sender << "' not connected" << endl;
                    broker_die(simulators, server);
                }

                /* next frame is topic */
                frame = zmsg_next(msg);
                if (!frame) {
                    FATAL << "PUBLISH message missing topic" << endl;
                    broker_die(simulators, server);
                }
                topic = fncs::to_string(frame);

                if (do_trace) {
                    /* next frame is value payload */
                    frame = zmsg_next(msg);
                    if (!frame) {
                        FATAL << "PUBLISH message missing value" << endl;
                        broker_die(simulators, server);
                    }
                    string value = fncs::to_string(frame);
                    trace << time_granted
                        << "\t" << topic
                        << "\t" << value
                        << endl;
                }

                /* send the message to subscribed sims */
                for (size_t i=0; i<n_sims; ++i) {
                    bool found = false;
                    if (simulators[i].subscription_values.count(topic)) {
                        found = true;
                    }
                    if (found) {
                        zmsg_t *msg_copy = zmsg_dup(msg);
                        if (!msg_copy) {
                            FATAL << "failed to copy pub message" << endl;
                            broker_die(simulators, server);
                        }
                        /* swap out original sender with new destiation */
                        zframe_reset(zmsg_first(msg_copy),
                                simulators[i].name.c_str(),
                                simulators[i].name.size());
                        /* send it on */
                        zmsg_send(&msg_copy, server);
                        found_one = true;
                        simulators[i].messages_pending = true;
                        TRACE << "pub to " << simulators[i].name << endl;
                    }
                }
                if (!found_one) {
                    TRACE << "dropping PUBLISH message '" << topic << "'" << endl;
                }
            }
            else if (fncs::DIE == message_type) {
                TRACE << "DIE received" << endl;

                /* did we receive message from a connected sim? */
                if (name_to_index.count(sender) == 0) {
                    FATAL << "simulator '" << sender << "' not connected" << endl;
                    broker_die(simulators, server);
                }

                broker_die(simulators, server);
            }
            else if (fncs::TIME_DELTA == message_type) {
                size_t index = 0; /* index of sim state */
                fncs::time time_delta;

                TRACE << "TIME_DELTA received" << endl;

                /* did we receive message from a connected sim? */
                if (name_to_index.count(sender) == 0) {
                    FATAL << "simulator '" << sender << "' not connected" << endl;
                    broker_die(simulators, server);
                }

                /* index of sim state */
                index = name_to_index[sender];

                /* next frame is time */
                frame = zmsg_next(msg);
                if (!frame) {
                    FATAL << "TIME_DELTA message missing time frame" << endl;
                    broker_die(simulators, server);
                }
                /* convert time string */
                {
                    istringstream iss(fncs::to_string(frame));
                    iss >> time_delta;
                }

                /* update sim state */
                simulators[index].time_delta = time_delta;
            }
            else {
                FATAL << "received unknown message type '"
                    << message_type << "'" << endl;
                broker_die(simulators, server);
            }

            zmsg_destroy(&msg);
        }
    }

    zsock_destroy(&server);

    if (trace.is_open()) {
        trace.close();
    }

    return 0;
}

