// tasks.cpp - the PS2 task scheduler (0x41ab00-0x41af70) natively, with fibers
//
// The game's logic runs as cooperative tasks (docs/TASKS.md): every vsync tick runs each ready
// task once, in priority order. The original gave every task a Windows thread and handed over
// with SetEvent + WaitForSingleObject, about 14 hand-overs per tick; on Linux each one was a
// futex wake-up and sleep across two host threads (~57 us of CPU each on the PC). Here every
// task is a fiber (runtime.c) on the host thread that runs the tick, and a hand-over is a
// register swap. The task list and the task structures stay in guest memory, exactly as the
// original kept them, so game code that reads them is unaffected; each function below follows
// the disassembly of the one it replaces.

#include "runtime.h"
#include <cstdio>
#include <unordered_map>

#define NATIVE extern "C" void

namespace {

// globals of the original
const uint32_t kHead = 0x758208;        // first task (priority order)
const uint32_t kCurrent = 0x758204;     // running task, 0 = none (the tick's caller runs)
const uint32_t kCurrentId = 0x75820c;
const uint32_t kNextAutoId = 0x678f0c;  // last id given to create(-1, ...)
const uint32_t kRearm = 0x678f08;       // byte: re-arm tasks with flag 4 when the list is done
const uint32_t kFlagEf4 = 0x678ef4, kFlagF10 = 0x678f10;
const uint32_t kMainEvent = 0x678f00, kMainThread = 0x678f04;

// task structure (0x58 bytes)
enum : uint32_t {
    T_FLAGS = 0x00, T_PRIO = 0x01, T_W2 = 0x02, T_ID = 0x04, T_WAIT = 0x08, T_SUSP = 0x0a,
    T_NEXT = 0x0c, T_PREV = 0x10, T_14 = 0x14, T_ARG = 0x1c, T_DATA = 0x20, T_ENTRY = 0x24,
    T_THREAD = 0x28, T_EVENT = 0x2c, T_NAME = 0x30, T_SIZE = 0x58,
};
enum : uint8_t { F_STARTED = 1, F_KEEP = 2, F_REARM = 4, F_DEAD = 8 };

RtThread *g_home;                                   // the context running the tick (message loop)
std::unordered_map<uint32_t, RtThread *> g_fiber;  // task -> its fiber
std::unordered_map<uint32_t, uint32_t> g_ptd;       // task -> its C runtime thread data (_tiddata)
uint32_t g_dead;                                    // task that ended itself, freed after the switch

uint32_t Malloc(uint32_t n) { return rt_call(0x41c490, 1, &n); }
void Free(uint32_t p) { rt_call(0x41c4b0, 1, &p); }

// The original started each task's thread with the C runtime's _beginthread (0x4a397d), which
// allocates and initialises the thread's _tiddata (rand() state at +0x14, errno, ...) in the
// creating thread; _threadstart (0x4a39f4) puts it into the new thread's TLS and calls the
// start function, and _endthread frees it when the thread ends. The same steps are kept here,
// so the per-task C runtime state and the heap layout stay as in the original.
uint32_t BeginThreadData(uint32_t task, uint32_t handle)
{
    uint32_t a[2] = {1, 0x74};
    uint32_t ptd = rt_call(0x4a53a6, 2, a);         // _calloc_crt(1, sizeof(_tiddata))
    if (!ptd) return 0;
    rt_call(0x4a7790, 1, &ptd);                     // _initptd
    WR32(ptd + 0x48, 0x41ab50);                     // start address: the task thread procedure
    WR32(ptd + 0x4c, task);                         // its argument
    WR32(ptd + 0x04, handle);
    return ptd;
}

// Releases a task that is not running (the original's 0x41ab00 did this in the task's own
// thread once woken with F_DEAD: close the event, free the structure, end the thread).
void Destroy(uint32_t task)
{
    auto it = g_fiber.find(task);
    if (it != g_fiber.end()) {
        rt_fiber_destroy(it->second);
        g_fiber.erase(it);
    }
    Free(task);
    auto pt = g_ptd.find(task);                     // then _endthread: _freeptd (0x4a780a)
    if (pt != g_ptd.end()) {
        uint32_t ptd = pt->second;
        g_ptd.erase(pt);
        rt_call(0x4a780a, 1, &ptd);
    }
}

// After every switch: release the task that ended itself before switching away.
void Reap()
{
    if (g_dead && g_fiber.count(g_dead) && g_fiber[g_dead] != rt_self()) {
        uint32_t t = g_dead;
        g_dead = 0;
        Destroy(t);
    }
}

// Hand over to `to` (0 = the tick's caller) and return when this context runs again.
void SwitchTo(uint32_t to)
{
    RtThread *target = to ? g_fiber[to] : g_home;
    if (!target) RT_Fatal("task without a fiber", to);
    rt_fiber_switch(target);
    Reap();
}

// 0x41ab70: from `start` on, count the wait counters of the tasks that are not suspended down
// and run the first that reaches 0; with none left the tick's caller continues. The caller
// waits until it gets its turn again (a finished caller never does).
void Dispatch(uint32_t start)
{
    WR8(kFlagEf4, 0);
    uint32_t t = start;
    while (t) {
        if (RD16(t + T_SUSP) == 0) {
            uint16_t w = (uint16_t)(RD16(t + T_WAIT) - 1);
            WR16(t + T_WAIT, w);
            if (w == 0) break;
        }
        t = RD32(t + T_NEXT);
    }
    uint32_t old = RD32(kCurrent);
    WR32(kCurrent, t);
    if (t) {
        WR32(kCurrentId, RD32(t + T_ID));
        if (!(RD8(t + T_FLAGS) & F_STARTED)) WR8(t + T_FLAGS, RD8(t + T_FLAGS) | F_STARTED);
    } else if (RD8(kRearm)) {
        WR8(kRearm, 0);
        for (uint32_t u = RD32(kHead); u; u = RD32(u + T_NEXT))
            if (RD8(u + T_FLAGS) & F_REARM) {
                WR8(u + T_FLAGS, RD8(u + T_FLAGS) & ~F_REARM);
                WR16(u + T_WAIT, 1);
            }
    }
    if (old && (RD8(old + T_FLAGS) & F_DEAD)) g_dead = old;
    SwitchTo(t);
}

// 0x41ac30: unlink a task (the list head is not updated, as in the original), mark it dead and
// release it unless it is the running one; returns the next task.
uint32_t Unlink(uint32_t t)
{
    uint32_t prev = RD32(t + T_PREV), next = RD32(t + T_NEXT);
    if (prev) WR32(prev + T_NEXT, next);
    if (next) WR32(next + T_PREV, prev);
    uint8_t f = RD8(t + T_FLAGS);
    if (!(f & F_KEEP)) {
        WR8(t + T_FLAGS, f | F_DEAD);
        if (t != RD32(kCurrent)) Destroy(t);
    }
    return next;
}

// A task's fiber: the C runtime's _threadstart(ptd), which calls 0x41ab50(task) (below).
void TaskMain(void *p)
{
    uint32_t task = (uint32_t)(uintptr_t)p;
    Reap();
    uint32_t ptd = g_ptd.count(task) ? g_ptd[task] : 0;
    if (ptd) {
        rt_call(0x4a39f4, 1, &ptd);
    } else {
        uint32_t t = task;
        rt_call(0x41ab50, 1, &t);
    }
    RT_Fatal("a finished task was resumed", task);
}

// 0x41ae30: op 0 kill, 1 suspend, 2 resume the tasks with `id` (-1: all but the running one);
// kill starts after the first task. If the running task was hit, dispatch from its successor.
void Control(int32_t id, int32_t op)
{
    uint32_t cur = RD32(kCurrent), head = RD32(kHead);
    uint32_t from = cur ? RD32(cur + T_NEXT) : 0;
    int self = 0;
    uint32_t t = op == 0 ? RD32(head + T_NEXT) : head;
    while (t) {
        uint32_t next = RD32(t + T_NEXT);
        bool apply;
        if (id == -1) {
            apply = t != cur;
        } else {
            apply = (int32_t)RD32(t + T_ID) == id;
            if (apply && t == cur && op != 2) { self = 1; from = next; }
        }
        if (apply) {
            if (op == 0) {
                if (t == from) from = next;
                Unlink(t);
            } else {
                int16_t s = (int16_t)RD16(t + T_SUSP);
                s = op == 1 ? s + 1 : s - 1;
                WR16(t + T_SUSP, (uint16_t)(s < 0 ? 0 : s));
            }
        }
        t = next;
        cur = RD32(kCurrent);
    }
    if (self) Dispatch(from);
}

}  // namespace

// 0x41acc0: empty task list
NATIVE N_0041acc0(void)
{
    WR32(kNextAutoId, 0xffffffffu);
    WR32(kHead, 0);
    WR8(kFlagEf4, 0);
    WR8(kFlagF10, 0);
    WR32(kMainEvent, 1);        // the original's event and thread handles; nothing reads them
    WR32(kMainThread, 1);
    g_home = rt_self();         // PS2 main, then the message loop on the same thread
    RT_RETURN(0, 0);
}

// 0x41ad00: create(id, entry, arg, stack, priority | wait << 8) -> id
NATIVE N_0041ad00(void)
{
    int32_t id = (int32_t)ARG(0);
    uint32_t entry = ARG(1), arg = ARG(2), prioWait = ARG(4);
    uint32_t t = Malloc(T_SIZE);
    if (!t) RT_RETURN(0xffffffffu, 0);
    if (id == -1) {                                 // next free negative id (the original's scan)
        id = (int32_t)RD32(kNextAutoId) - 1;
        if (id >= 0) id = -2;
        for (uint32_t u = RD32(kHead); u;) {
            if ((int32_t)RD32(u + T_ID) == id) id--;
            else u = RD32(u + T_NEXT);
        }
        WR32(kNextAutoId, (uint32_t)id);
    }
    uint8_t prio = (uint8_t)prioWait;
    int32_t wait = (int32_t)prioWait >> 8;
    WR32(t + T_ENTRY, entry);
    WR32(t + T_ARG, arg);
    WR32(t + T_ID, (uint32_t)id);
    WR8(t + T_PRIO, prio);
    WR8(t + T_FLAGS, wait ? F_REARM : 0);
    if (wait) WR8(kRearm, prio);
    WR16(t + T_WAIT, (uint16_t)(wait + 1));
    WR16(t + T_SUSP, 0);
    WR16(t + T_W2, 0);
    WR32(t + T_DATA, t + T_SIZE);
    WR32(t + T_14, 0);
    // insert before the first task with a higher priority number, else at the end
    uint32_t u = RD32(kHead);
    if (!u) {
        WR32(t + T_NEXT, 0);
        WR32(t + T_PREV, 0);
        WR32(kHead, t);
    } else {
        while (RD8(u + T_PRIO) <= prio && RD32(u + T_NEXT)) u = RD32(u + T_NEXT);
        if (RD8(u + T_PRIO) > prio) {
            uint32_t prev = RD32(u + T_PREV);
            WR32(u + T_PREV, t);
            if (prev) WR32(prev + T_NEXT, t);
            else WR32(kHead, t);                    // (the original would write through 0 here)
            WR32(t + T_PREV, prev);
            WR32(t + T_NEXT, u);
        } else {
            WR32(u + T_NEXT, t);
            WR32(t + T_PREV, u);
            WR32(t + T_NEXT, 0);
        }
    }
    char name[16];
    snprintf(name, sizeof(name), "TASK:%02X:%02X", (unsigned)id, (unsigned)prio);
    for (int i = 0; i < 16; i++) WR8(t + T_NAME + i, (uint8_t)name[i]);
    WR32(t + T_EVENT, 0);
    RtThread *f = rt_fiber_create(TaskMain, (void *)(uintptr_t)t);
    g_fiber[t] = f;
    WR32(t + T_THREAD, rt_handle_of(f));
    if (uint32_t ptd = BeginThreadData(t, rt_handle_of(f))) g_ptd[t] = ptd;
    RT_RETURN((uint32_t)id, 0);
}

// 0x41af10: vsync tick (message loop): run the ready tasks from the head
NATIVE N_0041af10(void)
{
    if (!g_home) g_home = rt_self();
    else if (g_home != rt_self()) RT_Fatal("task tick from another thread", 0x41af10);
    Dispatch(RD32(kHead));
    RT_RETURN(0, 0);
}

// 0x41af20: sleep(n) - the running task waits n ticks
NATIVE N_0041af20(void)
{
    uint32_t cur = RD32(kCurrent);
    WR16(cur + T_WAIT, ARG(0));
    WR16(cur + T_W2, 0);
    Dispatch(RD32(cur + T_NEXT));
    RT_RETURN(0, 0);
}

// 0x41ae30: control(id, op)
NATIVE N_0041ae30(void)
{
    Control((int32_t)ARG(0), (int32_t)ARG(1));
    RT_RETURN(0, 0);
}

// 0x41ac90: end all tasks (program exit)
NATIVE N_0041ac90(void)
{
    if (RD32(kHead)) {
        Control(-1, 0);
        Unlink(RD32(kHead));
    }
    RT_RETURN(0, 0);
}

// 0x41ab50: task thread procedure (entered from _threadstart): entry(arg), then the task ends.
// (The original first waited for the task's first turn; a fiber only starts at its first turn.)
NATIVE N_0041ab50(void)
{
    uint32_t task = ARG(0), arg = RD32(task + T_ARG);
    rt_call(RD32(task + T_ENTRY), 1, &arg);
    Dispatch(Unlink(RD32(kCurrent)));
    RT_Fatal("a finished task was resumed", task);
}

// 0x41aef0: the running task ends
NATIVE N_0041aef0(void)
{
    Dispatch(Unlink(RD32(kCurrent)));
    RT_Fatal("a finished task was resumed", 0x41aef0);
}

// 0x41af50: number of tasks with this id
NATIVE N_0041af50(void)
{
    uint32_t n = 0, id = ARG(0);
    for (uint32_t t = RD32(kHead); t; t = RD32(t + T_NEXT))
        if (RD32(t + T_ID) == id) n++;
    RT_RETURN(n, 0);
}
