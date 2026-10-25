/* Cooperative suspension is checked across a real machine-ticket race. */
#define _GNU_SOURCE
#include <assert.h>
#include "../android/host/threads.c"
void void_log(const char *format, ...){(void)format;}

static int acquired;
static cpu_set_t inherited_cores;
static void *check_host_affinity(void *arg)
{
    (void)arg;
    cpu_set_t actual;
    assert(!sched_getaffinity(0, sizeof actual, &actual) && CPU_EQUAL(&actual, &inherited_cores));
    void_restore_host_affinity();
    assert(!sched_getaffinity(0, sizeof actual, &actual) && CPU_EQUAL(&actual, &host_cores));
    return NULL;
}
static void *claim_machine(void *arg)
{
    thread_machine_acquire((void_thread *)arg);
    __atomic_store_n(&acquired, 1, __ATOMIC_RELEASE);
    machine_release();
    return NULL;
}
static void pause_briefly(void)
{
    struct timespec ts = { 0, 20000000 };
    nanosleep(&ts, NULL);
}

int main(void)
{
    assert(!sched_getaffinity(0, sizeof host_cores, &host_cores));
    host_cores_valid = 1;
    CPU_ZERO(&inherited_cores);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &host_cores)) { CPU_SET(cpu, &inherited_cores); break; }
    assert(!sched_setaffinity(0, sizeof inherited_cores, &inherited_cores));
    pthread_t host_thread;
    assert(!pthread_create(&host_thread, NULL, check_host_affinity, NULL));
    pthread_join(host_thread, NULL);
    void_restore_host_affinity();

    pthread_once(&big_cores_once, big_cores_init);
    big_cores = inherited_cores; big_cores_count = 1;
    void_thread guest = {0};
    /* A driver or Android task-profile change can widen a running thread's mask. */
    assert(!sched_setaffinity(0, sizeof host_cores, &host_cores));
    thread_machine_acquire(&guest);
    cpu_set_t actual;
    assert(!sched_getaffinity(0, sizeof actual, &actual) && CPU_EQUAL(&actual, &big_cores));
    machine_release(); void_restore_host_affinity();
    guest.suspended = 2;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, claim_machine, &guest));
    pause_briefly();
    pthread_mutex_lock(&wait_lock);
    assert(guest.suspended == 2 && !__atomic_load_n(&acquired, __ATOMIC_ACQUIRE));
    --guest.suspended;
    pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_lock);
    pause_briefly();
    assert(!__atomic_load_n(&acquired, __ATOMIC_ACQUIRE));
    pthread_mutex_lock(&wait_lock);
    --guest.suspended;
    pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_lock);
    pthread_join(thread, NULL);
    assert(__atomic_load_n(&acquired, __ATOMIC_ACQUIRE));

    acquired = 0;
    machine_acquire();
    assert(!pthread_create(&thread, NULL, claim_machine, &guest));
    for (;;) {
        pthread_mutex_lock(&mlock);
        int waiting = ticket_next - ticket_serving == 2;
        pthread_mutex_unlock(&mlock);
        if (waiting) break;
        sched_yield();
    }
    pthread_mutex_lock(&wait_lock); guest.suspended = 1; pthread_mutex_unlock(&wait_lock);
    machine_release();
    pause_briefly();
    assert(!__atomic_load_n(&acquired, __ATOMIC_ACQUIRE));
    pthread_mutex_lock(&wait_lock);
    guest.suspended = 0; pthread_cond_broadcast(&wait_cond);
    pthread_mutex_unlock(&wait_lock);
    pthread_join(thread, NULL);
    assert(__atomic_load_n(&acquired, __ATOMIC_ACQUIRE));
    puts("threads_selfcheck: guest affinity restored after reset, host mixer affinity, nested suspension and suspension racing a pending machine ticket PASS");
    return 0;
}
