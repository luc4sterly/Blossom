#include <cstdio>
#include <algorithm>
#include "common/exceptions.hpp"
#include "core/scheduler.hpp"

Scheduler::Scheduler()
{

}

void Scheduler::reset()
{
    events.clear();

    closest_event_time = 0;

    quantum.count = 0;
    quantum.remainder = 0;

    cycles11.count = 0;
    cycles11.remainder = 0;

    cycles9.count = 0;
    cycles9.remainder = 0;

    xtensa_cycles.count = 0;
    xtensa_cycles.remainder = 0;
}

void Scheduler::calculate_cycles_to_run()
{
    const int MAX_CYCLES = max_cycles;

    cycles11_to_run = 0;

    //Up to the next event, at most one slice. An overdue event gives 0. (The clamp used to sit in
    //an inner variable that shadowed this one, so an overdue event gave a negative slice that
    //wrapped to a budget of over a billion cycles when cast to int.)
    int64_t delta = closest_event_time - quantum.count;
    if (delta > MAX_CYCLES)
        delta = MAX_CYCLES;
    if (delta < 0)
        delta = 0;

    quantum_cycles = delta;

    cycles11_to_run = delta * cycles11.clockrate / quantum.clockrate;
    cycles11.remainder = delta * cycles11.clockrate % quantum.clockrate;
    if (cycles11.remainder >= quantum.clockrate / cycles11.clockrate)
    {
        cycles11_to_run++;
        cycles11.remainder -= quantum.clockrate / cycles11.clockrate;
    }

    cycles9_to_run = delta * cycles9.clockrate / quantum.clockrate;
    cycles9.remainder = delta * cycles9.clockrate % quantum.clockrate;
    if (cycles9.remainder >= quantum.clockrate / cycles9.clockrate)
    {
        cycles9_to_run++;
        cycles9.remainder -= quantum.clockrate / cycles9.clockrate;
    }

    xtensa_cycles_to_run = delta * xtensa_cycles.clockrate / quantum.clockrate;
    xtensa_cycles.remainder = delta * xtensa_cycles.clockrate % quantum.clockrate;
    if (xtensa_cycles.remainder >= quantum.clockrate / xtensa_cycles.clockrate)
    {
        xtensa_cycles_to_run++;
        xtensa_cycles.remainder -= quantum.clockrate / xtensa_cycles.clockrate;
    }
}

void Scheduler::add_event(std::function<void(uint64_t)> func, int64_t cycles, uint64_t clockrate, uint64_t param)
{
    SchedulerEvent event;

    if (cycles < 0)
    {
        static int warned = 0;
        if (warned++ < 8)
            fprintf(stderr, "[Scheduler] Event scheduled %lld cycles in the past, running it now\n", (long long)cycles);
        cycles = 0;
    }

    event.func = func;
    event.time_registered = -1; //TODO

    event.time_to_run = quantum.count + (cycles * (quantum.clockrate / clockrate));
    event.param = param;

    closest_event_time = std::min(event.time_to_run, closest_event_time);

    events.push_back(event);

    //Nothing schedules more than a handful of events at once; a runaway (a device rescheduling
    //itself in a loop) would otherwise eat all memory before anything shows up
    if (events.size() > 100000)
        EmuException::die("[Scheduler] More than 100000 pending events, something is rescheduling itself in a loop");
}

void Scheduler::process_events()
{
    quantum.count += quantum_cycles;
    cycles11.count += cycles11_to_run;
    cycles9.count += cycles9_to_run;
    xtensa_cycles.count += xtensa_cycles_to_run;
    if (quantum.count >= closest_event_time)
    {
        int64_t new_time = 0x7FFFFFFFULL << 32ULL;
        for (auto it = events.begin(); it != events.end(); )
        {
            if (it->time_to_run <= closest_event_time)
            {
                it->func(it->param);
                it = events.erase(it);
            }
            else
            {
                new_time = std::min(it->time_to_run, new_time);
                it++;
            }
        }
        closest_event_time = new_time;
    }
}
