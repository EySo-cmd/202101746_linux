#define _GNU_SOURCE
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string.h>

#define NPROC 10

typedef enum {
    STATE_READY,
    STATE_RUNNING,
    STATE_SLEEP,
    STATE_FIN
} proc_state;

typedef enum {
    POLICY_RR = 0,
    POLICY_FCFS = 1
} sched_policy;

typedef struct {
    pid_t pid;
    int remaining_quantum;
    proc_state state;
    int io_remaining;     
    int wait_time;        // READY 큐에서 기다린 시간
} PCB;

// 전역변수 헷갈려서 주석
static PCB pcbs[NPROC];
static int base_quantum = 3;
static int now_index = -1;
static int alive_procs = 0;
static volatile sig_atomic_t tick_occurred = 0;
static volatile sig_atomic_t total_ticks = 0;
static sched_policy g_policy = POLICY_RR;

static int find_index(pid_t pid) {
    for(int i = 0; i < NPROC; i++) {
        if(pcbs[i].pid == pid) return i;
    }
    return -1;
}

static int all_quantum_zero(void) {
    for(int i = 0; i < NPROC; i++) {
        if(pcbs[i].state != STATE_FIN && pcbs[i].remaining_quantum > 0)
            return 0;
    }
    return 1;
}

static void reset_all_quantum(void) {
    for(int i = 0; i < NPROC; i++) {
        if(pcbs[i].state != STATE_FIN) pcbs[i].remaining_quantum = base_quantum;
    }
}

static int pick_next_process(void) {
    if(alive_procs == 0) return -1;

    if(g_policy == POLICY_FCFS) {
        for(int i = 0; i < NPROC; i++) {
            if(pcbs[i].state == STATE_READY) return i;
        }
        return -1;
    }

    // POLICY_RR
    int start = now_index;
    int i = (now_index + 1) % NPROC;

    if(now_index == -1) {
        start = NPROC - 1;
        i = 0;
    }

    while(1) {
        if(pcbs[i].state == STATE_READY && pcbs[i].remaining_quantum > 0) return i;
        i = (i + 1) % NPROC;
        if(i == (start + 1) % NPROC) break;
    }
    return -1;
}

// 부모 시그널 핸들러

static void sigusr2_handler(int signo, siginfo_t *info, void *context) {
    (void)signo; (void)context;
    pid_t sender = info->si_pid;
    int idx = find_index(sender);
    if(idx == -1) return;

    int io_time = (rand() % 5) + 1; // 1~5 ticks
    pcbs[idx].state = STATE_SLEEP;
    pcbs[idx].io_remaining = io_time;

    if(now_index == idx) now_index = -1;

    // 로그용
    // printf("[Scheduler] PID %d requested I/O, sleep %d ticks\n", sender, io_time);
}

static void sigchld_handler(int signo) {
    (void)signo;
    int status;
    pid_t pid;

    while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int idx = find_index(pid);
        if(idx != -1 && pcbs[idx].state != STATE_FIN) {
            pcbs[idx].state = STATE_FIN;
            pcbs[idx].remaining_quantum = 0;
            pcbs[idx].io_remaining = 0;
            alive_procs--;
            if(now_index == idx) now_index = -1;
        }
    }
}

static void sigalrm_handler(int signo) {
    (void)signo;
    tick_occurred = 1;
    total_ticks++;

    // 1. SLEEP I/O 감소
    for(int i = 0; i < NPROC; i++) {
        if(pcbs[i].state == STATE_SLEEP && pcbs[i].io_remaining > 0) {
            pcbs[i].io_remaining--;
            if(pcbs[i].io_remaining == 0) pcbs[i].state = STATE_READY;
        }
    }

    // 2. READY 대기시간 증가
    for (int i = 0; i < NPROC; i++) {
        if (pcbs[i].state == STATE_READY) pcbs[i].wait_time++;
    }

    // 3. RUNNING 처리
    if(now_index != -1 && pcbs[now_index].state == STATE_RUNNING) {
        if(g_policy == POLICY_RR) {
            pcbs[now_index].remaining_quantum--;
            if(pcbs[now_index].remaining_quantum <= 0) {
                pcbs[now_index].state = STATE_READY;
                now_index = -1;
            }
        } else {
            // 아무 일도 없음
        }
    }

    // 4. RR에서만 "전부 0이면 리셋" 의미가 있음
    if(g_policy == POLICY_RR) {
        if (all_quantum_zero()) reset_all_quantum();
    }

    // 5. 다음 선택
    if(now_index == -1) {
        int next = pick_next_process();
        if (next != -1) {
            now_index = next;
            pcbs[now_index].state = STATE_RUNNING;
        }
    }

    // 6. 실행 시그널
    if(now_index != -1 && pcbs[now_index].state == STATE_RUNNING) {
        kill(pcbs[now_index].pid, SIGUSR1);
    }
}

// 자식

static int child_cpu_burst = 0;

static void child_sigusr1_handler(int signo) {
    (void)signo;

    if(child_cpu_burst <= 0) {
        child_cpu_burst = (rand() % 10) + 1;
    }

    child_cpu_burst--;

    if(child_cpu_burst <= 0) {
        int r = rand() % 2; // 0 종료, 1 I/O
        if(r == 0) {
            _exit(0);
        } else {
            kill(getppid(), SIGUSR2);
        }
    }
}

typedef struct {
    double avg_wait;
    int max_wait;
    int total_ticks;
} Result;

static void setup_timer(int usec_interval) {
    struct itimerval timer;
    timer.it_interval.tv_sec  = usec_interval / 1000000;
    timer.it_interval.tv_usec = usec_interval % 1000000;
    timer.it_value = timer.it_interval;
    if (setitimer(ITIMER_REAL, &timer, NULL) < 0) {
        perror("setitimer");
        exit(1);
    }
}

static void stop_timer(void) {
    struct itimerval timer = {0};
    setitimer(ITIMER_REAL, &timer, NULL);
}

static Result run_once(int quantum, sched_policy policy) {
    Result r = {0};
    base_quantum = quantum;
    g_policy = policy;

    now_index = -1;
    alive_procs = NPROC;
    tick_occurred = 0;
    total_ticks = 0;
    memset(pcbs, 0, sizeof(pcbs));

    for(int i = 0; i < NPROC; i++) {
        pid_t pid = fork();
        if(pid < 0) {
            perror("fork");
            exit(1);
        } else if(pid == 0) {
            srand((unsigned)time(NULL) ^ (unsigned)getpid());

            struct sigaction sa_child;
            memset(&sa_child, 0, sizeof(sa_child));
            sa_child.sa_handler = child_sigusr1_handler;
            sigemptyset(&sa_child.sa_mask);
            sa_child.sa_flags = SA_RESTART;
            sigaction(SIGUSR1, &sa_child, NULL);

            while (1) pause();
            _exit(0);
        } else {
            pcbs[i].pid = pid;
            pcbs[i].remaining_quantum = base_quantum;
            pcbs[i].state = STATE_READY;
            pcbs[i].io_remaining = 0;
            pcbs[i].wait_time = 0;
        }
    }

    // 타이머 1초임 이거
    setup_timer(1000000);

    while(alive_procs > 0) pause();

    stop_timer();

    int total_wait = 0;
    int finished = 0;
    int max_wait = 0;

    for(int i = 0; i < NPROC; i++) {
        if (pcbs[i].state == STATE_FIN) {
            total_wait += pcbs[i].wait_time;
            if (pcbs[i].wait_time > max_wait) max_wait = pcbs[i].wait_time;
            finished++;
        }
    }

    r.avg_wait = (finished > 0) ? ((double)total_wait / finished) : 0.0;
    r.max_wait = max_wait;
    r.total_ticks = (int)total_ticks;
    return r;
}

static const char* policy_name(sched_policy p) {
    return (p == POLICY_RR) ? "RR" : "FCFS";
}

int main(int argc, char *argv[]) {
    srand((unsigned)time(NULL));

    struct sigaction sa_alrm, sa_usr2, sa_chld;
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = sigalrm_handler;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa_alrm, NULL);

    memset(&sa_usr2, 0, sizeof(sa_usr2));
    sa_usr2.sa_sigaction = sigusr2_handler;
    sigemptyset(&sa_usr2.sa_mask);
    sa_usr2.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR2, &sa_usr2, NULL);

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    int quanta[] = {1, 2, 3, 5, 10};
    int qn = (int)(sizeof(quanta) / sizeof(quanta[0]));

    if(argc == 3) {
        sched_policy p = (strcmp(argv[1], "FCFS") == 0) ? POLICY_FCFS : POLICY_RR;
        int q = atoi(argv[2]);
        if(q <= 0) q = 3;

        Result rr = run_once(q, p);
        printf("Policy=%s, Quantum=%d -> avg_wait=%.2f, max_wait=%d, ticks=%d\n",
               policy_name(p), q, rr.avg_wait, rr.max_wait, rr.total_ticks);
        return 0;
    }

    printf("===== Scheduling Experiment (NPROC=%d) =====\n", NPROC);
    printf("Tick = 1s, CPU burst = 1~10, IO = 1~5 ticks\n\n");

    printf("%-6s %-6s %-10s %-10s %-10s\n",
           "POL", "Q", "AVG_WAIT", "MAX_WAIT", "TICKS");
    printf("------------------------------------------------------\n");

    for(int i = 0; i < 2; i++) {
        sched_policy p = (i == 0) ? POLICY_RR : POLICY_FCFS;

        for(int j = 0; j < qn; j++) {
            Result res = run_once(quanta[j], p);
            printf("%-6s %-6d %-10.2f %-10d %-10d\n"
                   policy_name(p), quanta[j], res.avg_wait, res.max_wait, res.total_ticks);
            fflush(stdout);
        }
    }

    printf("\n끝!!!!!!\n");
    return 0;
}