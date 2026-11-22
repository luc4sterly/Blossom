#pragma once

//Debug aid: sample the native call stack of one thread from another (the watchdog), so that a
//frame stuck somewhere in C++ can be located with addr2line. Windows x64 only for now.
namespace NativeStack
{

//Call on the thread to sample later
void set_target_thread();

//Suspend the target thread, walk its stack and print it to stderr; samples times, gap_ms apart
void report(int samples, int gap_ms);

}
