/*
 * kernel/workqueue.c - generic async execution with shared worker pool
 *
 * Copyright (C) 2002		Ingo Molnar
 *
 *   Derived from the taskqueue/keventd code by:
 *     David Woodhouse <dwmw2@infradead.org>
 *     Andrew Morton
 *     Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *     Theodore Ts'o <tytso@mit.edu>
 *
 * Made to use alloc_percpu by Christoph Lameter.
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This is the generic async execution mechanism.  Work items as are
 * executed in process context.  The worker pool is shared and
 * automatically managed.  There are two worker pools for each CPU (one for
 * normal work items and the other for high priority ones) and some extra
 * pools for workqueues which are not bound to any specific CPU - the
 * number of these backing pools is dynamic.
 *
 * Please read Documentation/workqueue.txt for details.
 *
 * linux内核-工作队列
 * 工作队列是一种常用的异步执行机制，工作项运行于进程上下文。工作项由
 * 工人来负责执行，这些工人叫做worker，工作项叫做work。这些worker实际是
 * 由内核线程所实现的，多个worker组合到一起成为线程池worker_pool，这些
 * 线程池被所有工作项共享，并且会自我管理。
 * 系统会在每个cpu上创建2个默认的线程池，一个是高优先级，一个是正常优先级
 * 分别负责处理高优先级事物和普通事物，这两种worker_pool是和cpu绑定的，不
 * 允许迁移，另外还有一些unbound类型的worker_pool，他们不和cpu绑定，可以在
 * 任意的cpu上运行，创建时他们的cpumask是cpu_possible_mask，这些unbound
 * 类型的线程池的数量是动态，系统会根据线程池的属性不同会创建不同的unbound
 *  worker_pool，后面会在具体代码中说明。
 *
 * 这里只是个短暂的介绍，更多详细的描述可以参看doc/workqueue.txt
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/hardirq.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/kallsyms.h>
#include <linux/debug_locks.h>
#include <linux/lockdep.h>
#include <linux/idr.h>
#include <linux/jhash.h>
#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/nodemask.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/locallock.h>
#include <linux/delay.h>

#include "workqueue_internal.h"

enum {
	/*
	 * worker_pool flags
	 *
	 * A bound pool is either associated or disassociated with its CPU.
	 * While associated (!DISASSOCIATED), all workers are bound to the
	 * CPU and none has %WORKER_UNBOUND set and concurrency management
	 * is in effect.
	 *
	 * While DISASSOCIATED, the cpu may be offline and all workers have
	 * %WORKER_UNBOUND set and concurrency management disabled, and may
	 * be executing on any CPU.  The pool behaves as an unbound one.
	 *
	 * Note that DISASSOCIATED should be flipped only while holding
	 * attach_mutex to avoid changing binding state while
	 * worker_attach_to_pool() is in progress.
	 */

	/*
	 * worker_pool的标志
	 *
	 * 一个bound型的线程池，有可能关联到它自己的cpu，也可能脱离它的cpu
	 * 由DISASSOCIATED这个标志位来决定。
	 * 当work_pool设置了(!DISASSOCIATED)标志，说明work_pool关联了cpu，
	 * pool中的所有worker都会绑定到固定的cpu上运行，并且这些worker会设置
	 * %WORKER_UNBOUND标记，同时这些worker会参与并发管理。
	 *
	 * 如果work_pool设置了DISASSOCIATED标志，说明这个cpu可能已经下线了。
	 * 所有的worker将设置%WORKER_UNBOUND标志，并发管理被禁止。worker可能
	 * 可以在任意的cpu上执行，此时这个pool的表现和unbound型的pool一致。
	 *
	 */
	POOL_MANAGER_ACTIVE	= 1 << 0,	/* being managed */
	POOL_DISASSOCIATED	= 1 << 2,	/* cpu can't serve workers */

	/* worker flags */
	WORKER_DIE		= 1 << 1,	/* die die die */
	WORKER_IDLE		= 1 << 2,	/* is idle */
	WORKER_PREP		= 1 << 3,	/* preparing to run works */
	WORKER_CPU_INTENSIVE	= 1 << 6,	/* cpu intensive */
	WORKER_UNBOUND		= 1 << 7,	/* worker is unbound */
	WORKER_REBOUND		= 1 << 8,	/* worker was rebound */

	WORKER_NOT_RUNNING	= WORKER_PREP | WORKER_CPU_INTENSIVE |
				  WORKER_UNBOUND | WORKER_REBOUND,

	NR_STD_WORKER_POOLS	= 2,		/* # standard pools per cpu */

	UNBOUND_POOL_HASH_ORDER	= 6,		/* hashed by pool->attrs */
	BUSY_WORKER_HASH_ORDER	= 6,		/* 64 pointers */

	MAX_IDLE_WORKERS_RATIO	= 4,		/* 1/4 of busy can be idle */
	IDLE_WORKER_TIMEOUT	= 300 * HZ,	/* keep idle ones for 5 mins */

	MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
						/* call for help after 10ms
						   (min two ticks) */
	MAYDAY_INTERVAL		= HZ / 10,	/* and then every 100ms */
	/* 
	 * 并发管理：创建kworker失败的情况下，再次重试创建的间隔时间
	 */
	CREATE_COOLDOWN		= HZ,		/* time to breath after fail */

	/*
	 * Rescue workers are used only on emergencies and shared by
	 * all cpus.  Give MIN_NICE.
	 */
	/*
	 * 在内存紧张的情况下，救援线程会被使用，给予MIN_NICE优先级
	 */
	RESCUER_NICE_LEVEL	= MIN_NICE,
	HIGHPRI_NICE_LEVEL	= MIN_NICE,

	WQ_NAME_LEN		= 24,
};

/*
 * Structure fields follow one of the following exclusion rules.
 *
 * I: Modifiable by initialization/destruction paths and read-only for
 *    everyone else.
 *
 * P: Preemption protected.  Disabling preemption is enough and should
 *    only be modified and accessed from the local cpu.
 *
 * L: pool->lock protected.  Access with pool->lock held.
 *
 * X: During normal operation, modification requires pool->lock and should
 *    be done only from local cpu.  Either disabling preemption on local
 *    cpu or grabbing pool->lock is enough for read access.  If
 *    POOL_DISASSOCIATED is set, it's identical to L.
 *
 *    On RT we need the extra protection via rt_lock_idle_list() for
 *    the list manipulations against read access from
 *    wq_worker_sleeping(). All other places are nicely serialized via
 *    pool->lock.
 *
 * A: pool->attach_mutex protected.
 *
 * PL: wq_pool_mutex protected.
 *
 * PR: wq_pool_mutex protected for writes.  RCU protected for reads.
 *
 * PW: wq_pool_mutex and wq->mutex protected for writes.  Either for reads.
 *
 * PWR: wq_pool_mutex and wq->mutex protected for writes.  Either or
 *      sched-RCU for reads.
 *
 * WQ: wq->mutex protected.
 *
 * WR: wq->mutex protected for writes.  RCU protected for reads.
 *
 * MD: wq_mayday_lock protected.
 */

/* struct worker is defined in workqueue_internal.h */

struct worker_pool {
	/* 自旋锁，保护worker_pool中的数据并发访问*/
	spinlock_t		lock;		/* the pool lock */
	/*
	 * 这个pool所关联的cpu，如果cpu的值为-1，则表示pool是unbound pool
	 * unbound pool不绑定任何cpu，可以漂移在任意cpu上运行。该字段仅在
	 * pool初始化、销毁函数被修改，运行过程中通常只有读操作。
	 */
	int			cpu;		/* I: the associated cpu */
	/*
	 * 这个pool所关联的节点号，for NUMA only
	 */
	int			node;		/* I: the associated node ID */
	/*
	 * pool的id号，由worker_pool_assign_id函数分配。
	 */
	int			id;		/* I: pool ID */
	/*
	 * 标志位，标识pool拥有的属性，如：
	 * POOL_MANAGER_ACTIVE	= 1 << 0, 正在被管理@@
	 * POOL_DISASSOCIATED	= 1 << 2, 脱离cpu，通常是cpu offline时会设置
	 */
	unsigned int		flags;		/* X: flags */

	/*
	 * 待处理的工作项的链表，标识了所有待处理的任务。
	 * 当用户使用schedule_work等类似接口时，最终会调用__queue_work排队
	 * 核心函数，会将work挂入该链表。
	 * __queue_work的对外常见的api有：
	 * schedule_work，schedule_work_on
	 * schedule_delayed_work，schedule_delayed_work_on
	 * queue_work
	 */
	struct list_head	worklist;	/* L: list of pending works */
	/* 当前pool中工人的数量 */
	int			nr_workers;	/* L: total number of workers */

	/* nr_idle includes the ones off idle_list for rebinding */
	/* 当前pool中空闲的工人数量，(这里的idle表示没有work可做) */
	int			nr_idle;	/* L: currently idle ones */

	/*
	 * 存放所有idle worker的链表
	 */
	struct list_head	idle_list;	/* X: list of idle workers */
	/*
	 * idle worker的定时器，因为CMWQ的worker动态管理的，如果空闲的worker
	 * 长时间没有工作要做，多余的worker会进行销毁。
	 */
	struct timer_list	idle_timer;	/* L: worker idle timeout */
	/*
	 * 救援线程的定时器，用于内存紧张的时候。
	 * 如果系统中work工作项负荷较重，这时仍有新任务要处理，同时由于内存
	 * 紧张无法创建新的worker来处理工作，这时则会将work交给救援线程处理。
	 *
	 * 问：救援线程哪来的？
	 * 答：创建workqueuue时可以一并创建救援线程，需要指定WQ_RECLAIM标志。
	 * 
	 * 通常这个救援线程基本用不上场，只是在特殊的情况下会使用。并且需要
	 * 用户自己创建带WQ_RECLAIM标志的workqueue。
	 *
	 * 系统中默认有6种工作队列，都是不带WQ_RECLAIM标志的，也就是不会创建
	 * 救援线程。
	 *
	 * 问：什么时候需要指定WQ_RECLAIM？
	 * 答：前面已经解释过了，救援线程通常是在内存紧张的时候才会用到，如果
	 * 你要处理的工作项非常重要，得不到执行会导致严重的系统问题，或者该工
	 * 作项涉及到了内存回收操作(可以帮助释放额外内存)，那么我们可认为该工
	 * 作是非常关键的任务，可以创建WQ_RECLAIM类型的工作队列，以保证在工作
	 * 项在系统内存压力较大的情况下仍可以得到执行。
	 * 
	 * 问：创建WQ_RECLAIM类型的workqueue有什么坏处？
	 * 标识了WQ_RECLAIM标志，创建工作队列时，系统会创建workqueue的时候，
	 * 一并创建一个后台救援线程，在系统内存空闲的情况下，rescuer thread
	 * 从来不会唤醒，只会浪费一个kthread的内存资源，无其他影响。
	 * 可以参见__alloc_workqueue_key接口流程。
	 */
	struct timer_list	mayday_timer;	/* L: SOS timer for workers */

	/* a workers is either on busy_hash or idle_list, or the manager */
	/*
	 * 正在执行的工作项处于busy状态，该hash表存放busy状态的工作项。
	 * 一个工作项有3种可能的状态：busy、idle、manager
	 **/
	DECLARE_HASHTABLE(busy_hash, BUSY_WORKER_HASH_ORDER);
						/* L: hash of busy workers */

	/* see manage_workers() for details on the two manager mutexes */
	/*
	 * 标识管理者worker
	 */
	struct worker		*manager;	/* L: purely informational */
	/*
	 * 互斥锁，用于attach/dettach操作
	 */
	struct mutex		attach_mutex;	/* attach/detach exclusion */
	/*
	 * pool中所有worker的链表
	 */
	struct list_head	workers;	/* A: attached workers */
	/*
	 * 完成量，用于通知all worker已经和pool解绑
	 */
	struct completion	*detach_completion; /* all workers detached */

	/*
	 * @@
	 */
	struct ida		worker_ida;	/* worker IDs for task name */

	/*
	 * 工作队列属性，主要由3个成员组成：
	 * nice     ## worker的进程优先级，nice值
	 * cpumask  ## cpu亲和性位图。
	 * no_numa  ## 用于控制NUMA亲和性
	 */
	struct workqueue_attrs	*attrs;		/* I: worker attributes */
	/*
	 * @@
	 */
	struct hlist_node	hash_node;	/* PL: unbound_pool_hash node */
	/*
	 * unbound pool的参考计数 @@
	 */
	int			refcnt;		/* PL: refcnt for unbound pools */

	/*
	 * The current concurrency level.  As it's likely to be accessed
	 * from other CPUs during try_to_wake_up(), put it in a separate
	 * cacheline.
	 */
	/*
	 * 标识pool中正在运行的worker数量。同时也表示了pool并发级别，
	 * 如：当nr_running=2时，表示当前有2个work在并发执行。
	 **/
	atomic_t		nr_running ____cacheline_aligned_in_smp;

	/*
	 * Destruction of pool is RCU protected to allow dereferences
	 * from get_work_pool().
	 */
	/*
	 *@@*/
	struct rcu_head		rcu;
} ____cacheline_aligned_in_smp;

/*
 * The per-pool workqueue.  While queued, the lower WORK_STRUCT_FLAG_BITS
 * of work_struct->data are used for flags and the remaining high bits
 * point to the pwq; thus, pwqs need to be aligned at two's power of the
 * number of flag bits.
 */
/*
 * 非常重要的结构体，用于建立workqueue和work_pool之间的关系。
 */
struct pool_workqueue {
	/*
	 * pool指针，指向对应的worker_pool
	 */
	struct worker_pool	*pool;		/* I: the associated pool */
	/*
	 * workqueue指针，反向指向对应的wq
	 */
	struct workqueue_struct *wq;		/* I: the owning workqueue */
	int			work_color;	/* L: current color */
	int			flush_color;	/* L: flushing color */
	int			refcnt;		/* L: reference count */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: nr of in_flight works */
	int			nr_active;	/* L: nr of active works */
	int			max_active;	/* L: max active works */
	struct list_head	delayed_works;	/* L: delayed works */
	/*
	 * wq->pwqs的一个链表节点，
	 */
	struct list_head	pwqs_node;	/* WR: node on wq->pwqs */
	struct list_head	mayday_node;	/* MD: node on wq->maydays */

	/*
	 * Release of unbound pwq is punted to system_wq.  See put_pwq()
	 * and pwq_unbound_release_workfn() for details.  pool_workqueue
	 * itself is also RCU protected so that the first pwq can be
	 * determined without grabbing wq->mutex.
	 */
	struct work_struct	unbound_release_work;
	struct rcu_head		rcu;
} __aligned(1 << WORK_STRUCT_FLAG_BITS);

/*
 * Structure used to wait for workqueue flush.
 */
/*
 * 用来等待一个workqueue队列上所有work项被flush
 */
struct wq_flusher {
	struct list_head	list;		/* WQ: list of flushers */
	int			flush_color;	/* WQ: flush color waiting for */
	struct completion	done;		/* flush completion */
};

struct wq_device;

/*
 * The externally visible workqueue.  It relays the issued work items to
 * the appropriate worker_pool through its pool_workqueues.
 */
/*
 * 工作队列结构体，用于存放工作任务，工作队列内部存在pool_workqueues指针用于找到对应的
 * worker_pool，CMWQ并发管理功能会根据工作队列的的pool_workqueues指针，来找到合适的
 * worker_pool，worker_pool被唤醒后，由worker_pool中的空闲线程来处理具体任务。
 */
struct workqueue_struct {
	/*
	 * 所有关联的pool_workqueues，
	 */
	struct list_head	pwqs;		/* WR: all pwqs of this wq */
	/*
	 * workqueues链表，全局变量workqueues的一个节点
	 */
	struct list_head	list;		/* PR: list of all workqueues */

	/*
	 * 互斥锁，对wq的结构体变量进行访问时，需要锁保护。
	 */
	struct mutex		mutex;		/* protects this wq */
	int			work_color;	/* WQ: current work color */
	int			flush_color;	/* WQ: current flush color */
	atomic_t		nr_pwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* WQ: first flusher */
	struct list_head	flusher_queue;	/* WQ: flush waiters */
	struct list_head	flusher_overflow; /* WQ: flush overflow list */

	struct list_head	maydays;	/* MD: pwqs requesting rescue */
	/*
	 * 如果创建wq时，指定了WQ_MEM_RECLAIM标记，则会创建一个救援线程。这里worker
	 * 代表救援线程，如果没有指定WQ_MEM_RECLAIM标记，worker为空。
	 */
	struct worker		*rescuer;	/* I: rescue worker */

	int			nr_drainers;	/* WQ: drain in progress */
	int			saved_max_active; /* WQ: saved pwq max_active */

	/*
	 * 工作队列的属性结构体，仅用于unbound工作队列
	 */
	struct workqueue_attrs	*unbound_attrs;	/* PW: only for unbound wqs */
	/*
	 * 默认的pwq指针，仅用于unbound工作队列
	 */
	struct pool_workqueue	*dfl_pwq;	/* PW: only for unbound wqs */

#ifdef CONFIG_SYSFS
	/*
	 * 如果创建wq时，指定了WQ_SYSFS标记，则会在sysfs文件系统下创建device设备
	 * workqueue_sysfs_register接口负责注册工作队列到sysfs中，注册后，用户可以
	 * 在sysfs下看到对应的工作队列属性，改变cpumask等。
	 */
	struct wq_device	*wq_dev;	/* I: for sysfs interface */
#endif
#ifdef CONFIG_LOCKDEP
	/*
	 * @@ 锁调试?
	 */
	struct lockdep_map	lockdep_map;
#endif
	/*
	 * 工作队列的名称。系统中默认的工作队列名称都以events开头。如：
	system_wq = alloc_workqueue("events", 0, 0);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND,
					    WQ_UNBOUND_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE, 0);
	system_power_efficient_wq = alloc_workqueue("events_power_efficient",
					      WQ_POWER_EFFICIENT, 0);
	system_freezable_power_efficient_wq = alloc_workqueue("events_freezable_power_efficient",
						      WQ_FREEZABLE | WQ_POWER_EFFICIENT, 0);
	 */
	char			name[WQ_NAME_LEN]; /* I: workqueue name */

	/*
	 * Destruction of workqueue_struct is sched-RCU protected to allow
	 * walking the workqueues list without grabbing wq_pool_mutex.
	 * This is used to dump all workqueues from sysrq.
	 */
	struct rcu_head		rcu;

	/* hot fields used during command issue, aligned to cacheline */
	/*
	 * 工作队列的flags，每个二进制位代表一种属性，请参看：WQ_xxx 宏定义
	 */
	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* flags */
	/*
	 * 工作队列的每CPU pool_workqueue指针，因为系统默认在每个CPU上创建了2个worker_pool。
	 * cpu_pwqs指针用于指向各CPU上的worker_pool，通过pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	 * 来找到某个cpu对应的pwq，然后通过pwq来找到对应的pool
	 */
	struct pool_workqueue __percpu *cpu_pwqs; /* I: per-cpu pwqs */
	/*
	 * @@
	 **/
	struct pool_workqueue __rcu *numa_pwq_tbl[]; /* PWR: unbound pwqs indexed by node */
};

/*
 * 这里定义了很多全局变量，用于管理、访问工作队列
 */
/*
 * 工作队列专用的SLAB缓存
 */
static struct kmem_cache *pwq_cache;

static cpumask_var_t *wq_numa_possible_cpumask;
					/* possible CPUs of each node */

static bool wq_disable_numa;
module_param_named(disable_numa, wq_disable_numa, bool, 0444);

/* see the comment above the definition of WQ_POWER_EFFICIENT */
static bool wq_power_efficient = IS_ENABLED(CONFIG_WQ_POWER_EFFICIENT_DEFAULT);
module_param_named(power_efficient, wq_power_efficient, bool, 0444);

static bool wq_numa_enabled;		/* unbound NUMA affinity enabled */

/* buf for wq_update_unbound_numa_attrs(), protected by CPU hotplug exclusion */
static struct workqueue_attrs *wq_update_unbound_numa_attrs_buf;

static DEFINE_MUTEX(wq_pool_mutex);	/* protects pools and workqueues list */
static DEFINE_SPINLOCK(wq_mayday_lock);	/* protects wq->maydays list */
static DECLARE_WAIT_QUEUE_HEAD(wq_manager_wait); /* wait for manager to go away */

/* 所有的wq都会加入到这个全局链表中，方便管理，使用each_for_wq可以遍历所有工作队列 */
static LIST_HEAD(workqueues);		/* PR: list of all workqueues */
static bool workqueue_freezing;		/* PL: have wqs started freezing? */

/* unbound队列的cpu掩码，当cpu上下线时会对其变更，默认情况下该掩码的值为possible_mask */
static cpumask_var_t wq_unbound_cpumask; /* PL: low level cpumask for all unbound wqs */

/* the per-cpu worker pools */
/* 
 * 每CPU的worker_pool结构，这里的NR_STD_WORKER_POOLS等于2，也就是每个CPU有2个pool，
 * 一个正常优先级的worker_pool，一个是高优先级的worker_pool
 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct worker_pool [NR_STD_WORKER_POOLS],
				     cpu_worker_pools);

/* 定义worker_pool的Idr结构，后面会通过idr_alloc来分配pool的id */
static DEFINE_IDR(worker_pool_idr);	/* PR: idr of all pools */

/* PL: hash of all unbound pools keyed by pool->attrs */
static DEFINE_HASHTABLE(unbound_pool_hash, UNBOUND_POOL_HASH_ORDER);

/* I: attributes used when instantiating standard unbound pools on demand */
/* unbound工作队列的属性，同样，默认情况下，系统会存在2种unboud pool，正常prio/高prio */
static struct workqueue_attrs *unbound_std_wq_attrs[NR_STD_WORKER_POOLS];

/* I: attributes used when instantiating ordered pools on demand */
/* 一个WQ若带有__WQ_ORDERED标记，代表工作队列上的work会排队执行 */
static struct workqueue_attrs *ordered_wq_attrs[NR_STD_WORKER_POOLS];

/* 
 * 系统中所有内置的工作队列，在init_workqueues中被创建。
 *
 * 1. system_wq:
 * 该队列是最常使用的工作队列，属性是普通优先级工作队列，kernel中常见api，如schedule_work
 * schedule_delay_work，schedule_work_on都是将work加入到该system_wq队列中。
 *
 * 2. system_highpri_wq:
 * 该队列和system_wq有些类似，从名称上可以看出来，该队列用于处理高优先级任务，向该队列投递
 * 任务，会叫醒高优先级的work_pool来处理工作任务。
 *
 * 3. system_unbound_wq:
 * 系统内置的一个unbound队列，对应的kworker线程是不和CPU绑定的。
 * 
 * 4. system_freezable_wq:
 *
 * 5. system_power_efficient_wq:
 * 定义为"功率高效"型的工作队列，该队列本身属于UNBOUND队列。
 *
 *
 * 6. system_freezable_power_efficient_wq:
 *
 *
 *
 *
 */
struct workqueue_struct *system_wq __read_mostly;
EXPORT_SYMBOL(system_wq);
struct workqueue_struct *system_highpri_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_highpri_wq);
struct workqueue_struct *system_long_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_long_wq);
struct workqueue_struct *system_unbound_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_unbound_wq);
struct workqueue_struct *system_freezable_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_wq);
struct workqueue_struct *system_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_power_efficient_wq);
struct workqueue_struct *system_freezable_power_efficient_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_freezable_power_efficient_wq);

static DEFINE_LOCAL_IRQ_LOCK(pendingb_lock);

static int worker_thread(void *__worker);
static void workqueue_sysfs_unregister(struct workqueue_struct *wq);

#define CREATE_TRACE_POINTS
#include <trace/events/workqueue.h>

/*
 * 下面三个宏对mutex和rcu持锁进行判断，保证数据安全访问，
 * 若没有持锁，内核给予报警提示
 */
#define assert_rcu_or_pool_mutex()					\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU or wq_pool_mutex should be held")

#define assert_rcu_or_wq_mutex(wq)					\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq->mutex),			\
			 "RCU or wq->mutex should be held")

#define assert_rcu_or_wq_mutex_or_pool_mutex(wq)			\
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() &&			\
			 !lockdep_is_held(&wq->mutex) &&		\
			 !lockdep_is_held(&wq_pool_mutex),		\
			 "RCU, wq->mutex or wq_pool_mutex should be held")

/* 
 * 遍历对应cpu上的所有work_pool 
 */
#define for_each_cpu_worker_pool(pool, cpu)				\
	for ((pool) = &per_cpu(cpu_worker_pools, cpu)[0];		\
	     (pool) < &per_cpu(cpu_worker_pools, cpu)[NR_STD_WORKER_POOLS]; \
	     (pool)++)

/**
 * for_each_pool - iterate through all worker_pools in the system
 * @pool: iteration cursor
 * @pi: integer used for iteration
 *
 * This must be called either with wq_pool_mutex held or RCU read
 * locked.  If the pool needs to be used beyond the locking in effect, the
 * caller is responsible for guaranteeing that the pool stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/* 
 * 遍历系统内所有work_pool 
 */
#define for_each_pool(pool, pi)						\
	idr_for_each_entry(&worker_pool_idr, pool, pi)			\
		if (({ assert_rcu_or_pool_mutex(); false; })) { }	\
		else

/**
 * for_each_pool_worker - iterate through all workers of a worker_pool
 * @worker: iteration cursor
 * @pool: worker_pool to iterate workers of
 *
 * This must be called with @pool->attach_mutex.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/* 
 * 遍历给定pool中所有worker 
 */
#define for_each_pool_worker(worker, pool)				\
	list_for_each_entry((worker), &(pool)->workers, node)		\
		if (({ lockdep_assert_held(&pool->attach_mutex); false; })) { } \
		else

/**
 * for_each_pwq - iterate through all pool_workqueues of the specified workqueue
 * @pwq: iteration cursor
 * @wq: the target workqueue
 *
 * This must be called either with wq->mutex held or RCU read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * The if/else clause exists only for the lockdep assertion and can be
 * ignored.
 */
/*
 * 遍历给定wq的所有pwq 
 */
#define for_each_pwq(pwq, wq)						\
	list_for_each_entry_rcu((pwq), &(wq)->pwqs, pwqs_node)		\
		if (({ assert_rcu_or_wq_mutex(wq); false; })) { }	\
		else

/*
 * 打入rt补丁后台，当在某些函数中，要添加加额外的锁保护。
 * 这里定义rt_lock_idle_list 和 sched_lock_idle_list，应用于
 * wq_worker_sleeping/worker_enter_idle/worker_leave_idle/
 * wake_up_worker函数中。
 *
 */
#ifdef CONFIG_PREEMPT_RT_BASE
static inline void rt_lock_idle_list(struct worker_pool *pool)
{
	preempt_disable();
}
static inline void rt_unlock_idle_list(struct worker_pool *pool)
{
	preempt_enable();
}
static inline void sched_lock_idle_list(struct worker_pool *pool) { }
static inline void sched_unlock_idle_list(struct worker_pool *pool) { }
#else
static inline void rt_lock_idle_list(struct worker_pool *pool) { }
static inline void rt_unlock_idle_list(struct worker_pool *pool) { }
static inline void sched_lock_idle_list(struct worker_pool *pool)
{
	spin_lock_irq(&pool->lock);
}
static inline void sched_unlock_idle_list(struct worker_pool *pool)
{
	spin_unlock_irq(&pool->lock);
}
#endif

/*
 * 用于跟踪work_struct的存活周期。如果在work_struct活动期间，
 * 其状态发生了状态变更，内核会通过debug_ojects机制对其进行检测，
 * 不正确的状态会报告。同时/sys/kernel/debug/debug_objects/stats记录了状态信息。
 *
 * 一般很少用，需要内核开启CONFIG_DEBUG_OBJECT支持。
 */
#ifdef CONFIG_DEBUG_OBJECTS_WORK

static struct debug_obj_descr work_debug_descr;

/*
 * 当检测到work_struct状态不对时，ODEBUG子系统会调用debug_hint进行错误报告，
 * 这里一般将函数地址作为返回值，内核会报告错误时，打印函数地址。
 * 如下，work_debug_hint函数返回work_struct结构的function。
 * 
 * 如下示例，对于已进入actived的work_struct又对work进行初始化，则会输出内核告警
 * debug/debug_objects # cat /proc/version
[   68.362751] ------------[ cut here ]------------
[   68.364219] WARNING: CPU: 7 PID: 269 at lib/debugobjects.c:263 debug_print_object+0x87/0xb0()
[   68.366220] ODEBUG: init active (active state 0) object type: work_struct hint: work_func1+0x0/0x50
[   68.366220] Modules linked in:
[   68.366220] CPU: 7 PID: 269 Comm: cat Not tainted 4.4.157-rt174-CGEL-V6.02.10.R2 #5
[   68.366220] Hardware name: QEMU Standard PC (i440FX + PIIX, 1996), BIOS rel-1.11.0-0-g63451fca13-prebuilt.qemu-project.org 04/01/2014
[   68.366220]  0000000000000000 ffff88003f2a7738 ffffffff813de868 ffff88003f2a7780
[   68.366220]  ffffffff82039e12 ffff88003f2a7770 ffffffff81062282 ffff880034f5c190
[   68.366220]  ffffffff82234220 ffffffff820e8acc ffff880034f5c190 ffffffff825baf40
[   68.366220] Call Trace:
[   68.366220]  [<ffffffff813de868>] dump_stack+0x59/0x81
[   68.366220]  [<ffffffff81062282>] warn_slowpath_common+0x82/0xc0
[   68.366220]  [<ffffffff8106230c>] warn_slowpath_fmt+0x4c/0x50
[   68.366220]  [<ffffffff813fa127>] debug_print_object+0x87/0xb0
[   68.366220]  [<ffffffff812475a0>] ? work_func2+0x30/0x30
[   68.366220]  [<ffffffff813fa557>] __debug_object_init+0x257/0x450
[   68.366220]  [<ffffffff812475a0>] ? work_func2+0x30/0x30
[   68.366220]  [<ffffffff813fa766>] debug_object_init+0x16/0x20
[   68.366220]  [<ffffffff81077ea9>] __init_work+0x19/0x30
[   68.366220]  [<ffffffff812476ce>] version_proc_show+0xde/0x110
[   68.366220]  [<ffffffff811f6b2a>] seq_read+0xca/0x370
[   68.366220]  [<ffffffff811b53de>] ? __kmalloc+0x2e/0x230
[   68.366220]  [<ffffffff8123da00>] ? proc_reg_write+0x70/0x70
[   68.366220]  [<ffffffff8123da42>] proc_reg_read+0x42/0x70
[   68.366220]  [<ffffffff811d31b5>] do_loop_readv_writev+0x75/0xa0
[   68.366220]  [<ffffffff8123da00>] ? proc_reg_write+0x70/0x70
[   68.366220]  [<ffffffff811d3e85>] do_readv_writev+0x1e5/0x200
[   68.366220]  [<ffffffff811d3ed6>] vfs_readv+0x36/0x50
[   68.366220]  [<ffffffff812044f3>] default_file_splice_read+0x263/0x370
[   68.366220]  [<ffffffff81202bd0>] ? page_cache_pipe_buf_release+0x20/0x20
[   68.366220]  [<ffffffff81167473>] ? __alloc_pages_nodemask+0x173/0xab0
[   68.366220]  [<ffffffff811b52fa>] ? kmem_cache_alloc_trace+0x1ba/0x1e0
[   68.366220]  [<ffffffff811f6e00>] ? seq_release+0x30/0x30
[   68.366220]  [<ffffffff811f742a>] ? seq_open+0x5a/0xa0
[   68.366220]  [<ffffffff812475f0>] ? work_func1+0x50/0x50
[   68.366220]  [<ffffffff811f74d1>] ? single_open+0x61/0xb0
[   68.366220]  [<ffffffff811b52fa>] ? kmem_cache_alloc_trace+0x1ba/0x1e0
[   68.366220]  [<ffffffff81202fb9>] do_splice_to+0x69/0x80
[   68.366220]  [<ffffffff81203079>] splice_direct_to_actor+0xa9/0x1e0
[   68.366220]  [<ffffffff81202b00>] ? generic_pipe_buf_nosteal+0x10/0x10
[   68.366220]  [<ffffffff8120323f>] do_splice_direct+0x8f/0xb0
[   68.366220]  [<ffffffff811d44d9>] do_sendfile+0x199/0x370
[   68.366220]  [<ffffffff811d4ff3>] SyS_sendfile64+0x93/0xb0
[   68.366220]  [<ffffffff810ca3d5>] ? __trace_hardirqs_off+0x15/0x20
[   68.366220]  [<ffffffff81b9a04e>] entry_SYSCALL_64_fastpath+0x22/0x92
[   68.366220] ---[ end trace 1ec556709a6e01e1 ]---
[   68.424076] work_func1 14: cpu = 7
Linux version 4.4.157-rt174-CGEL-V6.02.10.R2 (yuchen@localhost.localdomain) (gcc version 6.2.0 (ZTE Embsys-TSP V3.07.20) ) #5 SMP PREEM9
/debug/debug_objects #
/debug/debug_objects # cat stats
max_chain     :15
warnings      :1
fixups        :1
pool_free     :256
pool_min_free :255
pool_used     :1237
pool_max_used :1238
 *
 */
static void *work_debug_hint(void *addr)
{
	return ((struct work_struct *) addr)->func;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
/*
 * 一个work_struct已经激活后，又对其重新初始化，下面函数会被ODEBUG子系统调用，
 * 对work进行fixup。
 */
static int work_fixup_init(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		/* 
		 * 如果work已经是active状态，这里取消work的执行，防止work执行异常 
		 */
		cancel_work_sync(work);
		debug_object_init(work, &work_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
/*
 * 一个work_struct已经激活后，又对其进行激活，下面函数会被ODEBUG子系统调用，
 * 进行fixup。
 */
static int work_fixup_activate(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		/*
		 * This is not really a fixup. The work struct was
		 * statically initialized. We just make sure that it
		 * is tracked in the object tracker.
		 */
		/*
		 * 进入这个分支，说明work是以静态方式定义的，
		 * 静态定义的work会置位WORK_STRUCT_STATIC_BIT
		 */
		if (test_bit(WORK_STRUCT_STATIC_BIT, work_data_bits(work))) {
			debug_object_init(work, &work_debug_descr);
			debug_object_activate(work, &work_debug_descr);
			return 0;
		}
		WARN_ON_ONCE(1);
		return 0;

	case ODEBUG_STATE_ACTIVE:
		/*
		 * 重复active会输出warning
		 */
		WARN_ON(1);

	default:
		return 0;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
/*
 * 尝试对一个已激活的work_struct进行free，下面函数会被ODEBUG子系统调用，
 * 进行fixup。
 * 内核中大部分work都是全局变量，所以一般不会对其销毁。
 */
static int work_fixup_free(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		/*
		 * 先取消任务，在释放object
		 */
		cancel_work_sync(work);
		debug_object_free(work, &work_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * 为work_struct结构，定义debug_object子系统，用于object对象的生命期跟踪
 */
static struct debug_obj_descr work_debug_descr = {
	.name		= "work_struct",
	.debug_hint	= work_debug_hint,
	.fixup_init	= work_fixup_init,
	.fixup_activate	= work_fixup_activate,
	.fixup_free	= work_fixup_free,
};

/*
 * 对work项设置active状态，在workqueue的实现中，
 * 当一个工作项被添加到worklist时，会被置为active状态
 */
static inline void debug_work_activate(struct work_struct *work)
{
	debug_object_activate(work, &work_debug_descr);
}

/*
 * 对work项设置deactive状态，在workqueue的实现中，
 * 当一个工作项将要被执行时（从队列中摘下）会被置为deactive状态
 */
static inline void debug_work_deactivate(struct work_struct *work)
{
	debug_object_deactivate(work, &work_debug_descr);
}

/*
 * 当对work_struct初始化时会被调用，该函数主要目的是对debug_object对象初始化。
 * 用于work项的生命期跟踪。
 */
void __init_work(struct work_struct *work, int onstack)
{
	if (onstack)
		debug_object_init_on_stack(work, &work_debug_descr);
	else
		debug_object_init(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(__init_work);

/*
 * 当一个工作项在栈上面使用时，需要使用下面接口来销毁对象
 * 因为开启ODEBUG系统后，需要跟踪对象free的状态，并将object
 * 从ODEBUG系统中移除，所以销毁work_struct时，
 * 需要调用debug_object_free处理。
 */
void destroy_work_on_stack(struct work_struct *work)
{
	debug_object_free(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_work_on_stack);

/*
 * 和destroy_work_on_stack一样，需要销毁object对象
 */
void destroy_delayed_work_on_stack(struct delayed_work *work)
{
	destroy_timer_on_stack(&work->timer);
	debug_object_free(&work->work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_delayed_work_on_stack);

#else
/*
 * 如果不开启CONFIG_DEBUG_OBJECT_WORK，就不需要跟踪work生命周期状态。
 * debug_work_xxx相关函数为空
 */
static inline void debug_work_activate(struct work_struct *work) { }
static inline void debug_work_deactivate(struct work_struct *work) { }
#endif

/**
 * worker_pool_assign_id - allocate ID and assing it to @pool
 * @pool: the pool pointer of interest
 *
 * Returns 0 if ID in [0, WORK_OFFQ_POOL_NONE) is allocated and assigned
 * successfully, -errno on failure.
 */
/*
 * 为work_pool申请一个id，这里调用了idr子系统来申请id。
 */
static int worker_pool_assign_id(struct worker_pool *pool)
{
	int ret;

	/*
	 * 一般操作work_pool中的成员，都要加锁保护，这里为lockdep假定持锁判断
	 */
	lockdep_assert_held(&wq_pool_mutex);

	/*
	 * 通过idr子系统申请id，idr可以提高查找速度。
	 */
	ret = idr_alloc(&worker_pool_idr, pool, 0, WORK_OFFQ_POOL_NONE,
			GFP_KERNEL);
	if (ret >= 0) {
		pool->id = ret;
		return 0;
	}
	return ret;
}

/**
 * unbound_pwq_by_node - return the unbound pool_workqueue for the given node
 * @wq: the target workqueue
 * @node: the node ID
 *
 * This must be called with any of wq_pool_mutex, wq->mutex or RCU
 * read locked.
 * If the pwq needs to be used beyond the locking in effect, the caller is
 * responsible for guaranteeing that the pwq stays online.
 *
 * Return: The unbound pool_workqueue for @node.
 */
/*
 * 通过指定的workqueue_struct和node id查找pool_workqueue，
 *
 * 通常：
 * 当CONFIG_NUMA=n时，node=NUMA_NO_NODE，
 * 当CONFIG_NUMA=y，但系统不满足NUMA架构（多CPU共享一块内存），node=0
 * 当CONFIG_NUMA=y，且系统满足NUMA架构，node等于对应节点id
 */
static struct pool_workqueue *unbound_pwq_by_node(struct workqueue_struct *wq,
						  int node)
{
	assert_rcu_or_wq_mutex_or_pool_mutex(wq);

	//printk("%s %d: node=%d\n", __func__,__LINE__,node);
	/*
	 * XXX: @node can be NUMA_NO_NODE if CPU goes offline while a
	 * delayed item is pending.  The plan is to keep CPU -> NODE
	 * mapping valid and stable across CPU on/offlines.  Once that
	 * happens, this workaround can be removed.
	 */
	if (unlikely(node == NUMA_NO_NODE))
		return wq->dfl_pwq;

	return rcu_dereference_raw(wq->numa_pwq_tbl[node]);
}

static unsigned int work_color_to_flags(int color)
{
	return color << WORK_STRUCT_COLOR_SHIFT;
}

static int get_work_color(struct work_struct *work)
{
	return (*work_data_bits(work) >> WORK_STRUCT_COLOR_SHIFT) &
		((1 << WORK_STRUCT_COLOR_BITS) - 1);
}

static int work_next_color(int color)
{
	return (color + 1) % WORK_NR_COLORS;
}

/*
 * While queued, %WORK_STRUCT_PWQ is set and non flag bits of a work's data
 * contain the pointer to the queued pwq.  Once execution starts, the flag
 * is cleared and the high bits contain OFFQ flags and pool ID.
 *
 * set_work_pwq(), set_work_pool_and_clear_pending(), mark_work_canceling()
 * and clear_work_data() can be used to set the pwq, pool or clear
 * work->data.  These functions should only be called while the work is
 * owned - ie. while the PENDING bit is set.
 *
 * get_work_pool() and get_work_pwq() can be used to obtain the pool or pwq
 * corresponding to a work.  Pool is available once the work has been
 * queued anywhere after initialization until it is sync canceled.  pwq is
 * available only while the work item is queued.
 *
 * %WORK_OFFQ_CANCELING is used to mark a work item which is being
 * canceled.  While being canceled, a work item may have its PENDING set
 * but stay off timer and worklist for arbitrarily long and nobody should
 * try to steal the PENDING bit.
 */
static inline void set_work_data(struct work_struct *work, unsigned long data,
				 unsigned long flags)
{
	WARN_ON_ONCE(!work_pending(work));
	atomic_long_set(&work->data, data | flags | work_static(work));
}

static void set_work_pwq(struct work_struct *work, struct pool_workqueue *pwq,
			 unsigned long extra_flags)
{
	set_work_data(work, (unsigned long)pwq,
		      WORK_STRUCT_PENDING | WORK_STRUCT_PWQ | extra_flags);
}

static void set_work_pool_and_keep_pending(struct work_struct *work,
					   int pool_id)
{
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT,
		      WORK_STRUCT_PENDING);
}

static void set_work_pool_and_clear_pending(struct work_struct *work,
					    int pool_id)
{
	/*
	 * The following wmb is paired with the implied mb in
	 * test_and_set_bit(PENDING) and ensures all updates to @work made
	 * here are visible to and precede any updates by the next PENDING
	 * owner.
	 */
	smp_wmb();
	set_work_data(work, (unsigned long)pool_id << WORK_OFFQ_POOL_SHIFT, 0);
	/*
	 * The following mb guarantees that previous clear of a PENDING bit
	 * will not be reordered with any speculative LOADS or STORES from
	 * work->current_func, which is executed afterwards.  This possible
	 * reordering can lead to a missed execution on attempt to qeueue
	 * the same @work.  E.g. consider this case:
	 *
	 *   CPU#0                         CPU#1
	 *   ----------------------------  --------------------------------
	 *
	 * 1  STORE event_indicated
	 * 2  queue_work_on() {
	 * 3    test_and_set_bit(PENDING)
	 * 4 }                             set_..._and_clear_pending() {
	 * 5                                 set_work_data() # clear bit
	 * 6                                 smp_mb()
	 * 7                               work->current_func() {
	 * 8				      LOAD event_indicated
	 *				   }
	 *
	 * Without an explicit full barrier speculative LOAD on line 8 can
	 * be executed before CPU#0 does STORE on line 1.  If that happens,
	 * CPU#0 observes the PENDING bit is still set and new execution of
	 * a @work is not queued in a hope, that CPU#1 will eventually
	 * finish the queued @work.  Meanwhile CPU#1 does not see
	 * event_indicated is set, because speculative LOAD was executed
	 * before actual STORE.
	 */
	smp_mb();
}

static void clear_work_data(struct work_struct *work)
{
	smp_wmb();	/* see set_work_pool_and_clear_pending() */
	set_work_data(work, WORK_STRUCT_NO_POOL, 0);
}

/*
 * 函数说明：
 * 通过给定的work，返回pool_workqueue指针。
 *
 * 函数实现：
 * 当一个work被insert后，work->data会被赋值，高位会被赋值为pwq指针，
 * 低位保存work的flags。如果一个work->data已经赋值，可以通过work->data
 * 来得到pool_workqueue指针。
 */
static struct pool_workqueue *get_work_pwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return (void *)(data & WORK_STRUCT_WQ_DATA_MASK);
	else
		return NULL;
}

/**
 * get_work_pool - return the worker_pool a given work was associated with
 * @work: the work item of interest
 *
 * Pools are created and destroyed under wq_pool_mutex, and allows read
 * access under RCU read lock.  As such, this function should be
 * called under wq_pool_mutex or inside of a rcu_read_lock() region.
 *
 * All fields of the returned pool are accessible as long as the above
 * mentioned locking is in effect.  If the returned pool needs to be used
 * beyond the critical section, the caller is responsible for ensuring the
 * returned pool is and stays online.
 *
 * Return: The worker_pool @work was last associated with.  %NULL if none.
 */
/*
 * 函数说明：
 *
 * 获取一个work所关联的worker_pool。
 * 函数实现：
 *
 * 返回值：
 */
static struct worker_pool *get_work_pool(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);
	int pool_id;

	assert_rcu_or_pool_mutex();

	/*
	 * 如果work项已经被queued（还没有被执行)，work->data会置%WORK_STRUCT_PWQ位
	 * work->data中保存了pwq指针。这里直接通过pwq返回对应的pool
	 */
	if (data & WORK_STRUCT_PWQ)
		return ((struct pool_workqueue *)
			(data & WORK_STRUCT_WQ_DATA_MASK))->pool;

	/*
	 * 当work项从队里上摘下，data的高位会保存上一次处理它的pool_id，
	 * 如果work项的pool_id等于WORK_OFFQ_POOL_NONE，说明没有关联任何pool
	 * WORK_OFFQ_POOL_NONE是一个最大值，当一个work被初始化时，
	 * work->data会默认赋值WORK_OFFQ_POOL_NONE，see %WORK_DATA_INIT()
	 *
	 * 返回NULL说明一个工作项还没有被queue过，或者已经被取消。
	 */
	pool_id = data >> WORK_OFFQ_POOL_SHIFT;
	if (pool_id == WORK_OFFQ_POOL_NONE)
		return NULL;

	/*
	 * 通过idr查找对应的pool，并返回。
	 */
	return idr_find(&worker_pool_idr, pool_id);
}

/**
 * get_work_pool_id - return the worker pool ID a given work is associated with
 * @work: the work item of interest
 *
 * Return: The worker_pool ID @work was last associated with.
 * %WORK_OFFQ_POOL_NONE if none.
 */
static int get_work_pool_id(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_PWQ)
		return ((struct pool_workqueue *)
			(data & WORK_STRUCT_WQ_DATA_MASK))->pool->id;

	return data >> WORK_OFFQ_POOL_SHIFT;
}

static void mark_work_canceling(struct work_struct *work)
{
	unsigned long pool_id = get_work_pool_id(work);

	pool_id <<= WORK_OFFQ_POOL_SHIFT;
	set_work_data(work, pool_id | WORK_OFFQ_CANCELING, WORK_STRUCT_PENDING);
}

static bool work_is_canceling(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	return !(data & WORK_STRUCT_PWQ) && (data & WORK_OFFQ_CANCELING);
}

/*
 * Policy functions.  These define the policies on how the global worker
 * pools are managed.  Unless noted otherwise, these functions assume that
 * they're being called with pool->lock held.
 */

/*
 * 函数说明：
 * 判断一个work pool，是否需要更多worker。
 *
 * 此函数的策略特别简单，如果当前pool中没有worker正在运行，则返回true
 *
 * nr_running 变量标识worker真真正正的在运行，因为pool->nr_running在
 * schedule函数中被修改，如果一个work的work_func调用了阻塞函数，则nr_running
 * 会递减。详见：wq_worker_running函数
 *
 * 函数实现：
 * 如果nr_running等于0，则返回true，否则返回false。
 * 
 * explain in detail： 
 * nr_running==0 意味着什么？
 * 首先nr_running变量由schedule函数进行修改，nr_running代表了真实运行状态。
 * nr_running等于0的情况：
 * 1. 当前pool中，所有的worker处于idle状态(没有work需要处理)，那么nr_running自然为0
 * 2. 当前pool中，一些worker处于idle状态，一些worker处于busy状态（有work要处理），
 *    但这些work的work_func是阻塞型的，所以执行过程中会使worker睡眠，work_func进入
 *    睡眠同样是调用schedule函数，所以nr_running会递减。这种情况下nr_running也为0。
 * 3. 当前pool中，所有worker都处于busy，但是都在处理阻塞型任务，而导致worker睡眠。
 *    nr_running也为0
 *
 * 是否需要创建新的worker？
 * 通常__need_more_worker需要和may_start_working配合使用，来创建额外的worker。
 *
 * 并发管理：
 * 1. 如果一个带阻塞的work被加入worklist，则__need_more_worker会返回true。
 * 此时在worker_thead线程函数中，会创建新的worker来处理新的任务，从而避免
 * 新加入的work被阻塞，实现workqueue的并发。
 *
 *
 * 2. 如果一个wq是INTENSIVE类型的，则在处理该wq上的work时，会将nr_running--
 * 并将worker->flags设置成INTENSIVE，nr_running--的目的是让worker不再参与
 * 并发管理，只有当work处理完毕后，在恢复并发管理(恢复worker->flags及nr_running)
 * 如果你的work是cpu消耗型的，使用默认的schedule_work会导致后面排队的任务阻塞，
 * 所以此时你最好新创建一个INTENSIVE属性的wq，并使用queue_work()来排队任务。
 * worker_thread会根据wq的属性来进行负载均衡的管理。
 * WQ_CPU_INTENSIVE属性就是这个作用。
 */
static bool __need_more_worker(struct worker_pool *pool)
{
	return !atomic_read(&pool->nr_running);
}

/*
 * Need to wake up a worker?  Called from anything but currently
 * running workers.
 *
 * Note that, because unbound workers never contribute to nr_running, this
 * function will always return %true for unbound pools as long as the
 * worklist isn't empty.
 */
/*
 * 函数说明：
 * 是否需要更多的worker？
 *
 * 可能的使用场景：
 * 1. 调用该函数来决定是否要唤醒idle worker。(处理任务时)
 * 2. 被manager调用，是否需要为pool创建新worker。
 */
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && __need_more_worker(pool);
}

/* Can I start working?  Called from busy but !running workers. */
/*
 * 函数说明：
 * 检查一个pool中的workers，是否有worker可以工作？
 *
 * 函数实现：
 * 判断一个pool中的idle线程是否为0，如果不为0，则说明有worker可以执行，返回真。
 *
 * explain in detail:
 * nr_idle变量代表了pool中的worker确确实实处于空闲状态，需要说明的是：当一个
 * worker在处理阻塞工作而进入睡眠态时，nr_idle并不会增加。因为work还没有处理
 * 完成，worker仍处于busy状态，并不是idle状态。
 *
 * pool->nr_idle 在worker_thread线程函数中增减。
 * 当没有任务处理时调用worker_enter_idle，使nr_idle++，然后主动调用schedule睡眠
 * 当有任务处理时，worker被唤醒，调用worker_leave_idle，使nr_idle--，
 *
 * 所以，如果一个pool->nr_idle等于0，说明当前pool中没有worker可以开始工作。（all busy）
 * 该函数与need_more_worker配合使用，已确定是否需要创建更多的worker。
 */
static bool may_start_working(struct worker_pool *pool)
{
	return pool->nr_idle;
}

/* Do I need to keep working?  Called from currently running workers. */
/*
 * 函数说明：
 * 判断current worker是否需要继续保持工作？
 *
 * 函数实现：
 * 1. 首先worklist不为空，说明仍有任务pending，需要处理。
 * 2. pool中的nr_running任务不多于1个
 *
 * worklist不为空这个条件好理解，这里说一下(nr_running <=1)这个条件，为什么running<=1？
 * 如果当前nr_running>=2，则说明有2个或2个以上worker已经被唤醒，正在服务于worklist
 * 那么current worker不应该继续keep working，当前worker可以直接sleep休息了。
 * 新添加的任务很可能已经有相应的worker要处理了，所以nr_running>=2时keep_working返回false。
 *
 * 该函数由current worker线程处理完任务后调用，若不需要保持working，则调用schedule进入idle。
 * worker进入idle后，nr_running会递减，直到nr_running递减为1时，若worklist仍有任务，
 * 则让current worker需要继续保持工作，持续处理任务。
 */
static bool keep_working(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) &&
		atomic_read(&pool->nr_running) <= 1;
}

/* Do we need a new worker?  Called from manager. */
/*
 * 函数说明：
 * 判断一个work pool是否需要创建新的worker。
 *
 * 函数实现：
 * 1. worklist非空    ## 代表有工作挂入了wq，需要处理
 * 2. 当前pool->nr_running为0  ## 代表所有的worker都没有处于运行态，几种情况：
 *                                (1)worker还没有被唤醒。
 *                                (2)work已唤醒，但work—>work_func存在睡眠函数。
 * 3. 并且可以工作的worker为0  ## 表明所有worker已经都被唤醒，确实需要创建额外的worker
 * 
 * 所以，若满足以上3个条件，返回true，表示需要创建新的worker。
 * 该函数被work manager调用。
 *
 * need_more_worker() == true && pool->nr_idle == 0 意味着，所有的worker均正在
 * 处理work但是都进入了阻塞状态。所以此时必须要创建额外的worker来处理新的work。
 * 如果pool->nr_idle == 0，但need_more_worker() == false，表示所有worker处于忙
 * 状态，但个别worker并没有阻塞，nr_running>0，正在执行work的function，应该很快
 * 会处理完成，处理完成后即可处理新的任务，而如果处理的work执行时间过长导致新的
 * work无法执行，请考虑使用WQ_CPU_INTENSIVE队列。work处理时间过长意味着该任务是
 * cpu消耗型的。  
 */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/* Do we have too many workers and should some go away? */
/*
 * 函数说明：
 * 策略函数，判断一个work pool中是否存在过多的worker？
 * 
 * 函数实现：
 * 
 */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
	int nr_busy = pool->nr_workers - nr_idle;

	/*
	 * MAX_IDLE_WORKERS_RATIO = 4
	 * 空闲的worker数量*4 >= busy worker的数量，则返回真。
	 * 也就是idle达到busy的4倍时，则认为pool中的worker过多。
	 */
	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/*
 * Wake up functions.
 */

/* Return the first idle worker.  Safe with preemption disabled */
/*
 * 函数说明：
 * 返回idle_list中的第一个idle worker，通常被wake_up_worker函数使用。
 * 如果idle_list为空，则说明pool中所有worker都处于忙状态，很有可能需要创建新worker，^_^
 *
 * 对于unbound pool，销毁worker时也有调用。
 */
static struct worker *first_idle_worker(struct worker_pool *pool)
{
	if (unlikely(list_empty(&pool->idle_list)))
		return NULL;

	return list_first_entry(&pool->idle_list, struct worker, entry);
}

/**
 * wake_up_worker - wake up an idle worker
 * @pool: worker pool to wake worker from
 *
 * Wake up the first idle worker of @pool.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
/*
 * 函数说明：
 * 从pool中唤醒一个worker，worker取idle_list列表中的第一个。
 */
static void wake_up_worker(struct worker_pool *pool)
{
	struct worker *worker;

	rt_lock_idle_list(pool);

	worker = first_idle_worker(pool);

	if (likely(worker))
		wake_up_process(worker->task);

	rt_unlock_idle_list(pool);
}

/**
 * wq_worker_running - a worker is running again
 * @task: task returning from sleep
 *
 * This function is called when a worker returns from schedule()
 */
/*
 * 函数说明：
 * 标识worker任务进入running状态，该函数由schedule()函数调用。
 * 也就是在worker唤醒的时候，会被调用。 
 * 
 * 函数实现：
 * 增加worker pool中的running worker个数
 * worker->pool->nr_running++;
 */
void wq_worker_running(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);

	/* worker已经在运行，直接返回 */
	if (!worker->sleeping)
		return;
	if (!(worker->flags & WORKER_NOT_RUNNING))
		atomic_inc(&worker->pool->nr_running);
	worker->sleeping = 0;
}

/**
 * wq_worker_sleeping - a worker is going to sleep
 * @task: task going to sleep
 * This function is called from schedule() when a busy worker is
 * going to sleep.
 */
/*
 * 函数说明：
 * 标识worker任务进入sleeping状态，该函数由schedule()函数调用。
 * 也就是在worker在空闲时，执行schedule()时调用。 
 * 
 * 函数实现：
 * 递减worker pool中的running worker个数
 * worker->pool->nr_running--;
 */
void wq_worker_sleeping(struct task_struct *task)
{
	struct worker *worker = kthread_data(task);
	struct worker_pool *pool;

	/*
	 * Rescuers, which may not have all the fields set up like normal
	 * workers, also reach here, let's not access anything before
	 * checking NOT_RUNNING.
	 */
	if (worker->flags & WORKER_NOT_RUNNING)
		return;

	pool = worker->pool;

	if (WARN_ON_ONCE(worker->sleeping))
		return;

	worker->sleeping = 1;

	/*
	 * The counterpart of the following dec_and_test, implied mb,
	 * worklist not empty test sequence is in insert_work().
	 * Please read comment there.
	 */
	if (atomic_dec_and_test(&pool->nr_running) &&
	    !list_empty(&pool->worklist)) {
		sched_lock_idle_list(pool);
		wake_up_worker(pool);
		sched_unlock_idle_list(pool);
	}
}

/**
 * worker_set_flags - set worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to set
 *
 * Set @flags in @worker->flags and adjust nr_running accordingly.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock)
 */
static inline void worker_set_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;

	WARN_ON_ONCE(worker->task != current);

	/* If transitioning into NOT_RUNNING, adjust nr_running. */
	if ((flags & WORKER_NOT_RUNNING) &&
	    !(worker->flags & WORKER_NOT_RUNNING)) {
		atomic_dec(&pool->nr_running);
	}

	worker->flags |= flags;
}

/**
 * worker_clr_flags - clear worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to clear
 *
 * Clear @flags in @worker->flags and adjust nr_running accordingly.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock)
 */
static inline void worker_clr_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;
	unsigned int oflags = worker->flags;

	WARN_ON_ONCE(worker->task != current);

	worker->flags &= ~flags;

	/*
	 * If transitioning out of NOT_RUNNING, increment nr_running.  Note
	 * that the nested NOT_RUNNING is not a noop.  NOT_RUNNING is mask
	 * of multiple flags, not a single flag.
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING))
			atomic_inc(&pool->nr_running);
}

/**
 * find_worker_executing_work - find worker which is executing a work
 * @pool: pool of interest
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @pool by searching
 * @pool->busy_hash which is keyed by the address of @work.  For a worker
 * to match, its current execution should match the address of @work and
 * its work function.  This is to avoid unwanted dependency between
 * unrelated work executions through a work item being recycled while still
 * being executed.
 *
 * This is a bit tricky.  A work item may be freed once its execution
 * starts and nothing prevents the freed area from being recycled for
 * another work item.  If the same work item address ends up being reused
 * before the original execution finishes, workqueue will identify the
 * recycled work item as currently executing and make it wait until the
 * current execution finishes, introducing an unwanted dependency.
 *
 * This function checks the work item address and work function to avoid
 * false positives.  Note that this isn't complete as one may construct a
 * work function which can introduce dependency onto itself through a
 * recycled work item.  Well, if somebody wants to shoot oneself in the
 * foot that badly, there's only so much we can do, and if such deadlock
 * actually occurs, it should be easy to locate the culprit work function.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 *
 * Return:
 * Pointer to worker which is executing @work if found, %NULL
 * otherwise.
 */
/*
 * 函数说明：
 * 在指定worker_pool中，查找一个work是否正在执行。
 *
 * 返回值：
 * 若找到，返回worker指针。
 * 未找到，返回NULL。
 * */
static struct worker *find_worker_executing_work(struct worker_pool *pool,
						 struct work_struct *work)
{
	struct worker *worker;

	/* 
	 * busy任务存放在哈希表中，这里遍历哈希表，如果worker的current_work
	 * 指针和current_func指针和work匹配，则说明该工作项正在执行。
	 * worker的这2个指针，在process_one_work中赋值，work处理完毕后，
	 * current_work指针和current_func指针会被置为NULL。
	 */
	hash_for_each_possible(pool->busy_hash, worker, hentry,
			       (unsigned long)work)
		if (worker->current_work == work &&
		    worker->current_func == work->func)
			return worker;

	/* 如果没有找到正在执行的任务，返回NULL */
	return NULL;
}

/**
 * move_linked_works - move linked works to a list
 * @work: start of series of works to be scheduled
 * @head: target list to append @work to
 * @nextp: out parameter for nested worklist walking
 *
 * Schedule linked works starting from @work to @head.  Work series to
 * be scheduled starts at @work and includes any consecutive work with
 * WORK_STRUCT_LINKED set in its predecessor.
 *
 * If @nextp is not NULL, it's updated to point to the next work of
 * the last scheduled work.  This allows move_linked_works() to be
 * nested inside outer list_for_each_entry_safe().
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
static void move_linked_works(struct work_struct *work, struct list_head *head,
			      struct work_struct **nextp)
{
	struct work_struct *n;

	/*
	 * Linked worklist will always end before the end of the list,
	 * use NULL for list head.
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(*work_data_bits(work) & WORK_STRUCT_LINKED))
			break;
	}

	/*
	 * If we're already inside safe list traversal and have moved
	 * multiple works to the scheduled queue, the next position
	 * needs to be updated.
	 */
	if (nextp)
		*nextp = n;
}

/**
 * get_pwq - get an extra reference on the specified pool_workqueue
 * @pwq: pool_workqueue to get
 *
 * Obtain an extra reference on @pwq.  The caller should guarantee that
 * @pwq has positive refcnt and be holding the matching pool->lock.
 */
static void get_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	WARN_ON_ONCE(pwq->refcnt <= 0);
	pwq->refcnt++;
}

/**
 * put_pwq - put a pool_workqueue reference
 * @pwq: pool_workqueue to put
 *
 * Drop a reference of @pwq.  If its refcnt reaches zero, schedule its
 * destruction.  The caller should be holding the matching pool->lock.
 */
static void put_pwq(struct pool_workqueue *pwq)
{
	lockdep_assert_held(&pwq->pool->lock);
	if (likely(--pwq->refcnt))
		return;
	if (WARN_ON_ONCE(!(pwq->wq->flags & WQ_UNBOUND)))
		return;
	/*
	 * @pwq can't be released under pool->lock, bounce to
	 * pwq_unbound_release_workfn().  This never recurses on the same
	 * pool->lock as this path is taken only for unbound workqueues and
	 * the release work item is scheduled on a per-cpu workqueue.  To
	 * avoid lockdep warning, unbound pool->locks are given lockdep
	 * subclass of 1 in get_unbound_pool().
	 */
	schedule_work(&pwq->unbound_release_work);
}

/**
 * put_pwq_unlocked - put_pwq() with surrounding pool lock/unlock
 * @pwq: pool_workqueue to put (can be %NULL)
 *
 * put_pwq() with locking.  This function also allows %NULL @pwq.
 */
static void put_pwq_unlocked(struct pool_workqueue *pwq)
{
	if (pwq) {
		/*
		 * As both pwqs and pools are RCU protected, the
		 * following lock operations are safe.
		 */
		rcu_read_lock();
		local_spin_lock_irq(pendingb_lock, &pwq->pool->lock);
		put_pwq(pwq);
		local_spin_unlock_irq(pendingb_lock, &pwq->pool->lock);
		rcu_read_unlock();
	}
}

static void pwq_activate_delayed_work(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);

	trace_workqueue_activate_work(work);
	move_linked_works(work, &pwq->pool->worklist, NULL);
	__clear_bit(WORK_STRUCT_DELAYED_BIT, work_data_bits(work));
	pwq->nr_active++;
}

static void pwq_activate_first_delayed(struct pool_workqueue *pwq)
{
	struct work_struct *work = list_first_entry(&pwq->delayed_works,
						    struct work_struct, entry);

	pwq_activate_delayed_work(work);
}

/**
 * pwq_dec_nr_in_flight - decrement pwq's nr_in_flight
 * @pwq: pwq of interest
 * @color: color of work which left the queue
 *
 * A work either has completed or is removed from pending queue,
 * decrement nr_in_flight of its pwq and handle workqueue flushing.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
static void pwq_dec_nr_in_flight(struct pool_workqueue *pwq, int color)
{
	/* uncolored work items don't participate in flushing or nr_active */
	if (color == WORK_NO_COLOR)
		goto out_put;

	pwq->nr_in_flight[color]--;

	pwq->nr_active--;
	if (!list_empty(&pwq->delayed_works)) {
		/* one down, submit a delayed one */
		if (pwq->nr_active < pwq->max_active)
			pwq_activate_first_delayed(pwq);
	}

	/* is flush in progress and are we at the flushing tip? */
	if (likely(pwq->flush_color != color))
		goto out_put;

	/* are there still in-flight works? */
	if (pwq->nr_in_flight[color])
		goto out_put;

	/* this pwq is done, clear flush_color */
	pwq->flush_color = -1;

	/*
	 * If this was the last pwq, wake up the first flusher.  It
	 * will handle the rest.
	 */
	if (atomic_dec_and_test(&pwq->wq->nr_pwqs_to_flush))
		complete(&pwq->wq->first_flusher->done);
out_put:
	put_pwq(pwq);
}

/**
 * try_to_grab_pending - steal work item from worklist and disable irq
 * @work: work item to steal
 * @is_dwork: @work is a delayed_work
 * @flags: place to store irq state
 *
 * Try to grab PENDING bit of @work.  This function can handle @work in any
 * stable state - idle, on timer or on worklist.
 *
 * Return:
 *  1		if @work was pending and we successfully stole PENDING
 *  0		if @work was idle and we claimed PENDING
 *  -EAGAIN	if PENDING couldn't be grabbed at the moment, safe to busy-retry
 *  -ENOENT	if someone else is canceling @work, this state may persist
 *		for arbitrarily long
 *
 * Note:
 * On >= 0 return, the caller owns @work's PENDING bit.  To avoid getting
 * interrupted while holding PENDING and @work off queue, irq must be
 * disabled on entry.  This, combined with delayed_work->timer being
 * irqsafe, ensures that we return -EAGAIN for finite short period of time.
 *
 * On successful return, >= 0, irq is disabled and the caller is
 * responsible for releasing it using local_irq_restore(*@flags).
 *
 * This function is safe to call from any context including IRQ handler.
 */
/*
 * 尝试去偷取一个PENDING的work项，该接口主要用于work队列的转移，或者work的取消。
 * 通常在取消一个工作项，或者将work项转移到会被调用。
 */
static int try_to_grab_pending(struct work_struct *work, bool is_dwork,
			       unsigned long *flags)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	local_lock_irqsave(pendingb_lock, *flags);

	/* try to steal the timer if it exists */
	if (is_dwork) {
		struct delayed_work *dwork = to_delayed_work(work);

		/*
		 * dwork->timer is irqsafe.  If del_timer() fails, it's
		 * guaranteed that the timer is not queued anywhere and not
		 * running on the local CPU.
		 */
		if (likely(del_timer(&dwork->timer)))
			return 1;
	}

	/*
	 * 如果一个work没有PENDING，直接返回0，表示无法grab
	 */
	/* try to claim PENDING the normal way */
	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return 0;

	rcu_read_lock();
	/*
	 * The queueing is in progress, or it is already queued. Try to
	 * steal it from ->worklist without clearing WORK_STRUCT_PENDING.
	 */
	pool = get_work_pool(work);
	if (!pool)
		goto fail;

	spin_lock(&pool->lock);
	/*
	 * work->data is guaranteed to point to pwq only while the work
	 * item is queued on pwq->wq, and both updating work->data to point
	 * to pwq on queueing and to pool on dequeueing are done under
	 * pwq->pool->lock.  This in turn guarantees that, if work->data
	 * points to pwq which is associated with a locked pool, the work
	 * item is currently queued on that pool.
	 */
	pwq = get_work_pwq(work);
	if (pwq && pwq->pool == pool) {
		debug_work_deactivate(work);

		/*
		 * A delayed work item cannot be grabbed directly because
		 * it might have linked NO_COLOR work items which, if left
		 * on the delayed_list, will confuse pwq->nr_active
		 * management later on and cause stall.  Make sure the work
		 * item is activated before grabbing.
		 */
		if (*work_data_bits(work) & WORK_STRUCT_DELAYED)
			pwq_activate_delayed_work(work);

		list_del_init(&work->entry);
		pwq_dec_nr_in_flight(pwq, get_work_color(work));

		/* work->data points to pwq iff queued, point to pool */
		set_work_pool_and_keep_pending(work, pool->id);

		spin_unlock(&pool->lock);
		rcu_read_unlock();
		return 1;
	}
	spin_unlock(&pool->lock);
fail:
	rcu_read_unlock();
	local_unlock_irqrestore(pendingb_lock, *flags);
	if (work_is_canceling(work))
		return -ENOENT;
	cpu_chill();
	return -EAGAIN;
}

/**
 * insert_work - insert a work into a pool
 * @pwq: pwq @work belongs to
 * @work: work to insert
 * @head: insertion point
 * @extra_flags: extra WORK_STRUCT_* flags to set
 *
 * Insert @work which belongs to @pwq after @head.  @extra_flags is or'd to
 * work_struct flags.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
/*
 * 函数说明：
 * 添加一个work任务到指定list中，
 * 一般的work项会加入到的pool->worklist, barrier work会加入到worker->schedule list中。
 */
static void insert_work(struct pool_workqueue *pwq, struct work_struct *work,
			struct list_head *head, unsigned int extra_flags)
{
	struct worker_pool *pool = pwq->pool;

	/* we own @work, set data and link */
	/*
	 * 设置work的pending位，并将pwq保存到work->data中，
	 * 因为pwq的值是256bytes对齐的，所以work->data的低8位，可以做其他使用。
	 * 详见：set_work_pwq, set_work_data
	 */
	set_work_pwq(work, pwq, extra_flags);
	/* 添加work到worklist */
	list_add_tail(&work->entry, head);
	/* 增加引用计数 pwq->refcnt++ */
	get_pwq(pwq);

	/*
	 * Ensure either wq_worker_sleeping() sees the above
	 * list_add_tail() or we see zero nr_running to avoid workers lying
	 * around lazily while there are works to be processed.
	 */
	smp_mb();

	/* 
	 * 如果有必要就唤醒一个idle worker，来处理任务。
	 * 通常，当pool负载低的情况下（所有worker全在睡觉），一定会执行wake_up_worker(pool);
	 *
	 * 还有另外一种情况，就是当前pool中有活跃线程正在运行，当一个新work加入到worklist后，
	 * 则由pool中的活跃线程继续处理，则不需要额外唤醒idle worker，可以避免进程切换开销？
	 *
	 * 可以参考process_one_work流程，和__need_more_worker函数的实现。
	 *
	 * process_one_work处理单个任务的流程中，处理完一个work后会继续判断worklist是否为空。
	 * 如果worklist非空，则keep working..
	 *
	 * 这里面有个问题，如果work的work_func是非常耗时的，则可能会导致新添加的work被阻塞。
	 * 使用WQ_CPU_INTENSIVE属性的wq，可以避免这个问题。
	 * 
	 */
	if (__need_more_worker(pool))
		wake_up_worker(pool);
}

/*
 * Test whether @work is being queued from another work executing on the
 * same workqueue.
 */
static bool is_chained_work(struct workqueue_struct *wq)
{
	struct worker *worker;

	worker = current_wq_worker();
	/*
	 * Return %true iff I'm a worker execuing a work item on @wq.  If
	 * I'm @worker, it's safe to dereference it without locking.
	 */
	return worker && worker->current_pwq->wq == wq;
}

static void __queue_work(int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	struct pool_workqueue *pwq;
	struct worker_pool *last_pool;
	struct list_head *worklist;
	unsigned int work_flags;
	unsigned int req_cpu = cpu;

	/*
	 * While a work item is PENDING && off queue, a task trying to
	 * steal the PENDING will busy-loop waiting for it to either get
	 * queued or lose PENDING.  Grabbing PENDING and queueing should
	 * happen with IRQ disabled.
	 */
	WARN_ON_ONCE_NONRT(!irqs_disabled());

	debug_work_activate(work);

	/* if draining, only works from the same workqueue are allowed */
	if (unlikely(wq->flags & __WQ_DRAINING) &&
	    WARN_ON_ONCE(!is_chained_work(wq)))
		return;

	rcu_read_lock();
retry:
	/*
	 * 如果cpu == WORK_CPU_UNBOUND，说明工作不需要在指定cpu上运行。
	 * 通常被schedule_work通用接口所使用。
	 */
	if (req_cpu == WORK_CPU_UNBOUND)
		cpu = raw_smp_processor_id();

	/* pwq which will be used unless @work is executing elsewhere */
	if (!(wq->flags & WQ_UNBOUND))
		pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	else
		pwq = unbound_pwq_by_node(wq, cpu_to_node(cpu));

	/*
	 * If @work was previously on a different pool, it might still be
	 * running there, in which case the work needs to be queued on that
	 * pool to guarantee non-reentrancy.
	 */
	/*
 	 * 在queue work的时候，做检查，如果work上一次在一个不同的pool中执行，
 	 * 这次queue work仍有可能在之前的pool中执行，不过work要进行排队。
 	 *
 	 * 如何检查：
 	 * 这里先获取上一次work所关联的pool。判断pool是否和本次pool一致，如果
 	 * 不一致，那么查看该work是否正在执行..(in last_pool)，如果work正在执行,
 	 * 那么就要变更pwq了，也就是要将该work放到上一次pool中执行。
	 */
	last_pool = get_work_pool(work);
	if (last_pool && last_pool != pwq->pool) {
		struct worker *worker;

		spin_lock(&last_pool->lock);

		/*
 		 * 在指定pool中，查找work是否正在运行，返回正在执行的worker。
		 */
		worker = find_worker_executing_work(last_pool, work);

		/*
 		 * 如果找到worker，并且work是同一个队列，则赋值pwq为上一次pool
		 */
		if (worker && worker->current_pwq->wq == wq) {
			pwq = worker->current_pwq;
		} else {
			/* meh... not running there, queue here */
			/*
			 * 如果work并没有执行，不变更pwq。
			 */
			spin_unlock(&last_pool->lock);
			spin_lock(&pwq->pool->lock);
		}
	} else {
		spin_lock(&pwq->pool->lock);
	}

	/*
	 * pwq is determined and locked.  For unbound pools, we could have
	 * raced with pwq release and it could already be dead.  If its
	 * refcnt is zero, repeat pwq selection.  Note that pwqs never die
	 * without another pwq replacing it in the numa_pwq_tbl or while
	 * work items are executing on it, so the retrying is guaranteed to
	 * make forward-progress.
	 */
	/*
	 * 如果是unbound pools，可能存在竞争问题，因为unbound pools是动态变化的，
	 * pool可能已经被destory掉，destory后pwq->refcnt则为0。
	 * 而针对bound pools，系统启动后默认会在每个cpu上创建2个pool，（high pri和normal）
	 * 这2个默认的的pool不会被destory，会一直存在，refcnt初值为1
	 * 可以跟踪代码，在init_worker_pool函数中，设置了refcnt初值
	 *
	 * 这里针对ubound pools判断，如果refcnt==0，则需要重新选择pwq。
	 */
	if (unlikely(!pwq->refcnt)) {
		if (wq->flags & WQ_UNBOUND) {
			spin_unlock(&pwq->pool->lock);
			cpu_relax();
			goto retry;
		}
		/*
		 * 而针对bound pools，不存在refcnt==0的情况，如果发生，则oops。
		 * /
		/* oops */
		WARN_ONCE(true, "workqueue: per-cpu pwq for %s on cpu%d has 0 refcnt",
			  wq->name, cpu);
	}

	/* pwq determined, queue */
	trace_workqueue_queue_work(req_cpu, pwq, work);

	if (WARN_ON(!list_empty(&work->entry)))
		goto out;

	pwq->nr_in_flight[pwq->work_color]++;
	work_flags = work_color_to_flags(pwq->work_color);

	/*
	 * 增加当前被激活的work个数，pwq->nr_active++，
	 */
	if (likely(pwq->nr_active < pwq->max_active)) {
		trace_workqueue_activate_work(work);
		pwq->nr_active++;
		/* 确定要加入pool的worklist列表 */
		worklist = &pwq->pool->worklist;
	} else {
		/*
 		 * 如果当前nr_active已经到达最大值，则不能继续向worklist中添加任务。
 		 * 设置work的 WORK_STRUCT_DELAYED 标记，此时需要将work加入到delayed_works队列中
 		 */
		work_flags |= WORK_STRUCT_DELAYED;
		worklist = &pwq->delayed_works;
	}

	/*
	 * 将work添加到对应的worklist上，同时唤醒pool中的worker，去处理任务
	 * 也有可能被work pool中的其他活跃线程来处理，则不需要唤醒idle worker。
	 */
	insert_work(pwq, work, worklist, work_flags);

out:
	spin_unlock(&pwq->pool->lock);
	rcu_read_unlock();
}

/**
 * queue_work_on - queue work on specific cpu
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @work: work to queue
 *
 * We queue the work to a specific CPU, the caller must ensure it
 * can't go away.
 *
 * Return: %false if @work was already on a queue, %true otherwise.
 */
bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	bool ret = false;
	unsigned long flags;

	local_lock_irqsave(pendingb_lock,flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = true;
	}

	local_unlock_irqrestore(pendingb_lock, flags);
	return ret;
}
EXPORT_SYMBOL(queue_work_on);

void delayed_work_timer_fn(unsigned long __data)
{
	struct delayed_work *dwork = (struct delayed_work *)__data;

	/* should have been called from irqsafe timer with irq already off */
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}
EXPORT_SYMBOL(delayed_work_timer_fn);

static void __queue_delayed_work(int cpu, struct workqueue_struct *wq,
				struct delayed_work *dwork, unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	WARN_ON_ONCE(!wq);
	WARN_ON_ONCE(timer->function != delayed_work_timer_fn ||
		     timer->data != (unsigned long)dwork);
	WARN_ON_ONCE(timer_pending(timer));
	WARN_ON_ONCE(!list_empty(&work->entry));

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	if (!delay) {
		__queue_work(cpu, wq, &dwork->work);
		return;
	}

	timer_stats_timer_set_start_info(&dwork->timer);

	dwork->wq = wq;
	dwork->cpu = cpu;
	timer->expires = jiffies + delay;

	if (unlikely(cpu != WORK_CPU_UNBOUND))
		add_timer_on(timer, cpu);
	else
		add_timer(timer);
}

/**
 * queue_delayed_work_on - queue work on specific CPU after delay
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * Return: %false if @work was already on a queue, %true otherwise.  If
 * @delay is zero and @dwork is idle, it will be scheduled for immediate
 * execution.
 */
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			   struct delayed_work *dwork, unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret = false;
	unsigned long flags;

	/* read the comment in __queue_work() */
	local_lock_irqsave(pendingb_lock, flags);

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		ret = true;
	}

	local_unlock_irqrestore(pendingb_lock, flags);
	return ret;
}
EXPORT_SYMBOL(queue_delayed_work_on);

/**
 * mod_delayed_work_on - modify delay of or queue a delayed work on specific CPU
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * If @dwork is idle, equivalent to queue_delayed_work_on(); otherwise,
 * modify @dwork's timer so that it expires after @delay.  If @delay is
 * zero, @work is guaranteed to be scheduled immediately regardless of its
 * current state.
 *
 * Return: %false if @dwork was idle and queued, %true if @dwork was
 * pending and its timer was modified.
 *
 * This function is safe to call from any context including IRQ handler.
 * See try_to_grab_pending() for details.
 */
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
			 struct delayed_work *dwork, unsigned long delay)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(&dwork->work, true, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (likely(ret >= 0)) {
		__queue_delayed_work(cpu, wq, dwork, delay);
		local_unlock_irqrestore(pendingb_lock, flags);
	}

	/* -ENOENT from try_to_grab_pending() becomes %true */
	return ret;
}
EXPORT_SYMBOL_GPL(mod_delayed_work_on);

/**
 * worker_enter_idle - enter idle state
 * @worker: worker which is entering idle state
 *
 * @worker is entering idle state.  Update stats and idle timer if
 * necessary.
 *
 * LOCKING:
 * spin_lock_irq(pool->lock).
 */
static void worker_enter_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(worker->flags & WORKER_IDLE) ||
	    WARN_ON_ONCE(!list_empty(&worker->entry) &&
			 (worker->hentry.next || worker->hentry.pprev)))
		return;

	/* can't use worker_set_flags(), also called from create_worker() */
	worker->flags |= WORKER_IDLE;
	pool->nr_idle++;
	worker->last_active = jiffies;

	/* idle_list is LIFO */
	rt_lock_idle_list(pool);
	list_add(&worker->entry, &pool->idle_list);
	rt_unlock_idle_list(pool);

	if (too_many_workers(pool) && !timer_pending(&pool->idle_timer))
		mod_timer(&pool->idle_timer, jiffies + IDLE_WORKER_TIMEOUT);

	/*
	 * Sanity check nr_running.  Because wq_unbind_fn() releases
	 * pool->lock between setting %WORKER_UNBOUND and zapping
	 * nr_running, the warning may trigger spuriously.  Check iff
	 * unbind is not in progress.
	 */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     pool->nr_workers == pool->nr_idle &&
		     atomic_read(&pool->nr_running));
}

/**
 * worker_leave_idle - leave idle state
 * @worker: worker which is leaving idle state
 *
 * @worker is leaving idle state.  Update stats.
 *
 * LOCKING:
 * spin_lock_irq(pool->lock).
 */
static void worker_leave_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WARN_ON_ONCE(!(worker->flags & WORKER_IDLE)))
		return;
	worker_clr_flags(worker, WORKER_IDLE);
	pool->nr_idle--;
	rt_lock_idle_list(pool);
	list_del_init(&worker->entry);
	rt_unlock_idle_list(pool);
}

static struct worker *alloc_worker(int node)
{
	struct worker *worker;

	worker = kzalloc_node(sizeof(*worker), GFP_KERNEL, node);
	if (worker) {
		INIT_LIST_HEAD(&worker->entry);
		INIT_LIST_HEAD(&worker->scheduled);
		INIT_LIST_HEAD(&worker->node);
		/* on creation a worker is in !idle && prep state */
		worker->flags = WORKER_PREP;
	}
	return worker;
}

/**
 * worker_attach_to_pool() - attach a worker to a pool
 * @worker: worker to be attached
 * @pool: the target pool
 *
 * Attach @worker to @pool.  Once attached, the %WORKER_UNBOUND flag and
 * cpu-binding of @worker are kept coordinated with the pool across
 * cpu-[un]hotplugs.
 */
static void worker_attach_to_pool(struct worker *worker,
				   struct worker_pool *pool)
{
	mutex_lock(&pool->attach_mutex);

	/*
	 * set_cpus_allowed_ptr() will fail if the cpumask doesn't have any
	 * online CPUs.  It'll be re-applied when any of the CPUs come up.
	 */
	set_cpus_allowed_ptr(worker->task, pool->attrs->cpumask);

	/*
	 * The pool->attach_mutex ensures %POOL_DISASSOCIATED remains
	 * stable across this function.  See the comments above the
	 * flag definition for details.
	 */
	if (pool->flags & POOL_DISASSOCIATED)
		worker->flags |= WORKER_UNBOUND;

	list_add_tail(&worker->node, &pool->workers);

	mutex_unlock(&pool->attach_mutex);
}

/**
 * worker_detach_from_pool() - detach a worker from its pool
 * @worker: worker which is attached to its pool
 * @pool: the pool @worker is attached to
 *
 * Undo the attaching which had been done in worker_attach_to_pool().  The
 * caller worker shouldn't access to the pool after detached except it has
 * other reference to the pool.
 */
static void worker_detach_from_pool(struct worker *worker,
				    struct worker_pool *pool)
{
	struct completion *detach_completion = NULL;

	mutex_lock(&pool->attach_mutex);
	list_del(&worker->node);
	if (list_empty(&pool->workers))
		detach_completion = pool->detach_completion;
	mutex_unlock(&pool->attach_mutex);

	/* clear leftover flags without pool->lock after it is detached */
	worker->flags &= ~(WORKER_UNBOUND | WORKER_REBOUND);

	if (detach_completion)
		complete(detach_completion);
}

/**
 * create_worker - create a new workqueue worker
 * @pool: pool the new worker will belong to
 *
 * Create and start a new worker which is attached to @pool.
 *
 * CONTEXT:
 * Might sleep.  Does GFP_KERNEL allocations.
 *
 * Return:
 * Pointer to the newly created worker.
 */
static struct worker *create_worker(struct worker_pool *pool)
{
	struct worker *worker = NULL;
	int id = -1;
	char id_buf[16];

	/* ID is needed to determine kthread name */
	id = ida_simple_get(&pool->worker_ida, 0, 0, GFP_KERNEL);
	if (id < 0)
		goto fail;

	worker = alloc_worker(pool->node);
	if (!worker)
		goto fail;

	worker->pool = pool;
	worker->id = id;

	if (pool->cpu >= 0)
		snprintf(id_buf, sizeof(id_buf), "%d:%d%s", pool->cpu, id,
			 pool->attrs->nice < 0  ? "H" : "");
	else
		snprintf(id_buf, sizeof(id_buf), "u%d:%d", pool->id, id);

	worker->task = kthread_create_on_node(worker_thread, worker, pool->node,
					      "kworker/%s", id_buf);
	if (IS_ERR(worker->task))
		goto fail;

	set_user_nice(worker->task, pool->attrs->nice);
	kthread_bind_mask(worker->task, pool->attrs->cpumask);

	/* successful, attach the worker to the pool */
	worker_attach_to_pool(worker, pool);

	/* start the newly created worker */
	spin_lock_irq(&pool->lock);
	worker->pool->nr_workers++;
	worker_enter_idle(worker);
	wake_up_process(worker->task);
	spin_unlock_irq(&pool->lock);

	return worker;

fail:
	if (id >= 0)
		ida_simple_remove(&pool->worker_ida, id);
	kfree(worker);
	return NULL;
}

/**
 * destroy_worker - destroy a workqueue worker
 * @worker: worker to be destroyed
 *
 * Destroy @worker and adjust @pool stats accordingly.  The worker should
 * be idle.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
static void destroy_worker(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	lockdep_assert_held(&pool->lock);

	/* sanity check frenzy */
	if (WARN_ON(worker->current_work) ||
	    WARN_ON(!list_empty(&worker->scheduled)) ||
	    WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;

	pool->nr_workers--;
	pool->nr_idle--;

	rt_lock_idle_list(pool);
	list_del_init(&worker->entry);
	rt_unlock_idle_list(pool);
	worker->flags |= WORKER_DIE;
	wake_up_process(worker->task);
}

static void idle_worker_timeout(unsigned long __pool)
{
	struct worker_pool *pool = (void *)__pool;

	spin_lock_irq(&pool->lock);

	while (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		/* idle_list is kept in LIFO order, check the last one */
		/*
		 * 后进先出，取得最后加入的worker
		 */
		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		/*
		 * 计算worker满足退出条件的timerout时间
		 * expires等于上次被激活的时间+5分钟
		 */
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		/*
		 * IDLE_WORKER_TIMEOUT=300， ## 5分钟
		 * 如果worker上一次被激活的时间+IDLE_WORKER_TIMEOUT小于jiffies
		 * 说明在5分钟期间，worker被重新激活过，所以这里重新修改定时器时间。
		 * 时间设置为上一次激活的时间+5分钟：
		 * 也就是expires ##worker->last_active + IDLE_WORKER_TIMEOUT
		 */
		if (time_before(jiffies, expires)) {
			mod_timer(&pool->idle_timer, expires);
			break;
		}
		/*
		 * 如果在5分钟期限内，worker没有被唤醒，则将worker销毁。
		 */

		destroy_worker(worker);
	}

	spin_unlock_irq(&pool->lock);
}

/*
 * 向rescuer线程信号，唤醒rescuer任务。
 */
static void send_mayday(struct work_struct *work)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq_mayday_lock);

	/* 如果wq没有WQ_MEM_RECLAIM标记，则不支持rescuer线程，直接返回 */
	if (!wq->rescuer)
		return;

	/* mayday mayday mayday */
	if (list_empty(&pwq->mayday_node)) {
		/*
		 * If @pwq is for an unbound wq, its base ref may be put at
		 * any time due to an attribute change.  Pin @pwq until the
		 * rescuer is done with it.
		 */
		get_pwq(pwq);
		list_add_tail(&pwq->mayday_node, &wq->maydays);
		/* 唤醒rescuer后台线程 */
		wake_up_process(wq->rescuer->task);
	}
}

/*
 * mayday_timer定时器处理函数，用于处理create_worker超时事件
 */
static void pool_mayday_timeout(unsigned long __pool)
{
	struct worker_pool *pool = (void *)__pool;
	struct work_struct *work;

	spin_lock_irq(&pool->lock);
	spin_lock(&wq_mayday_lock);		/* for wq->maydays */

	if (need_to_create_worker(pool)) {
		/*
		 * We've been trying to create a new worker but
		 * haven't been successful.  We might be hitting an
		 * allocation deadlock.  Send distress signals to
		 * rescuers.
		 */
		/*
		 * 走到这里，说明我们尝试创建了多次worker，都没有成功。
		 * 很有可能工作任务调用在内存回收路径上，这种情况下，
		 * 可能会导致死锁。所以我们向rescuers线程发送求救信号。
		 * 将工作任务转交给rescuers线程来处理。
		 */
		list_for_each_entry(work, &pool->worklist, entry)
			send_mayday(work);
	}

	spin_unlock(&wq_mayday_lock);
	spin_unlock_irq(&pool->lock);

	/*
	 * MAYDAY_INTERVAL = HZ / 10   //and then every 100ms
	 * 更新定时器，每100ms在执行一次
	 */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INTERVAL);
}

/**
 * maybe_create_worker - create a new worker if necessary
 * @pool: pool to create a new worker for
 *
 * Create a new worker for @pool if necessary.  @pool is guaranteed to
 * have at least one idle worker on return from this function.  If
 * creating a new worker takes longer than MAYDAY_INTERVAL, mayday is
 * sent to all rescuers with works scheduled on @pool to resolve
 * possible allocation deadlock.
 *
 * On return, need_to_create_worker() is guaranteed to be %false and
 * may_start_working() %true.
 *
 * LOCKING:
 * spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.  Called only from
 * manager.
 */
static void maybe_create_worker(struct worker_pool *pool)
__releases(&pool->lock)
__acquires(&pool->lock)
{
restart:
	spin_unlock_irq(&pool->lock);

	/* if we don't make progress in MAYDAY_INITIAL_TIMEOUT, call for help */
	/*
	 * MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
	 * 启动定时器，时间为1/100秒，也就是10ms，最小为2个tick
	 */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT);

	while (true) {
		/*
		 * 如果create_worker返回空，则说明kworker没有创建成功
		 * 可能是内存不足导致，这时再判断一次是否还需要创建kworker
		 * 因为很有可能之前忙于工作的worker已经空闲，或者已经开始
		 * 处理新任务，这种情况下，就不需要再创建新kworker了，退出
		 * 循环。如果仍需要创建新worker，则继续循环，间隔一小段时间，
		 * 再次尝试创建worker。
		 */
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;

		/* 休息1s，再次尝试 */
		schedule_timeout_interruptible(CREATE_COOLDOWN);

		/*
		 * 1s后再次检查是否需要创建新worker，避免不必要开销(创建新worker) 
		 */
		if (!need_to_create_worker(pool))
			break;
	}

	del_timer_sync(&pool->mayday_timer);
	spin_lock_irq(&pool->lock);
	/*
	 * This is necessary even after a new worker was just successfully
	 * created as @pool->lock was dropped and the new worker might have
	 * already become busy.
	 */
	if (need_to_create_worker(pool))
		goto restart;
}

/**
 * manage_workers - manage worker pool
 * @worker: self
 *
 * Assume the manager role and manage the worker pool @worker belongs
 * to.  At any given time, there can be only zero or one manager per
 * pool.  The exclusion is handled automatically by this function.
 *
 * The caller can safely start processing works on false return.  On
 * true return, it's guaranteed that need_to_create_worker() is false
 * and may_start_working() is true.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.
 *
 * Return:
 * %false if the pool doesn't need management and the caller can safely
 * start processing works, %true if management function was performed and
 * the conditions that the caller verified before calling the function may
 * no longer be true.
 */
static bool manage_workers(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (pool->flags & POOL_MANAGER_ACTIVE)
		return false;

	pool->flags |= POOL_MANAGER_ACTIVE;
	pool->manager = worker;

	printk("%s %d\n", __func__,__LINE__);
	maybe_create_worker(pool);

	pool->manager = NULL;
	pool->flags &= ~POOL_MANAGER_ACTIVE;
	wake_up(&wq_manager_wait);
	return true;
}

/**
 * process_one_work - process single work
 * @worker: self
 * @work: work to process
 *
 * Process @work.  This function contains all the logics necessary to
 * process a single work including synchronization against and
 * interaction with other workers on the same cpu, queueing and
 * flushing.  As long as context requirement is met, any worker can
 * call this function to process a work.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock) which is released and regrabbed.
 */
static void process_one_work(struct worker *worker, struct work_struct *work)
__releases(&pool->lock)
__acquires(&pool->lock)
{
	struct pool_workqueue *pwq = get_work_pwq(work);
	struct worker_pool *pool = worker->pool;
	bool cpu_intensive = pwq->wq->flags & WQ_CPU_INTENSIVE;
	int work_color;
	struct worker *collision;
#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the struct work_struct from
	 * inside the function that is called from it, this we need to
	 * take into account for lockdep too.  To avoid bogus "held
	 * lock freed" warnings as well as problems when looking into
	 * work->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &work->lockdep_map);
#endif
	/* ensure we're on the correct CPU */
	/*
	 * 如果一个pool是bound类型的(per-cpu的)，则检查当前worker
	 * 是否在正确的cpu上执行，pool->cpu是pool所绑定的cpu号
	 * 如果worker没有在指定的cpu上运行，在打印内核警告。
	 */
	WARN_ON_ONCE(!(pool->flags & POOL_DISASSOCIATED) &&
		     raw_smp_processor_id() != pool->cpu);

	/*
	 * A single work shouldn't be executed concurrently by
	 * multiple workers on a single cpu.  Check whether anyone is
	 * already processing the work.  If so, defer the work to the
	 * currently executing one.
	 */
	/*
	 * 单个任务不能同时被多个worker共同处理，所以这里检查是否当前work
	 * 已经有其他worker正在处理，如果有，则将延迟到work执行完毕在处理。
	 */
	collision = find_worker_executing_work(pool, work);
	if (unlikely(collision)) {
		move_linked_works(work, &collision->scheduled, NULL);
		return;
	}

	/* claim and dequeue */
	debug_work_deactivate(work);
	hash_add(pool->busy_hash, &worker->hentry, (unsigned long)work);
	worker->current_work = work;
	worker->current_func = work->func;
	worker->current_pwq = pwq;
	work_color = get_work_color(work);

	list_del_init(&work->entry);

	/*
	 * CPU intensive works don't participate in concurrency management.
	 * They're the scheduler's responsibility.  This takes @worker out
	 * of concurrency management and the next code block will chain
	 * execution of the pending work items.
	 */
	if (unlikely(cpu_intensive))
		worker_set_flags(worker, WORKER_CPU_INTENSIVE);

	/*
	 * Wake up another worker if necessary.  The condition is always
	 * false for normal per-cpu workers since nr_running would always
	 * be >= 1 at this point.  This is used to chain execution of the
	 * pending work items for WORKER_NOT_RUNNING workers such as the
	 * UNBOUND and CPU_INTENSIVE ones.
	 */
	if (need_more_worker(pool))
		wake_up_worker(pool);

	/*
	 * Record the last pool and clear PENDING which should be the last
	 * update to @work.  Also, do this inside @pool->lock so that
	 * PENDING and queued state changes happen together while IRQ is
	 * disabled.
	 */
	set_work_pool_and_clear_pending(work, pool->id);

	spin_unlock_irq(&pool->lock);

	lock_map_acquire_read(&pwq->wq->lockdep_map);
	lock_map_acquire(&lockdep_map);
	trace_workqueue_execute_start(work);
	worker->current_func(work);
	/*
	 * While we must be careful to not use "work" after this, the trace
	 * point will only record its address.
	 */
	trace_workqueue_execute_end(work);
	lock_map_release(&lockdep_map);
	lock_map_release(&pwq->wq->lockdep_map);

	if (unlikely(in_atomic() || lockdep_depth(current) > 0)) {
		pr_err("BUG: workqueue leaked lock or atomic: %s/0x%08x/%d\n"
		       "     last function: %pf\n",
		       current->comm, preempt_count(), task_pid_nr(current),
		       worker->current_func);
		debug_show_held_locks(current);
		dump_stack();
	}

	/*
	 * The following prevents a kworker from hogging CPU on !PREEMPT
	 * kernels, where a requeueing work item waiting for something to
	 * happen could deadlock with stop_machine as such work item could
	 * indefinitely requeue itself while all other CPUs are trapped in
	 * stop_machine. At the same time, report a quiescent RCU state so
	 * the same condition doesn't freeze RCU.
	 */
	cond_resched_rcu_qs();

	spin_lock_irq(&pool->lock);

	/* clear cpu intensive status */
	if (unlikely(cpu_intensive))
		worker_clr_flags(worker, WORKER_CPU_INTENSIVE);

	/* we're done with it, release */
	hash_del(&worker->hentry);
	worker->current_work = NULL;
	worker->current_func = NULL;
	worker->current_pwq = NULL;
	worker->desc_valid = false;
	pwq_dec_nr_in_flight(pwq, work_color);
}

/**
 * process_scheduled_works - process scheduled works
 * @worker: self
 *
 * Process all scheduled works.  Please note that the scheduled list
 * may change while processing a work, so this function repeatedly
 * fetches a work from the top and executes it.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock) which may be released and regrabbed
 * multiple times.
 */
static void process_scheduled_works(struct worker *worker)
{
	while (!list_empty(&worker->scheduled)) {
		struct work_struct *work = list_first_entry(&worker->scheduled,
						struct work_struct, entry);
		process_one_work(worker, work);
	}
}

/**
 * worker_thread - the worker thread function
 * @__worker: self
 *
 * The worker thread function.  All workers belong to a worker_pool -
 * either a per-cpu one or dynamic unbound one.  These workers process all
 * work items regardless of their specific target workqueue.  The only
 * exception is work items which belong to workqueues with a rescuer which
 * will be explained in rescuer_thread().
 *
 * Return: 0
 */
static int worker_thread(void *__worker)
{
	struct worker *worker = __worker;
	struct worker_pool *pool = worker->pool;

	/* tell the scheduler that this is a workqueue worker */
	worker->task->flags |= PF_WQ_WORKER;
woke_up:
	spin_lock_irq(&pool->lock);

	/* am I supposed to die? */
	if (unlikely(worker->flags & WORKER_DIE)) {
		spin_unlock_irq(&pool->lock);
		WARN_ON_ONCE(!list_empty(&worker->entry));
		worker->task->flags &= ~PF_WQ_WORKER;

		set_task_comm(worker->task, "kworker/dying");
		ida_simple_remove(&pool->worker_ida, worker->id);
		worker_detach_from_pool(worker, pool);
		kfree(worker);
		return 0;
	}

	worker_leave_idle(worker);
recheck:
	/* no more worker necessary? */
	if (!need_more_worker(pool))
		goto sleep;

	/* do we need to manage? */
	if (unlikely(!may_start_working(pool)) && manage_workers(worker))
		goto recheck;

	/*
	 * ->scheduled list can only be filled while a worker is
	 * preparing to process a work or actually processing it.
	 * Make sure nobody diddled with it while I was sleeping.
	 */
	WARN_ON_ONCE(!list_empty(&worker->scheduled));

	/*
	 * Finish PREP stage.  We're guaranteed to have at least one idle
	 * worker or that someone else has already assumed the manager
	 * role.  This is where @worker starts participating in concurrency
	 * management if applicable and concurrency management is restored
	 * after being rebound.  See rebind_workers() for details.
	 */
	worker_clr_flags(worker, WORKER_PREP | WORKER_REBOUND);

	do {
		struct work_struct *work =
			list_first_entry(&pool->worklist,
					 struct work_struct, entry);

		if (likely(!(*work_data_bits(work) & WORK_STRUCT_LINKED))) {
			/* optimization path, not strictly necessary */
			process_one_work(worker, work);
			if (unlikely(!list_empty(&worker->scheduled)))
				process_scheduled_works(worker);
		} else {
			move_linked_works(work, &worker->scheduled, NULL);
			process_scheduled_works(worker);
		}
	} while (keep_working(pool));

	worker_set_flags(worker, WORKER_PREP);
sleep:
	/*
	 * pool->lock is held and there's no work to process and no need to
	 * manage, sleep.  Workers are woken up only while holding
	 * pool->lock or from local cpu, so setting the current state
	 * before releasing pool->lock is enough to prevent losing any
	 * event.
	 */
	worker_enter_idle(worker);
	__set_current_state(TASK_INTERRUPTIBLE);
	spin_unlock_irq(&pool->lock);
	schedule();
	goto woke_up;
}

/**
 * rescuer_thread - the rescuer thread function
 * @__rescuer: self
 *
 * Workqueue rescuer thread function.  There's one rescuer for each
 * workqueue which has WQ_MEM_RECLAIM set.
 *
 * Regular work processing on a pool may block trying to create a new
 * worker which uses GFP_KERNEL allocation which has slight chance of
 * developing into deadlock if some works currently on the same queue
 * need to be processed to satisfy the GFP_KERNEL allocation.  This is
 * the problem rescuer solves.
 *
 * When such condition is possible, the pool summons rescuers of all
 * workqueues which have works queued on the pool and let them process
 * those works so that forward progress can be guaranteed.
 *
 * This should happen rarely.
 *
 * Return: 0
 */
static int rescuer_thread(void *__rescuer)
{
	struct worker *rescuer = __rescuer;
	struct workqueue_struct *wq = rescuer->rescue_wq;
	struct list_head *scheduled = &rescuer->scheduled;
	bool should_stop;

	set_user_nice(current, RESCUER_NICE_LEVEL);

	/*
	 * Mark rescuer as worker too.  As WORKER_PREP is never cleared, it
	 * doesn't participate in concurrency management.
	 */
	rescuer->task->flags |= PF_WQ_WORKER;
repeat:
	set_current_state(TASK_INTERRUPTIBLE);

	/*
	 * By the time the rescuer is requested to stop, the workqueue
	 * shouldn't have any work pending, but @wq->maydays may still have
	 * pwq(s) queued.  This can happen by non-rescuer workers consuming
	 * all the work items before the rescuer got to them.  Go through
	 * @wq->maydays processing before acting on should_stop so that the
	 * list is always empty on exit.
	 */
	should_stop = kthread_should_stop();

	/* see whether any pwq is asking for help */
	spin_lock_irq(&wq_mayday_lock);

	while (!list_empty(&wq->maydays)) {
		struct pool_workqueue *pwq = list_first_entry(&wq->maydays,
					struct pool_workqueue, mayday_node);
		struct worker_pool *pool = pwq->pool;
		struct work_struct *work, *n;

		__set_current_state(TASK_RUNNING);
		list_del_init(&pwq->mayday_node);

		spin_unlock_irq(&wq_mayday_lock);

		worker_attach_to_pool(rescuer, pool);

		spin_lock_irq(&pool->lock);
		rescuer->pool = pool;

		/*
		 * Slurp in all works issued via this workqueue and
		 * process'em.
		 */
		WARN_ON_ONCE(!list_empty(scheduled));
		list_for_each_entry_safe(work, n, &pool->worklist, entry)
			if (get_work_pwq(work) == pwq)
				move_linked_works(work, scheduled, &n);

		if (!list_empty(scheduled)) {
			process_scheduled_works(rescuer);

			/*
			 * The above execution of rescued work items could
			 * have created more to rescue through
			 * pwq_activate_first_delayed() or chained
			 * queueing.  Let's put @pwq back on mayday list so
			 * that such back-to-back work items, which may be
			 * being used to relieve memory pressure, don't
			 * incur MAYDAY_INTERVAL delay inbetween.
			 */
			if (need_to_create_worker(pool)) {
				spin_lock(&wq_mayday_lock);
				get_pwq(pwq);
				list_move_tail(&pwq->mayday_node, &wq->maydays);
				spin_unlock(&wq_mayday_lock);
			}
		}

		/*
		 * Put the reference grabbed by send_mayday().  @pool won't
		 * go away while we're still attached to it.
		 */
		put_pwq(pwq);

		/*
		 * Leave this pool.  If need_more_worker() is %true, notify a
		 * regular worker; otherwise, we end up with 0 concurrency
		 * and stalling the execution.
		 */
		if (need_more_worker(pool))
			wake_up_worker(pool);

		rescuer->pool = NULL;
		spin_unlock_irq(&pool->lock);

		worker_detach_from_pool(rescuer, pool);

		spin_lock_irq(&wq_mayday_lock);
	}

	spin_unlock_irq(&wq_mayday_lock);

	if (should_stop) {
		__set_current_state(TASK_RUNNING);
		rescuer->task->flags &= ~PF_WQ_WORKER;
		return 0;
	}

	/* rescuers should never participate in concurrency management */
	WARN_ON_ONCE(!(rescuer->flags & WORKER_NOT_RUNNING));
	schedule();
	goto repeat;
}

/*
 * workqueue同步的结构体，工作项的flush操作会被使用。
 *
 */
struct wq_barrier {
	/*
	 * barrier工作项，该工作项会插入到被等待任务的后面。
	 * 用于等待前面的work项工作完成。
	 */
	struct work_struct	work;
	/*
	 * 当worker处理完成barrier工作项，会唤醒调用flush_work的进程
	 */
	struct completion	done;
	/*
	 * 记录那个阻塞进程（调用flush_work的那个task）
	 */
	struct task_struct	*task;	/* purely informational */
};

/*
 * barrier工作项的function，当barrier work被调度完成执行，说明被flush的work
 * 已经完成执行。这里唤醒等待线程。
 */
static void wq_barrier_func(struct work_struct *work)
{
	struct wq_barrier *barr = container_of(work, struct wq_barrier, work);
	complete(&barr->done);
}

/**
 * insert_wq_barrier - insert a barrier work
 * @pwq: pwq to insert barrier into
 * @barr: wq_barrier to insert
 * @target: target work to attach @barr to
 * @worker: worker currently executing @target, NULL if @target is not executing
 *
 * @barr is linked to @target such that @barr is completed only after
 * @target finishes execution.  Please note that the ordering
 * guarantee is observed only with respect to @target and on the local
 * cpu.
 *
 * Currently, a queued barrier can't be canceled.  This is because
 * try_to_grab_pending() can't determine whether the work to be
 * grabbed is at the head of the queue and thus can't clear LINKED
 * flag of the previous work while there must be a valid next work
 * after a work with LINKED flag set.
 *
 * Note that when @worker is non-NULL, @target may be modified
 * underneath us, so we can't reliably determine pwq from @target.
 *
 * CONTEXT:
 * spin_lock_irq(pool->lock).
 */
static void insert_wq_barrier(struct pool_workqueue *pwq,
			      struct wq_barrier *barr,
			      struct work_struct *target, struct worker *worker)
{
	struct list_head *head;
	unsigned int linked = 0;

	/*
	 * debugobject calls are safe here even with pool->lock locked
	 * as we know for sure that this will not trigger any of the
	 * checks and call back into the fixup functions where we
	 * might deadlock.
	 */
	INIT_WORK_ONSTACK(&barr->work, wq_barrier_func);
	__set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&barr->work));
	init_completion(&barr->done);
	barr->task = current;

	/*
	 * If @target is currently being executed, schedule the
	 * barrier to the worker; otherwise, put it after @target.
	 */
	/*
	 * 如果worker不为NULL，说明工作项正在此worker上运行，
	 * 将barrier插入到scheduled列表里。
	 */
	if (worker)
		head = worker->scheduled.next;
	else {
		
		/*
		 * 如果worker为NULL，说明work项还没有投入运行，将barrier直接
		 * 插入到work项之后。
		 */
		unsigned long *bits = work_data_bits(target);

		head = target->entry.next;
		/* there can already be other linked works, inherit and set */
		linked = *bits & WORK_STRUCT_LINKED;
		__set_bit(WORK_STRUCT_LINKED_BIT, bits);
	}

	/*
	 * 设置work项为active， for ODEBUG 
	 */
	debug_work_activate(&barr->work);
	/*
	 * 将work项插入到合适的list，并唤醒work_pool。
	 */
	insert_work(pwq, &barr->work, head,
		    work_color_to_flags(WORK_NO_COLOR) | linked);
}

/**
 * flush_workqueue_prep_pwqs - prepare pwqs for workqueue flushing
 * @wq: workqueue being flushed
 * @flush_color: new flush color, < 0 for no-op
 * @work_color: new work color, < 0 for no-op
 *
 * Prepare pwqs for workqueue flushing.
 *
 * If @flush_color is non-negative, flush_color on all pwqs should be
 * -1.  If no pwq has in-flight commands at the specified color, all
 * pwq->flush_color's stay at -1 and %false is returned.  If any pwq
 * has in flight commands, its pwq->flush_color is set to
 * @flush_color, @wq->nr_pwqs_to_flush is updated accordingly, pwq
 * wakeup logic is armed and %true is returned.
 *
 * The caller should have initialized @wq->first_flusher prior to
 * calling this function with non-negative @flush_color.  If
 * @flush_color is negative, no flush color update is done and %false
 * is returned.
 *
 * If @work_color is non-negative, all pwqs should have the same
 * work_color which is previous to @work_color and all will be
 * advanced to @work_color.
 *
 * CONTEXT:
 * mutex_lock(wq->mutex).
 *
 * Return:
 * %true if @flush_color >= 0 and there's something to flush.  %false
 * otherwise.
 */
static bool flush_workqueue_prep_pwqs(struct workqueue_struct *wq,
				      int flush_color, int work_color)
{
	bool wait = false;
	struct pool_workqueue *pwq;

	if (flush_color >= 0) {
		WARN_ON_ONCE(atomic_read(&wq->nr_pwqs_to_flush));
		atomic_set(&wq->nr_pwqs_to_flush, 1);
	}

	for_each_pwq(pwq, wq) {
		struct worker_pool *pool = pwq->pool;

		spin_lock_irq(&pool->lock);

		if (flush_color >= 0) {
			WARN_ON_ONCE(pwq->flush_color != -1);

			if (pwq->nr_in_flight[flush_color]) {
				pwq->flush_color = flush_color;
				atomic_inc(&wq->nr_pwqs_to_flush);
				wait = true;
			}
		}

		if (work_color >= 0) {
			WARN_ON_ONCE(work_color != work_next_color(pwq->work_color));
			pwq->work_color = work_color;
		}

		spin_unlock_irq(&pool->lock);
	}

	if (flush_color >= 0 && atomic_dec_and_test(&wq->nr_pwqs_to_flush))
		complete(&wq->first_flusher->done);

	return wait;
}

/**
 * flush_workqueue - ensure that any scheduled work has run to completion.
 * @wq: workqueue to flush
 *
 * This function sleeps until all work items which were queued on entry
 * have finished execution, but it is not livelocked by new incoming ones.
 */
/*
 * 等待一个workqueue队列上的所有work项执行完成。
 * 与drain_workqueue不同点是：drain期间不允许有new work被queue入。
 * 而flush_workqueue期间，不会阻碍new work的到来。
 */
void flush_workqueue(struct workqueue_struct *wq)
{
	struct wq_flusher this_flusher = {
		.list = LIST_HEAD_INIT(this_flusher.list),
		.flush_color = -1,
		.done = COMPLETION_INITIALIZER_ONSTACK(this_flusher.done),
	};
	int next_color;

	lock_map_acquire(&wq->lockdep_map);
	lock_map_release(&wq->lockdep_map);

	mutex_lock(&wq->mutex);

	/*
	 * Start-to-wait phase
	 */
	next_color = work_next_color(wq->work_color);

	if (next_color != wq->flush_color) {
		/*
		 * Color space is not full.  The current work_color
		 * becomes our flush_color and work_color is advanced
		 * by one.
		 */
		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow));
		this_flusher.flush_color = wq->work_color;
		wq->work_color = next_color;

		if (!wq->first_flusher) {
			/* no flush in progress, become the first flusher */
			WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

			wq->first_flusher = &this_flusher;

			if (!flush_workqueue_prep_pwqs(wq, wq->flush_color,
						       wq->work_color)) {
				/* nothing to flush, done */
				wq->flush_color = next_color;
				wq->first_flusher = NULL;
				goto out_unlock;
			}
		} else {
			/* wait in queue */
			WARN_ON_ONCE(wq->flush_color == this_flusher.flush_color);
			list_add_tail(&this_flusher.list, &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}
	} else {
		/*
		 * Oops, color space is full, wait on overflow queue.
		 * The next flush completion will assign us
		 * flush_color and transfer to flusher_queue.
		 */
		list_add_tail(&this_flusher.list, &wq->flusher_overflow);
	}

	mutex_unlock(&wq->mutex);

	wait_for_completion(&this_flusher.done);

	/*
	 * Wake-up-and-cascade phase
	 *
	 * First flushers are responsible for cascading flushes and
	 * handling overflow.  Non-first flushers can simply return.
	 */
	if (wq->first_flusher != &this_flusher)
		return;

	mutex_lock(&wq->mutex);

	/* we might have raced, check again with mutex held */
	if (wq->first_flusher != &this_flusher)
		goto out_unlock;

	wq->first_flusher = NULL;

	WARN_ON_ONCE(!list_empty(&this_flusher.list));
	WARN_ON_ONCE(wq->flush_color != this_flusher.flush_color);

	while (true) {
		struct wq_flusher *next, *tmp;

		/* complete all the flushers sharing the current flush color */
		list_for_each_entry_safe(next, tmp, &wq->flusher_queue, list) {
			if (next->flush_color != wq->flush_color)
				break;
			list_del_init(&next->list);
			complete(&next->done);
		}

		WARN_ON_ONCE(!list_empty(&wq->flusher_overflow) &&
			     wq->flush_color != work_next_color(wq->work_color));

		/* this flush_color is finished, advance by one */
		wq->flush_color = work_next_color(wq->flush_color);

		/* one color has been freed, handle overflow queue */
		if (!list_empty(&wq->flusher_overflow)) {
			/*
			 * Assign the same color to all overflowed
			 * flushers, advance work_color and append to
			 * flusher_queue.  This is the start-to-wait
			 * phase for these overflowed flushers.
			 */
			list_for_each_entry(tmp, &wq->flusher_overflow, list)
				tmp->flush_color = wq->work_color;

			wq->work_color = work_next_color(wq->work_color);

			list_splice_tail_init(&wq->flusher_overflow,
					      &wq->flusher_queue);
			flush_workqueue_prep_pwqs(wq, -1, wq->work_color);
		}

		if (list_empty(&wq->flusher_queue)) {
			WARN_ON_ONCE(wq->flush_color != wq->work_color);
			break;
		}

		/*
		 * Need to flush more colors.  Make the next flusher
		 * the new first flusher and arm pwqs.
		 */
		WARN_ON_ONCE(wq->flush_color == wq->work_color);
		WARN_ON_ONCE(wq->flush_color != next->flush_color);

		list_del_init(&next->list);
		wq->first_flusher = next;

		if (flush_workqueue_prep_pwqs(wq, wq->flush_color, -1))
			break;

		/*
		 * Meh... this color is already done, clear first
		 * flusher and repeat cascading.
		 */
		wq->first_flusher = NULL;
	}

out_unlock:
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL(flush_workqueue);

/**
 * drain_workqueue - drain a workqueue
 * @wq: workqueue to drain
 *
 * Wait until the workqueue becomes empty.  While draining is in progress,
 * only chain queueing is allowed.  IOW, only currently pending or running
 * work items on @wq can queue further work items on it.  @wq is flushed
 * repeatedly until it becomes empty.  The number of flushing is determined
 * by the depth of chaining and should be relatively short.  Whine if it
 * takes too long.
 */
/*
 * 等待一个workqueue队列上的所有work项全部处理完成，
 * 在等待过程中，该接口会阻塞，并对wq设置__WQ_DRAINING标记，
 * 防止在drain期间，仍有人向wq队列投递任务。(仅chailed work是被允许的)
 */
void drain_workqueue(struct workqueue_struct *wq)
{
	unsigned int flush_cnt = 0;
	struct pool_workqueue *pwq;

	/*
	 * __queue_work() needs to test whether there are drainers, is much
	 * hotter than drain_workqueue() and already looks at @wq->flags.
	 * Use __WQ_DRAINING so that queue doesn't have to check nr_drainers.
	 */
	mutex_lock(&wq->mutex);
	if (!wq->nr_drainers++)
		wq->flags |= __WQ_DRAINING;
	mutex_unlock(&wq->mutex);
reflush:
	flush_workqueue(wq);

	mutex_lock(&wq->mutex);

	for_each_pwq(pwq, wq) {
		bool drained;

		spin_lock_irq(&pwq->pool->lock);
		drained = !pwq->nr_active && list_empty(&pwq->delayed_works);
		spin_unlock_irq(&pwq->pool->lock);

		/*
		 * 如果pwq上的work项全部执行完成，则遍历下一个pwq
		 */
		if (drained)
			continue;

		if (++flush_cnt == 10 ||
		    (flush_cnt % 100 == 0 && flush_cnt <= 1000))
			pr_warn("workqueue %s: drain_workqueue() isn't complete after %u tries\n",
				wq->name, flush_cnt);

		mutex_unlock(&wq->mutex);
		goto reflush;
	}

	if (!--wq->nr_drainers)
		wq->flags &= ~__WQ_DRAINING;
	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(drain_workqueue);

/*
 * flush work的准备工作，主要功能为：将barrier work插入到被等待work之后。
 */
static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr)
{
	struct worker *worker = NULL;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	might_sleep();

	rcu_read_lock();
	/*
	 * 获取上一次work执行的pool
	 */
	pool = get_work_pool(work);
	if (!pool) {
		/*
		 * NULL pool 说明work没有被queued，或者已取消。
		 */
		rcu_read_unlock();
		return false;
	}

	spin_lock_irq(&pool->lock);
	/* see the comment in try_to_grab_pending() with the same code */
	/*
	 * 尝试获取pwq指针，如果pwq不为NULL，说明work已经被insert，但没有得到调度
	 * 得到调度后work->data会存储poo_id，覆盖了data的高bit位，pwq变为NULL
	 */
	pwq = get_work_pwq(work);
	if (pwq) {
		if (unlikely(pwq->pool != pool))
			goto already_gone;
	} else {
		/*
		 * 查看work项是否正在running，如果没有运行，那么就无法跟踪了。
		 */
		worker = find_worker_executing_work(pool, work);
		if (!worker)
			goto already_gone;
		pwq = worker->current_pwq;
	}

	insert_wq_barrier(pwq, barr, work, worker);
	spin_unlock_irq(&pool->lock);

	/*
	 * If @max_active is 1 or rescuer is in use, flushing another work
	 * item on the same workqueue may lead to deadlock.  Make sure the
	 * flusher is not running on the same workqueue by verifying write
	 * access.
	 */
	if (pwq->wq->saved_max_active == 1 || pwq->wq->rescuer)
		lock_map_acquire(&pwq->wq->lockdep_map);
	else
		lock_map_acquire_read(&pwq->wq->lockdep_map);
	lock_map_release(&pwq->wq->lockdep_map);
	rcu_read_unlock();
	return true;
already_gone:
	spin_unlock_irq(&pool->lock);
	rcu_read_unlock();
	return false;
}

/**
 * flush_work - wait for a work to finish executing the last queueing instance
 * @work: the work to flush
 *
 * Wait until @work has finished execution.  @work is guaranteed to be idle
 * on return if it hasn't been requeued since flush started.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
/*
 * 用于等待一个工作项执行完成。
 *
 * 1、如果工作项已经执行完毕，接口立即返回false。
 * 2、如果工作项尚未得到执行，该接口会阻塞，待工作项执行完毕后，返回true。
 *
 */
bool flush_work(struct work_struct *work)
{
	struct wq_barrier barr;

	lock_map_acquire(&work->lockdep_map);
	lock_map_release(&work->lockdep_map);

	/*
	 * wq_barrier结构用于进程同步，使用completion机制
	 */
	if (start_flush_work(work, &barr)) {
		/* 等待kworker唤醒当前进程 */
		wait_for_completion(&barr.done);
		destroy_work_on_stack(&barr.work);
		return true;
	} else {
		return false;
	}
}
EXPORT_SYMBOL_GPL(flush_work);

struct cwt_wait {
	wait_queue_t		wait;
	struct work_struct	*work;
};

static int cwt_wakefn(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	struct cwt_wait *cwait = container_of(wait, struct cwt_wait, wait);

	if (cwait->work != key)
		return 0;
	return autoremove_wake_function(wait, mode, sync, key);
}

static bool __cancel_work_timer(struct work_struct *work, bool is_dwork)
{
	static DECLARE_WAIT_QUEUE_HEAD(cancel_waitq);
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(work, is_dwork, &flags);
		/*
		 * If someone else is already canceling, wait for it to
		 * finish.  flush_work() doesn't work for PREEMPT_NONE
		 * because we may get scheduled between @work's completion
		 * and the other canceling task resuming and clearing
		 * CANCELING - flush_work() will return false immediately
		 * as @work is no longer busy, try_to_grab_pending() will
		 * return -ENOENT as @work is still being canceled and the
		 * other canceling task won't be able to clear CANCELING as
		 * we're hogging the CPU.
		 *
		 * Let's wait for completion using a waitqueue.  As this
		 * may lead to the thundering herd problem, use a custom
		 * wake function which matches @work along with exclusive
		 * wait and wakeup.
		 */
		if (unlikely(ret == -ENOENT)) {
			struct cwt_wait cwait;

			init_wait(&cwait.wait);
			cwait.wait.func = cwt_wakefn;
			cwait.work = work;

			prepare_to_wait_exclusive(&cancel_waitq, &cwait.wait,
						  TASK_UNINTERRUPTIBLE);
			if (work_is_canceling(work))
				schedule();
			finish_wait(&cancel_waitq, &cwait.wait);
		}
	} while (unlikely(ret < 0));

	/* tell other tasks trying to grab @work to back off */
	mark_work_canceling(work);
	local_unlock_irqrestore(pendingb_lock, flags);

	flush_work(work);
	clear_work_data(work);

	/*
	 * Paired with prepare_to_wait() above so that either
	 * waitqueue_active() is visible here or !work_is_canceling() is
	 * visible there.
	 */
	smp_mb();
	if (waitqueue_active(&cancel_waitq))
		__wake_up(&cancel_waitq, TASK_NORMAL, 1, work);

	return ret;
}

/**
 * cancel_work_sync - cancel a work and wait for it to finish
 * @work: the work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself or migrates to
 * another workqueue.  On return from this function, @work is
 * guaranteed to be not pending or executing on any CPU.
 *
 * cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's.  Use cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the workqueue on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return:
 * %true if @work was pending, %false otherwise.
 */
bool cancel_work_sync(struct work_struct *work)
{
	return __cancel_work_timer(work, false);
}
EXPORT_SYMBOL_GPL(cancel_work_sync);

/**
 * flush_delayed_work - wait for a dwork to finish executing the last queueing
 * @dwork: the delayed work to flush
 *
 * Delayed timer is cancelled and the pending work is queued for
 * immediate execution.  Like flush_work(), this function only
 * considers the last queueing instance of @dwork.
 *
 * Return:
 * %true if flush_work() waited for the work to finish execution,
 * %false if it was already idle.
 */
bool flush_delayed_work(struct delayed_work *dwork)
{
	local_lock_irq(pendingb_lock);
	if (del_timer_sync(&dwork->timer))
		__queue_work(dwork->cpu, dwork->wq, &dwork->work);
	local_unlock_irq(pendingb_lock);
	return flush_work(&dwork->work);
}
EXPORT_SYMBOL(flush_delayed_work);

/**
 * cancel_delayed_work - cancel a delayed work
 * @dwork: delayed_work to cancel
 *
 * Kill off a pending delayed_work.
 *
 * Return: %true if @dwork was pending and canceled; %false if it wasn't
 * pending.
 *
 * Note:
 * The work callback function may still be running on return, unless
 * it returns %true and the work doesn't re-arm itself.  Explicitly flush or
 * use cancel_delayed_work_sync() to wait on it.
 *
 * This function is safe to call from any context including IRQ handler.
 */
bool cancel_delayed_work(struct delayed_work *dwork)
{
	unsigned long flags;
	int ret;

	do {
		ret = try_to_grab_pending(&dwork->work, true, &flags);
	} while (unlikely(ret == -EAGAIN));

	if (unlikely(ret < 0))
		return false;

	set_work_pool_and_clear_pending(&dwork->work,
					get_work_pool_id(&dwork->work));
	local_unlock_irqrestore(pendingb_lock, flags);
	return ret;
}
EXPORT_SYMBOL(cancel_delayed_work);

/**
 * cancel_delayed_work_sync - cancel a delayed work and wait for it to finish
 * @dwork: the delayed work cancel
 *
 * This is cancel_work_sync() for delayed works.
 *
 * Return:
 * %true if @dwork was pending, %false otherwise.
 */
bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_timer(&dwork->work, true);
}
EXPORT_SYMBOL(cancel_delayed_work_sync);

/**
 * schedule_on_each_cpu - execute a function synchronously on each online CPU
 * @func: the function to call
 *
 * schedule_on_each_cpu() executes @func on each online CPU using the
 * system workqueue and blocks until all CPUs have completed.
 * schedule_on_each_cpu() is very slow.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int schedule_on_each_cpu(work_func_t func)
{
	int cpu;
	struct work_struct __percpu *works;

	works = alloc_percpu(struct work_struct);
	if (!works)
		return -ENOMEM;

	get_online_cpus();

	for_each_online_cpu(cpu) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, func);
		schedule_work_on(cpu, work);
	}

	for_each_online_cpu(cpu)
		flush_work(per_cpu_ptr(works, cpu));

	put_online_cpus();
	free_percpu(works);
	return 0;
}

/**
 * execute_in_process_context - reliably execute the routine with user context
 * @fn:		the function to execute
 * @ew:		guaranteed storage for the execute work structure (must
 *		be available when the work executes)
 *
 * Executes the function immediately if process context is available,
 * otherwise schedules the function for delayed execution.
 *
 * Return:	0 - function was executed
 *		1 - function was scheduled for execution
 */
int execute_in_process_context(work_func_t fn, struct execute_work *ew)
{
	if (!in_interrupt()) {
		fn(&ew->work);
		return 0;
	}

	INIT_WORK(&ew->work, fn);
	schedule_work(&ew->work);

	return 1;
}
EXPORT_SYMBOL_GPL(execute_in_process_context);

/**
 * free_workqueue_attrs - free a workqueue_attrs
 * @attrs: workqueue_attrs to free
 *
 * Undo alloc_workqueue_attrs().
 */
void free_workqueue_attrs(struct workqueue_attrs *attrs)
{
	if (attrs) {
		free_cpumask_var(attrs->cpumask);
		kfree(attrs);
	}
}

/**
 * alloc_workqueue_attrs - allocate a workqueue_attrs
 * @gfp_mask: allocation mask to use
 *
 * Allocate a new workqueue_attrs, initialize with default settings and
 * return it.
 *
 * Return: The allocated new workqueue_attr on success. %NULL on failure.
 */
struct workqueue_attrs *alloc_workqueue_attrs(gfp_t gfp_mask)
{
	struct workqueue_attrs *attrs;

	attrs = kzalloc(sizeof(*attrs), gfp_mask);
	if (!attrs)
		goto fail;
	if (!alloc_cpumask_var(&attrs->cpumask, gfp_mask))
		goto fail;

	cpumask_copy(attrs->cpumask, cpu_possible_mask);
	return attrs;
fail:
	free_workqueue_attrs(attrs);
	return NULL;
}

static void copy_workqueue_attrs(struct workqueue_attrs *to,
				 const struct workqueue_attrs *from)
{
	to->nice = from->nice;
	cpumask_copy(to->cpumask, from->cpumask);
	/*
	 * Unlike hash and equality test, this function doesn't ignore
	 * ->no_numa as it is used for both pool and wq attrs.  Instead,
	 * get_unbound_pool() explicitly clears ->no_numa after copying.
	 */
	to->no_numa = from->no_numa;
}

/* hash value of the content of @attr */
static u32 wqattrs_hash(const struct workqueue_attrs *attrs)
{
	u32 hash = 0;

	hash = jhash_1word(attrs->nice, hash);
	hash = jhash(cpumask_bits(attrs->cpumask),
		     BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long), hash);
	return hash;
}

/* content equality test */
static bool wqattrs_equal(const struct workqueue_attrs *a,
			  const struct workqueue_attrs *b)
{
	if (a->nice != b->nice)
		return false;
	if (!cpumask_equal(a->cpumask, b->cpumask))
		return false;
	return true;
}

/**
 * init_worker_pool - initialize a newly zalloc'd worker_pool
 * @pool: worker_pool to initialize
 *
 * Initialize a newly zalloc'd @pool.  It also allocates @pool->attrs.
 *
 * Return: 0 on success, -errno on failure.  Even on failure, all fields
 * inside @pool proper are initialized and put_unbound_pool() can be called
 * on @pool safely to release it.
 */
static int init_worker_pool(struct worker_pool *pool)
{
	spin_lock_init(&pool->lock);
	pool->id = -1;
	pool->cpu = -1;
	pool->node = NUMA_NO_NODE;
	pool->flags |= POOL_DISASSOCIATED;
	INIT_LIST_HEAD(&pool->worklist);
	INIT_LIST_HEAD(&pool->idle_list);
	hash_init(pool->busy_hash);

	init_timer_deferrable(&pool->idle_timer);
	pool->idle_timer.function = idle_worker_timeout;
	pool->idle_timer.data = (unsigned long)pool;

	setup_timer(&pool->mayday_timer, pool_mayday_timeout,
		    (unsigned long)pool);

	mutex_init(&pool->attach_mutex);
	INIT_LIST_HEAD(&pool->workers);

	ida_init(&pool->worker_ida);
	INIT_HLIST_NODE(&pool->hash_node);
	pool->refcnt = 1;

	/* shouldn't fail above this point */
	pool->attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if (!pool->attrs)
		return -ENOMEM;
	return 0;
}

static void rcu_free_wq(struct rcu_head *rcu)
{
	struct workqueue_struct *wq =
		container_of(rcu, struct workqueue_struct, rcu);

	if (!(wq->flags & WQ_UNBOUND))
		free_percpu(wq->cpu_pwqs);
	else
		free_workqueue_attrs(wq->unbound_attrs);

	kfree(wq->rescuer);
	kfree(wq);
}

static void rcu_free_pool(struct rcu_head *rcu)
{
	struct worker_pool *pool = container_of(rcu, struct worker_pool, rcu);

	ida_destroy(&pool->worker_ida);
	free_workqueue_attrs(pool->attrs);
	kfree(pool);
}

/**
 * put_unbound_pool - put a worker_pool
 * @pool: worker_pool to put
 *
 * Put @pool.  If its refcnt reaches zero, it gets destroyed in RCU
 * safe manner.  get_unbound_pool() calls this function on its failure path
 * and this function should be able to release pools which went through,
 * successfully or not, init_worker_pool().
 *
 * Should be called with wq_pool_mutex held.
 */
static void put_unbound_pool(struct worker_pool *pool)
{
	DECLARE_COMPLETION_ONSTACK(detach_completion);
	struct worker *worker;

	lockdep_assert_held(&wq_pool_mutex);

	if (--pool->refcnt)
		return;

	/* sanity checks */
	if (WARN_ON(!(pool->cpu < 0)) ||
	    WARN_ON(!list_empty(&pool->worklist)))
		return;

	/* release id and unhash */
	if (pool->id >= 0)
		idr_remove(&worker_pool_idr, pool->id);
	hash_del(&pool->hash_node);

	/*
	 * Become the manager and destroy all workers.  This prevents
	 * @pool's workers from blocking on attach_mutex.  We're the last
	 * manager and @pool gets freed with the flag set.
	 */
	spin_lock_irq(&pool->lock);
	wait_event_lock_irq(wq_manager_wait,
			    !(pool->flags & POOL_MANAGER_ACTIVE), pool->lock);
	pool->flags |= POOL_MANAGER_ACTIVE;

	while ((worker = first_idle_worker(pool)))
		destroy_worker(worker);
	WARN_ON(pool->nr_workers || pool->nr_idle);
	spin_unlock_irq(&pool->lock);

	mutex_lock(&pool->attach_mutex);
	if (!list_empty(&pool->workers))
		pool->detach_completion = &detach_completion;
	mutex_unlock(&pool->attach_mutex);

	if (pool->detach_completion)
		wait_for_completion(pool->detach_completion);

	/* shut down the timers */
	del_timer_sync(&pool->idle_timer);
	del_timer_sync(&pool->mayday_timer);

	/* RCU protected to allow dereferences from get_work_pool() */
	call_rcu(&pool->rcu, rcu_free_pool);
}

/**
 * get_unbound_pool - get a worker_pool with the specified attributes
 * @attrs: the attributes of the worker_pool to get
 *
 * Obtain a worker_pool which has the same attributes as @attrs, bump the
 * reference count and return it.  If there already is a matching
 * worker_pool, it will be used; otherwise, this function attempts to
 * create a new one.
 *
 * Should be called with wq_pool_mutex held.
 *
 * Return: On success, a worker_pool with the same attributes as @attrs.
 * On failure, %NULL.
 */
static struct worker_pool *get_unbound_pool(const struct workqueue_attrs *attrs)
{
	u32 hash = wqattrs_hash(attrs);
	struct worker_pool *pool;
	int node;
	int target_node = NUMA_NO_NODE;

	lockdep_assert_held(&wq_pool_mutex);

	/* do we already have a matching pool? */
	hash_for_each_possible(unbound_pool_hash, pool, hash_node, hash) {
		if (wqattrs_equal(pool->attrs, attrs)) {
			pool->refcnt++;
			return pool;
		}
	}

	/* if cpumask is contained inside a NUMA node, we belong to that node */
	if (wq_numa_enabled) {
		for_each_node(node) {
			if (cpumask_subset(attrs->cpumask,
					   wq_numa_possible_cpumask[node])) {
				target_node = node;
				break;
			}
		}
	}

	/* nope, create a new one */
	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, target_node);
	if (!pool || init_worker_pool(pool) < 0)
		goto fail;

	lockdep_set_subclass(&pool->lock, 1);	/* see put_pwq() */
	copy_workqueue_attrs(pool->attrs, attrs);
	pool->node = target_node;

	/*
	 * no_numa isn't a worker_pool attribute, always clear it.  See
	 * 'struct workqueue_attrs' comments for detail.
	 */
	pool->attrs->no_numa = false;

	if (worker_pool_assign_id(pool) < 0)
		goto fail;

	/* create and start the initial worker */
	if (!create_worker(pool))
		goto fail;

	/* install */
	hash_add(unbound_pool_hash, &pool->hash_node, hash);

	return pool;
fail:
	if (pool)
		put_unbound_pool(pool);
	return NULL;
}

static void rcu_free_pwq(struct rcu_head *rcu)
{
	kmem_cache_free(pwq_cache,
			container_of(rcu, struct pool_workqueue, rcu));
}

/*
 * Scheduled on system_wq by put_pwq() when an unbound pwq hits zero refcnt
 * and needs to be destroyed.
 */
static void pwq_unbound_release_workfn(struct work_struct *work)
{
	struct pool_workqueue *pwq = container_of(work, struct pool_workqueue,
						  unbound_release_work);
	struct workqueue_struct *wq = pwq->wq;
	struct worker_pool *pool = pwq->pool;
	bool is_last;

	if (WARN_ON_ONCE(!(wq->flags & WQ_UNBOUND)))
		return;

	mutex_lock(&wq->mutex);
	list_del_rcu(&pwq->pwqs_node);
	is_last = list_empty(&wq->pwqs);
	mutex_unlock(&wq->mutex);

	mutex_lock(&wq_pool_mutex);
	put_unbound_pool(pool);
	mutex_unlock(&wq_pool_mutex);

	call_rcu(&pwq->rcu, rcu_free_pwq);

	/*
	 * If we're the last pwq going away, @wq is already dead and no one
	 * is gonna access it anymore.  Schedule RCU free.
	 */
	if (is_last)
		call_rcu(&wq->rcu, rcu_free_wq);
}

/**
 * pwq_adjust_max_active - update a pwq's max_active to the current setting
 * @pwq: target pool_workqueue
 *
 * If @pwq isn't freezing, set @pwq->max_active to the associated
 * workqueue's saved_max_active and activate delayed work items
 * accordingly.  If @pwq is freezing, clear @pwq->max_active to zero.
 */
/*
 * 调整pwq的max_active，也就是work项的最大激活数，当队列被冻结时，
 * pwq->max_active被调整为0；解冻后，max_active重新恢复为saved_max_active
 * 
 */
static void pwq_adjust_max_active(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;
	bool freezable = wq->flags & WQ_FREEZABLE;

	/* for @wq->saved_max_active */
	lockdep_assert_held(&wq->mutex);

	/* fast exit for non-freezable wqs */
	if (!freezable && pwq->max_active == wq->saved_max_active)
		return;

	spin_lock_irq(&pwq->pool->lock);

	/*
	 * During [un]freezing, the caller is responsible for ensuring that
	 * this function is called at least once after @workqueue_freezing
	 * is updated and visible.
	 */
	if (!freezable || !workqueue_freezing) {
		pwq->max_active = wq->saved_max_active;

		/*
		 * 解冻后，将delayed_works中的wroks转移到pool->worklist中
		 */
		while (!list_empty(&pwq->delayed_works) &&
		       pwq->nr_active < pwq->max_active)
			pwq_activate_first_delayed(pwq);

		/*
		 * Need to kick a worker after thawed or an unbound wq's
		 * max_active is bumped.  It's a slow path.  Do it always.
		 */
		wake_up_worker(pwq->pool);
	} else {
		pwq->max_active = 0;
	}

	spin_unlock_irq(&pwq->pool->lock);
}

/* initialize newly alloced @pwq which is associated with @wq and @pool */
static void init_pwq(struct pool_workqueue *pwq, struct workqueue_struct *wq,
		     struct worker_pool *pool)
{
	BUG_ON((unsigned long)pwq & WORK_STRUCT_FLAG_MASK);

	memset(pwq, 0, sizeof(*pwq));

	pwq->pool = pool;
	pwq->wq = wq;
	pwq->flush_color = -1;
	pwq->refcnt = 1;
	INIT_LIST_HEAD(&pwq->delayed_works);
	INIT_LIST_HEAD(&pwq->pwqs_node);
	INIT_LIST_HEAD(&pwq->mayday_node);
	INIT_WORK(&pwq->unbound_release_work, pwq_unbound_release_workfn);
}

/* sync @pwq with the current state of its associated wq and link it */
static void link_pwq(struct pool_workqueue *pwq)
{
	struct workqueue_struct *wq = pwq->wq;

	lockdep_assert_held(&wq->mutex);

	/* may be called multiple times, ignore if already linked */
	if (!list_empty(&pwq->pwqs_node))
		return;

	/* set the matching work_color */
	pwq->work_color = wq->work_color;

	/* sync max_active to the current setting */
	pwq_adjust_max_active(pwq);

	/* link in @pwq */
	list_add_rcu(&pwq->pwqs_node, &wq->pwqs);
}

/* obtain a pool matching @attr and create a pwq associating the pool and @wq */
static struct pool_workqueue *alloc_unbound_pwq(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	lockdep_assert_held(&wq_pool_mutex);

	pool = get_unbound_pool(attrs);
	if (!pool)
		return NULL;

	pwq = kmem_cache_alloc_node(pwq_cache, GFP_KERNEL, pool->node);
	if (!pwq) {
		put_unbound_pool(pool);
		return NULL;
	}

	init_pwq(pwq, wq, pool);
	return pwq;
}

/**
 * wq_calc_node_cpumask - calculate a wq_attrs' cpumask for the specified node
 * @attrs: the wq_attrs of the default pwq of the target workqueue
 * @node: the target NUMA node
 * @cpu_going_down: if >= 0, the CPU to consider as offline
 * @cpumask: outarg, the resulting cpumask
 *
 * Calculate the cpumask a workqueue with @attrs should use on @node.  If
 * @cpu_going_down is >= 0, that cpu is considered offline during
 * calculation.  The result is stored in @cpumask.
 *
 * If NUMA affinity is not enabled, @attrs->cpumask is always used.  If
 * enabled and @node has online CPUs requested by @attrs, the returned
 * cpumask is the intersection of the possible CPUs of @node and
 * @attrs->cpumask.
 *
 * The caller is responsible for ensuring that the cpumask of @node stays
 * stable.
 *
 * Return: %true if the resulting @cpumask is different from @attrs->cpumask,
 * %false if equal.
 */
static bool wq_calc_node_cpumask(const struct workqueue_attrs *attrs, int node,
				 int cpu_going_down, cpumask_t *cpumask)
{
	if (!wq_numa_enabled || attrs->no_numa)
		goto use_dfl;

	/* does @node have any online CPUs @attrs wants? */
	cpumask_and(cpumask, cpumask_of_node(node), attrs->cpumask);
	if (cpu_going_down >= 0)
		cpumask_clear_cpu(cpu_going_down, cpumask);

	if (cpumask_empty(cpumask))
		goto use_dfl;

	/* yeap, return possible CPUs in @node that @attrs wants */
	cpumask_and(cpumask, attrs->cpumask, wq_numa_possible_cpumask[node]);
	return !cpumask_equal(cpumask, attrs->cpumask);

use_dfl:
	cpumask_copy(cpumask, attrs->cpumask);
	return false;
}

/* install @pwq into @wq's numa_pwq_tbl[] for @node and return the old pwq */
static struct pool_workqueue *numa_pwq_tbl_install(struct workqueue_struct *wq,
						   int node,
						   struct pool_workqueue *pwq)
{
	struct pool_workqueue *old_pwq;

	lockdep_assert_held(&wq_pool_mutex);
	lockdep_assert_held(&wq->mutex);

	/* link_pwq() can handle duplicate calls */
	link_pwq(pwq);

	old_pwq = rcu_access_pointer(wq->numa_pwq_tbl[node]);
	rcu_assign_pointer(wq->numa_pwq_tbl[node], pwq);
	return old_pwq;
}

/* context to store the prepared attrs & pwqs before applying */
struct apply_wqattrs_ctx {
	struct workqueue_struct	*wq;		/* target workqueue */
	struct workqueue_attrs	*attrs;		/* attrs to apply */
	struct list_head	list;		/* queued for batching commit */
	struct pool_workqueue	*dfl_pwq;
	struct pool_workqueue	*pwq_tbl[];
};

/* free the resources after success or abort */
static void apply_wqattrs_cleanup(struct apply_wqattrs_ctx *ctx)
{
	if (ctx) {
		int node;

		for_each_node(node)
			put_pwq_unlocked(ctx->pwq_tbl[node]);
		put_pwq_unlocked(ctx->dfl_pwq);

		free_workqueue_attrs(ctx->attrs);

		kfree(ctx);
	}
}

/* allocate the attrs and pwqs for later installation */
static struct apply_wqattrs_ctx *
apply_wqattrs_prepare(struct workqueue_struct *wq,
		      const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;
	struct workqueue_attrs *new_attrs, *tmp_attrs;
	int node;

	lockdep_assert_held(&wq_pool_mutex);

	ctx = kzalloc(sizeof(*ctx) + nr_node_ids * sizeof(ctx->pwq_tbl[0]),
		      GFP_KERNEL);

	new_attrs = alloc_workqueue_attrs(GFP_KERNEL);
	tmp_attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if (!ctx || !new_attrs || !tmp_attrs)
		goto out_free;

	/*
	 * Calculate the attrs of the default pwq.
	 * If the user configured cpumask doesn't overlap with the
	 * wq_unbound_cpumask, we fallback to the wq_unbound_cpumask.
	 */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, wq_unbound_cpumask);
	if (unlikely(cpumask_empty(new_attrs->cpumask)))
		cpumask_copy(new_attrs->cpumask, wq_unbound_cpumask);

	/*
	 * We may create multiple pwqs with differing cpumasks.  Make a
	 * copy of @new_attrs which will be modified and used to obtain
	 * pools.
	 */
	copy_workqueue_attrs(tmp_attrs, new_attrs);

	/*
	 * If something goes wrong during CPU up/down, we'll fall back to
	 * the default pwq covering whole @attrs->cpumask.  Always create
	 * it even if we don't use it immediately.
	 */
	ctx->dfl_pwq = alloc_unbound_pwq(wq, new_attrs);
	if (!ctx->dfl_pwq)
		goto out_free;

	for_each_node(node) {
		if (wq_calc_node_cpumask(new_attrs, node, -1, tmp_attrs->cpumask)) {
			ctx->pwq_tbl[node] = alloc_unbound_pwq(wq, tmp_attrs);
			if (!ctx->pwq_tbl[node])
				goto out_free;
		} else {
			ctx->dfl_pwq->refcnt++;
			ctx->pwq_tbl[node] = ctx->dfl_pwq;
		}
	}

	/* save the user configured attrs and sanitize it. */
	copy_workqueue_attrs(new_attrs, attrs);
	cpumask_and(new_attrs->cpumask, new_attrs->cpumask, cpu_possible_mask);
	ctx->attrs = new_attrs;

	ctx->wq = wq;
	free_workqueue_attrs(tmp_attrs);
	return ctx;

out_free:
	free_workqueue_attrs(tmp_attrs);
	free_workqueue_attrs(new_attrs);
	apply_wqattrs_cleanup(ctx);
	return NULL;
}

/* set attrs and install prepared pwqs, @ctx points to old pwqs on return */
static void apply_wqattrs_commit(struct apply_wqattrs_ctx *ctx)
{
	int node;

	/* all pwqs have been created successfully, let's install'em */
	mutex_lock(&ctx->wq->mutex);

	copy_workqueue_attrs(ctx->wq->unbound_attrs, ctx->attrs);

	/* save the previous pwq and install the new one */
	for_each_node(node)
		ctx->pwq_tbl[node] = numa_pwq_tbl_install(ctx->wq, node,
							  ctx->pwq_tbl[node]);

	/* @dfl_pwq might not have been used, ensure it's linked */
	link_pwq(ctx->dfl_pwq);
	swap(ctx->wq->dfl_pwq, ctx->dfl_pwq);

	mutex_unlock(&ctx->wq->mutex);
}

static void apply_wqattrs_lock(void)
{
	/* CPUs should stay stable across pwq creations and installations */
	get_online_cpus();
	mutex_lock(&wq_pool_mutex);
}

static void apply_wqattrs_unlock(void)
{
	mutex_unlock(&wq_pool_mutex);
	put_online_cpus();
}

static int apply_workqueue_attrs_locked(struct workqueue_struct *wq,
					const struct workqueue_attrs *attrs)
{
	struct apply_wqattrs_ctx *ctx;
	int ret = -ENOMEM;

	/* only unbound workqueues can change attributes */
	if (WARN_ON(!(wq->flags & WQ_UNBOUND)))
		return -EINVAL;

	/* creating multiple pwqs breaks ordering guarantee */
	if (!list_empty(&wq->pwqs)) {
		if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
			return -EINVAL;

		wq->flags &= ~__WQ_ORDERED;
	}

	ctx = apply_wqattrs_prepare(wq, attrs);

	/* the ctx has been prepared successfully, let's commit it */
	if (ctx) {
		apply_wqattrs_commit(ctx);
		ret = 0;
	}

	apply_wqattrs_cleanup(ctx);

	return ret;
}

/**
 * apply_workqueue_attrs - apply new workqueue_attrs to an unbound workqueue
 * @wq: the target workqueue
 * @attrs: the workqueue_attrs to apply, allocated with alloc_workqueue_attrs()
 *
 * Apply @attrs to an unbound workqueue @wq.  Unless disabled, on NUMA
 * machines, this function maps a separate pwq to each NUMA node with
 * possibles CPUs in @attrs->cpumask so that work items are affine to the
 * NUMA node it was issued on.  Older pwqs are released as in-flight work
 * items finish.  Note that a work item which repeatedly requeues itself
 * back-to-back will stay on its current pwq.
 *
 * Performs GFP_KERNEL allocations.
 *
 * Return: 0 on success and -errno on failure.
 */
int apply_workqueue_attrs(struct workqueue_struct *wq,
			  const struct workqueue_attrs *attrs)
{
	int ret;

	apply_wqattrs_lock();
	ret = apply_workqueue_attrs_locked(wq, attrs);
	apply_wqattrs_unlock();

	return ret;
}

/**
 * wq_update_unbound_numa - update NUMA affinity of a wq for CPU hot[un]plug
 * @wq: the target workqueue
 * @cpu: the CPU coming up or going down
 * @online: whether @cpu is coming up or going down
 *
 * This function is to be called from %CPU_DOWN_PREPARE, %CPU_ONLINE and
 * %CPU_DOWN_FAILED.  @cpu is being hot[un]plugged, update NUMA affinity of
 * @wq accordingly.
 *
 * If NUMA affinity can't be adjusted due to memory allocation failure, it
 * falls back to @wq->dfl_pwq which may not be optimal but is always
 * correct.
 *
 * Note that when the last allowed CPU of a NUMA node goes offline for a
 * workqueue with a cpumask spanning multiple nodes, the workers which were
 * already executing the work items for the workqueue will lose their CPU
 * affinity and may execute on any CPU.  This is similar to how per-cpu
 * workqueues behave on CPU_DOWN.  If a workqueue user wants strict
 * affinity, it's the user's responsibility to flush the work item from
 * CPU_DOWN_PREPARE.
 */
static void wq_update_unbound_numa(struct workqueue_struct *wq, int cpu,
				   bool online)
{
	int node = cpu_to_node(cpu);
	int cpu_off = online ? -1 : cpu;
	struct pool_workqueue *old_pwq = NULL, *pwq;
	struct workqueue_attrs *target_attrs;
	cpumask_t *cpumask;

	lockdep_assert_held(&wq_pool_mutex);

	if (!wq_numa_enabled || !(wq->flags & WQ_UNBOUND) ||
	    wq->unbound_attrs->no_numa)
		return;

	/*
	 * We don't wanna alloc/free wq_attrs for each wq for each CPU.
	 * Let's use a preallocated one.  The following buf is protected by
	 * CPU hotplug exclusion.
	 */
	target_attrs = wq_update_unbound_numa_attrs_buf;
	cpumask = target_attrs->cpumask;

	copy_workqueue_attrs(target_attrs, wq->unbound_attrs);
	pwq = unbound_pwq_by_node(wq, node);

	/*
	 * Let's determine what needs to be done.  If the target cpumask is
	 * different from the default pwq's, we need to compare it to @pwq's
	 * and create a new one if they don't match.  If the target cpumask
	 * equals the default pwq's, the default pwq should be used.
	 */
	if (wq_calc_node_cpumask(wq->dfl_pwq->pool->attrs, node, cpu_off, cpumask)) {
		if (cpumask_equal(cpumask, pwq->pool->attrs->cpumask))
			return;
	} else {
		goto use_dfl_pwq;
	}

	/* create a new pwq */
	pwq = alloc_unbound_pwq(wq, target_attrs);
	if (!pwq) {
		pr_warn("workqueue: allocation failed while updating NUMA affinity of \"%s\"\n",
			wq->name);
		goto use_dfl_pwq;
	}

	/* Install the new pwq. */
	mutex_lock(&wq->mutex);
	old_pwq = numa_pwq_tbl_install(wq, node, pwq);
	goto out_unlock;

use_dfl_pwq:
	mutex_lock(&wq->mutex);
	spin_lock_irq(&wq->dfl_pwq->pool->lock);
	get_pwq(wq->dfl_pwq);
	spin_unlock_irq(&wq->dfl_pwq->pool->lock);
	old_pwq = numa_pwq_tbl_install(wq, node, wq->dfl_pwq);
out_unlock:
	mutex_unlock(&wq->mutex);
	put_pwq_unlocked(old_pwq);
}

static int alloc_and_link_pwqs(struct workqueue_struct *wq)
{
	bool highpri = wq->flags & WQ_HIGHPRI;
	int cpu, ret;

	if (!(wq->flags & WQ_UNBOUND)) {
		wq->cpu_pwqs = alloc_percpu(struct pool_workqueue);
		if (!wq->cpu_pwqs)
			return -ENOMEM;

		/*
		 * 针对bound型wq，所有的cpu work_pools是共享的，每个wq分享所有的pool。
		 * 这里将wq关联到所有cpu的pool上，建立wq和work_pool的映射关系。
		 * 通过cpu_pwqs指针来找到对应的work_pool
		 **/
		for_each_possible_cpu(cpu) {
			struct pool_workqueue *pwq =
				per_cpu_ptr(wq->cpu_pwqs, cpu);
			struct worker_pool *cpu_pools =
				per_cpu(cpu_worker_pools, cpu);

			/* 初始化pwq结构，主要是对pwq成员进行赋初值 */
			init_pwq(pwq, wq, &cpu_pools[highpri]);

			mutex_lock(&wq->mutex);
			/* 将wq的一些参数状态同步到pwq，并将新的pwq加入到wq->pwqs list中*/
			link_pwq(pwq);
			mutex_unlock(&wq->mutex);
		}
		return 0;
	} else if (wq->flags & __WQ_ORDERED) {
		ret = apply_workqueue_attrs(wq, ordered_wq_attrs[highpri]);
		/* there should only be single pwq for ordering guarantee */
		WARN(!ret && (wq->pwqs.next != &wq->dfl_pwq->pwqs_node ||
			      wq->pwqs.prev != &wq->dfl_pwq->pwqs_node),
		     "ordering guarantee broken for workqueue %s\n", wq->name);
		return ret;
	} else {
		return apply_workqueue_attrs(wq, unbound_std_wq_attrs[highpri]);
	}
}

/*
 * 函数说明：
 * 根据传入的max_active和wq flags，来确定实际max_active，确保max_active在系统范围之内。
 * max_active代表一个wq可以排队的work最大数量。
 *
 * 通常：
 * bound-wq：max_active=256  //WQ_MAX_ACTIVE/2
 * unbound-wq：max_active=256 
 *
 * 返回值：
 * 	最终确定的max_active。
 */
static int wq_clamp_max_active(int max_active, unsigned int flags,
			       const char *name)
{
	int lim = flags & WQ_UNBOUND ? WQ_UNBOUND_MAX_ACTIVE : WQ_MAX_ACTIVE;

	if (max_active < 1 || max_active > lim)
		pr_warn("workqueue: max_active %d requested for %s is out of range, clamping between %d and %d\n",
			max_active, name, 1, lim);

	return clamp_val(max_active, 1, lim);
}

struct workqueue_struct *__alloc_workqueue_key(const char *fmt,
					       unsigned int flags,
					       int max_active,
					       struct lock_class_key *key,
					       const char *lock_name, ...)
{
	size_t tbl_size = 0;
	va_list args;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	/*
	 * Unbound && max_active == 1 used to imply ordered, which is no
	 * longer the case on NUMA machines due to per-node pools.  While
	 * alloc_ordered_workqueue() is the right way to create an ordered
	 * workqueue, keep the previous behavior to avoid subtle breakages
	 * on NUMA.
	 */
	if ((flags & WQ_UNBOUND) && max_active == 1)
		flags |= __WQ_ORDERED;

	/* see the comment above the definition of WQ_POWER_EFFICIENT */
	if ((flags & WQ_POWER_EFFICIENT) && wq_power_efficient)
		flags |= WQ_UNBOUND;

	/* allocate wq and format name */
	if (flags & WQ_UNBOUND)
		tbl_size = nr_node_ids * sizeof(wq->numa_pwq_tbl[0]);

	wq = kzalloc(sizeof(*wq) + tbl_size, GFP_KERNEL);
	if (!wq)
		return NULL;

	if (flags & WQ_UNBOUND) {
		wq->unbound_attrs = alloc_workqueue_attrs(GFP_KERNEL);
		if (!wq->unbound_attrs)
			goto err_free_wq;
	}

	va_start(args, lock_name);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);

	max_active = max_active ?: WQ_DFL_ACTIVE;
	max_active = wq_clamp_max_active(max_active, flags, wq->name);

	/* init wq */
	wq->flags = flags;
	wq->saved_max_active = max_active;
	mutex_init(&wq->mutex);
	atomic_set(&wq->nr_pwqs_to_flush, 0);
	INIT_LIST_HEAD(&wq->pwqs);
	INIT_LIST_HEAD(&wq->flusher_queue);
	INIT_LIST_HEAD(&wq->flusher_overflow);
	INIT_LIST_HEAD(&wq->maydays);

	lockdep_init_map(&wq->lockdep_map, lock_name, key, 0);
	INIT_LIST_HEAD(&wq->list);

	if (alloc_and_link_pwqs(wq) < 0)
		goto err_free_wq;

	/*
	 * Workqueues which may be used during memory reclaim should
	 * have a rescuer to guarantee forward progress.
	 */
	if (flags & WQ_MEM_RECLAIM) {
		struct worker *rescuer;

		rescuer = alloc_worker(NUMA_NO_NODE);
		if (!rescuer)
			goto err_destroy;

		rescuer->rescue_wq = wq;
		rescuer->task = kthread_create(rescuer_thread, rescuer, "%s",
					       wq->name);
		if (IS_ERR(rescuer->task)) {
			kfree(rescuer);
			goto err_destroy;
		}

		wq->rescuer = rescuer;
		kthread_bind_mask(rescuer->task, cpu_possible_mask);
		wake_up_process(rescuer->task);
	}

	if ((wq->flags & WQ_SYSFS) && workqueue_sysfs_register(wq))
		goto err_destroy;

	/*
	 * wq_pool_mutex protects global freeze state and workqueues list.
	 * Grab it, adjust max_active and add the new @wq to workqueues
	 * list.
	 */
	mutex_lock(&wq_pool_mutex);

	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);
	mutex_unlock(&wq->mutex);

	list_add_tail_rcu(&wq->list, &workqueues);

	mutex_unlock(&wq_pool_mutex);

	return wq;

err_free_wq:
	free_workqueue_attrs(wq->unbound_attrs);
	kfree(wq);
	return NULL;
err_destroy:
	destroy_workqueue(wq);
	return NULL;
}
EXPORT_SYMBOL_GPL(__alloc_workqueue_key);

/**
 * destroy_workqueue - safely terminate a workqueue
 * @wq: target workqueue
 *
 * Safely destroy a workqueue. All work currently pending will be done first.
 */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	int node;

	/* drain it before proceeding with destruction */
	drain_workqueue(wq);

	/* sanity checks */
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		int i;

		for (i = 0; i < WORK_NR_COLORS; i++) {
			if (WARN_ON(pwq->nr_in_flight[i])) {
				mutex_unlock(&wq->mutex);
				return;
			}
		}

		if (WARN_ON((pwq != wq->dfl_pwq) && (pwq->refcnt > 1)) ||
		    WARN_ON(pwq->nr_active) ||
		    WARN_ON(!list_empty(&pwq->delayed_works))) {
			mutex_unlock(&wq->mutex);
			return;
		}
	}
	mutex_unlock(&wq->mutex);

	/*
	 * wq list is used to freeze wq, remove from list after
	 * flushing is complete in case freeze races us.
	 */
	mutex_lock(&wq_pool_mutex);
	list_del_rcu(&wq->list);
	mutex_unlock(&wq_pool_mutex);

	workqueue_sysfs_unregister(wq);

	if (wq->rescuer)
		kthread_stop(wq->rescuer->task);

	if (!(wq->flags & WQ_UNBOUND)) {
		/*
		 * The base ref is never dropped on per-cpu pwqs.  Directly
		 * schedule RCU free.
		 */
		call_rcu(&wq->rcu, rcu_free_wq);
	} else {
		/*
		 * We're the sole accessor of @wq at this point.  Directly
		 * access numa_pwq_tbl[] and dfl_pwq to put the base refs.
		 * @wq will be freed when the last pwq is released.
		 */
		for_each_node(node) {
			pwq = rcu_access_pointer(wq->numa_pwq_tbl[node]);
			RCU_INIT_POINTER(wq->numa_pwq_tbl[node], NULL);
			put_pwq_unlocked(pwq);
		}

		/*
		 * Put dfl_pwq.  @wq may be freed any time after dfl_pwq is
		 * put.  Don't access it afterwards.
		 */
		pwq = wq->dfl_pwq;
		wq->dfl_pwq = NULL;
		put_pwq_unlocked(pwq);
	}
}
EXPORT_SYMBOL_GPL(destroy_workqueue);

/**
 * workqueue_set_max_active - adjust max_active of a workqueue
 * @wq: target workqueue
 * @max_active: new max_active value.
 *
 * Set max_active of @wq to @max_active.
 *
 * CONTEXT:
 * Don't call from IRQ context.
 */
void workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	struct pool_workqueue *pwq;

	/* disallow meddling with max_active for ordered workqueues */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return;

	max_active = wq_clamp_max_active(max_active, wq->flags, wq->name);

	mutex_lock(&wq->mutex);

	wq->flags &= ~__WQ_ORDERED;
	wq->saved_max_active = max_active;

	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);

	mutex_unlock(&wq->mutex);
}
EXPORT_SYMBOL_GPL(workqueue_set_max_active);

/**
 * current_work - retrieve %current task's work struct
 *
 * Determine if %current task is a workqueue worker and what it's working on.
 * Useful to find out the context that the %current task is running in.
 *
 * Return: work struct if %current task is a workqueue worker, %NULL otherwise.
 */
struct work_struct *current_work(void)
{
	struct worker *worker = current_wq_worker();

	return worker ? worker->current_work : NULL;
}
EXPORT_SYMBOL(current_work);

/**
 * current_is_workqueue_rescuer - is %current workqueue rescuer?
 *
 * Determine whether %current is a workqueue rescuer.  Can be used from
 * work functions to determine whether it's being run off the rescuer task.
 *
 * Return: %true if %current is a workqueue rescuer. %false otherwise.
 */
bool current_is_workqueue_rescuer(void)
{
	struct worker *worker = current_wq_worker();

	return worker && worker->rescue_wq;
}

/**
 * workqueue_congested - test whether a workqueue is congested
 * @cpu: CPU in question
 * @wq: target workqueue
 *
 * Test whether @wq's cpu workqueue for @cpu is congested.  There is
 * no synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * If @cpu is WORK_CPU_UNBOUND, the test is performed on the local CPU.
 * Note that both per-cpu and unbound workqueues may be associated with
 * multiple pool_workqueues which have separate congested states.  A
 * workqueue being congested on one CPU doesn't mean the workqueue is also
 * contested on other CPUs / NUMA nodes.
 *
 * Return:
 * %true if congested, %false otherwise.
 */
bool workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	struct pool_workqueue *pwq;
	bool ret;

	rcu_read_lock();
	preempt_disable();

	if (cpu == WORK_CPU_UNBOUND)
		cpu = smp_processor_id();

	if (!(wq->flags & WQ_UNBOUND))
		pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	else
		pwq = unbound_pwq_by_node(wq, cpu_to_node(cpu));

	ret = !list_empty(&pwq->delayed_works);
	preempt_enable();
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(workqueue_congested);

/**
 * work_busy - test whether a work is currently pending or running
 * @work: the work to be tested
 *
 * Test whether @work is currently pending or running.  There is no
 * synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * Return:
 * OR'd bitmask of WORK_BUSY_* bits.
 */
/*
 * 函数说明：
 * 判断一个work是否busy。busy包含2种状态：
 * 1. work处于pending。
 * --- pending位通常在insert_work->set_work_pwq中置位，表示work已经挂入wq，等待被处理。
 * 2. work处于running。
 * --- 这里的running包含了阻塞状态，只要work_func没有执行完成就是running。
 *
 * 函数实现：
 * 函数通过判断pending标志位，及running状态来确定work是否busy
 *
 * 返回值：
 * 0: work已完成（不再运行）
 * WORK_BUSY_PENDING: work已经pending
 * WORK_BUSY_RUNNING: work正在running
 *
 * NOTE:
 * 这个函数没有同步，其结果不一定可靠，仅作为建议提示和调试使用。
 */
unsigned int work_busy(struct work_struct *work)
{
	struct worker_pool *pool;
	unsigned long flags;
	unsigned int ret = 0;

	/*
	 * 一个work项被queue后，会置为PENDING状态。
	 */
	if (work_pending(work))
		ret |= WORK_BUSY_PENDING;

	rcu_read_lock();
	pool = get_work_pool(work);
	if (pool) {
		spin_lock_irqsave(&pool->lock, flags);
		/*
		 * 如果work处于running状态，额外添加BUSY_RUNNING标志
		 */
		if (find_worker_executing_work(pool, work))
			ret |= WORK_BUSY_RUNNING;
		spin_unlock_irqrestore(&pool->lock, flags);
	}
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(work_busy);

/**
 * set_worker_desc - set description for the current work item
 * @fmt: printf-style format string
 * @...: arguments for the format string
 *
 * This function can be called by a running work function to describe what
 * the work item is about.  If the worker task gets dumped, this
 * information will be printed out together to help debugging.  The
 * description can be at most WORKER_DESC_LEN including the trailing '\0'.
 */
void set_worker_desc(const char *fmt, ...)
{
	struct worker *worker = current_wq_worker();
	va_list args;

	if (worker) {
		va_start(args, fmt);
		vsnprintf(worker->desc, sizeof(worker->desc), fmt, args);
		va_end(args);
		worker->desc_valid = true;
	}
}

/**
 * print_worker_info - print out worker information and description
 * @log_lvl: the log level to use when printing
 * @task: target task
 *
 * If @task is a worker and currently executing a work item, print out the
 * name of the workqueue being serviced and worker description set with
 * set_worker_desc() by the currently executing work item.
 *
 * This function can be safely called on any task as long as the
 * task_struct itself is accessible.  While safe, this function isn't
 * synchronized and may print out mixups or garbages of limited length.
 */
void print_worker_info(const char *log_lvl, struct task_struct *task)
{
	work_func_t *fn = NULL;
	char name[WQ_NAME_LEN] = { };
	char desc[WORKER_DESC_LEN] = { };
	struct pool_workqueue *pwq = NULL;
	struct workqueue_struct *wq = NULL;
	bool desc_valid = false;
	struct worker *worker;

	if (!(task->flags & PF_WQ_WORKER))
		return;

	/*
	 * This function is called without any synchronization and @task
	 * could be in any state.  Be careful with dereferences.
	 */
	worker = probe_kthread_data(task);

	/*
	 * Carefully copy the associated workqueue's workfn and name.  Keep
	 * the original last '\0' in case the original contains garbage.
	 */
	probe_kernel_read(&fn, &worker->current_func, sizeof(fn));
	probe_kernel_read(&pwq, &worker->current_pwq, sizeof(pwq));
	probe_kernel_read(&wq, &pwq->wq, sizeof(wq));
	probe_kernel_read(name, wq->name, sizeof(name) - 1);

	/* copy worker description */
	probe_kernel_read(&desc_valid, &worker->desc_valid, sizeof(desc_valid));
	if (desc_valid)
		probe_kernel_read(desc, worker->desc, sizeof(desc) - 1);

	if (fn || name[0] || desc[0]) {
		printk("%sWorkqueue: %s %pf", log_lvl, name, fn);
		if (desc[0])
			pr_cont(" (%s)", desc);
		pr_cont("\n");
	}
}

static void pr_cont_pool_info(struct worker_pool *pool)
{
	pr_cont(" cpus=%*pbl", nr_cpumask_bits, pool->attrs->cpumask);
	if (pool->node != NUMA_NO_NODE)
		pr_cont(" node=%d", pool->node);
	pr_cont(" flags=0x%x nice=%d", pool->flags, pool->attrs->nice);
}

static void pr_cont_work(bool comma, struct work_struct *work)
{
	if (work->func == wq_barrier_func) {
		struct wq_barrier *barr;

		barr = container_of(work, struct wq_barrier, work);

		pr_cont("%s BAR(%d)", comma ? "," : "",
			task_pid_nr(barr->task));
	} else {
		pr_cont("%s %pf", comma ? "," : "", work->func);
	}
}

static void show_pwq(struct pool_workqueue *pwq)
{
	struct worker_pool *pool = pwq->pool;
	struct work_struct *work;
	struct worker *worker;
	bool has_in_flight = false, has_pending = false;
	int bkt;

	pr_info("  pwq %d:", pool->id);
	pr_cont_pool_info(pool);

	pr_cont(" active=%d/%d%s\n", pwq->nr_active, pwq->max_active,
		!list_empty(&pwq->mayday_node) ? " MAYDAY" : "");

	hash_for_each(pool->busy_hash, bkt, worker, hentry) {
		if (worker->current_pwq == pwq) {
			has_in_flight = true;
			break;
		}
	}
	if (has_in_flight) {
		bool comma = false;

		pr_info("    in-flight:");
		hash_for_each(pool->busy_hash, bkt, worker, hentry) {
			if (worker->current_pwq != pwq)
				continue;

			pr_cont("%s %d%s:%pf", comma ? "," : "",
				task_pid_nr(worker->task),
				worker == pwq->wq->rescuer ? "(RESCUER)" : "",
				worker->current_func);
			list_for_each_entry(work, &worker->scheduled, entry)
				pr_cont_work(false, work);
			comma = true;
		}
		pr_cont("\n");
	}

	list_for_each_entry(work, &pool->worklist, entry) {
		if (get_work_pwq(work) == pwq) {
			has_pending = true;
			break;
		}
	}
	if (has_pending) {
		bool comma = false;

		pr_info("    pending:");
		list_for_each_entry(work, &pool->worklist, entry) {
			if (get_work_pwq(work) != pwq)
				continue;

			pr_cont_work(comma, work);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont("\n");
	}

	if (!list_empty(&pwq->delayed_works)) {
		bool comma = false;

		pr_info("    delayed:");
		list_for_each_entry(work, &pwq->delayed_works, entry) {
			pr_cont_work(comma, work);
			comma = !(*work_data_bits(work) & WORK_STRUCT_LINKED);
		}
		pr_cont("\n");
	}
}

 
#if 0
struct workqueue_struct {
	struct list_head	pwqs;		/* WR: all pwqs of this wq */
	struct list_head	list;		/* PR: list of all workqueues */

	struct mutex		mutex;		/* protects this wq */
	int			work_color;	/* WQ: current work color */
	int			flush_color;	/* WQ: current flush color */
	atomic_t		nr_pwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* WQ: first flusher */
	struct list_head	flusher_queue;	/* WQ: flush waiters */
	struct list_head	flusher_overflow; /* WQ: flush overflow list */

	struct list_head	maydays;	/* MD: pwqs requesting rescue */
	struct worker		*rescuer;	/* I: rescue worker */

	int			nr_drainers;	/* WQ: drain in progress */
	int			saved_max_active; /* WQ: saved pwq max_active */

	struct workqueue_attrs	*unbound_attrs;	/* PW: only for unbound wqs */
	struct pool_workqueue	*dfl_pwq;	/* PW: only for unbound wqs */

#ifdef CONFIG_SYSFS
	struct wq_device	*wq_dev;	/* I: for sysfs interface */
#endif
#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lockdep_map;
#endif
	char			name[WQ_NAME_LEN]; /* I: workqueue name */

	/*
	 * Destruction of workqueue_struct is sched-RCU protected to allow
	 * walking the workqueues list without grabbing wq_pool_mutex.
	 * This is used to dump all workqueues from sysrq.
	 */
	struct rcu_head		rcu;

	/* hot fields used during command issue, aligned to cacheline */
	unsigned int		flags ____cacheline_aligned; /* WQ: WQ_* flags */
	struct pool_workqueue __percpu *cpu_pwqs; /* I: per-cpu pwqs */
	struct pool_workqueue __rcu *numa_pwq_tbl[]; /* PWR: unbound pwqs indexed by node */
};
#endif
void show_wq_info(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	list_for_each_entry_rcu(wq, &workqueues, list) {
		printk("wq->name: %s\n", wq->name);
		printk("wq->flags: 0x%08x\n", wq->flags);
		printk("wq->cpu_pwqs: 0x%p\n", wq->cpu_pwqs);
		
	}
}
void show_wq_state(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;
	unsigned long flags;
	int i = 0, j = 0;
	list_for_each_entry_rcu(wq, &workqueues, list) {
		printk("-----------%d-----------\n", i++);
		printk("wq->name: %s\n", wq->name);
		printk("wq->flags: 0x%x\n", wq->flags);
		printk("wq->cpu_pwqs: 0x%p\n", wq->cpu_pwqs);
		printk("wq->numa_pwq_tbl: 0x%p\n", wq->numa_pwq_tbl);
		j = 0;
		for_each_pwq(pwq, wq) {
			printk("\twq->pwq[%d] 0x%p, pool 0x%p, pool_cpu %d\n", j, pwq, pwq->pool, pwq->pool->cpu);
			j++;
		}
	}
}

long work_func(void *args)
{
	printk("run work in!\n");
	printk("current cpu %d!\n", smp_processor_id());
	printk("run work out!\n");
}

long work_func_intensive(void *args)
{
	printk("run intensive work in!\n");
	printk("current cpu %d!\n", smp_processor_id());
	mdelay(1000);
	mdelay(1000);
	mdelay(1000);
	mdelay(1000);
	printk("run intensive work out!\n");
}

void work_test(void)
{
	work_on_cpu(WORK_CPU_UNBOUND, work_func_intensive, NULL);
	work_on_cpu(WORK_CPU_UNBOUND, work_func, NULL);
}


/**
 * show_workqueue_state - dump workqueue state
 *
 * Called from a sysrq handler and prints out all busy workqueues and
 * pools.
 */
void show_workqueue_state(void)
{
	struct workqueue_struct *wq;
	struct worker_pool *pool;
	unsigned long flags;
	int pi;

	rcu_read_lock();

	show_wq_state();
	work_test();
	pr_info("Showing busy workqueues and worker pools:\n");

	list_for_each_entry_rcu(wq, &workqueues, list) {
		struct pool_workqueue *pwq;
		bool idle = true;

		for_each_pwq(pwq, wq) {
			if (pwq->nr_active || !list_empty(&pwq->delayed_works)) {
				idle = false;
				break;
			}
		}
		if (idle)
			continue;

		pr_info("workqueue %s: flags=0x%x\n", wq->name, wq->flags);

		for_each_pwq(pwq, wq) {
			spin_lock_irqsave(&pwq->pool->lock, flags);
			if (pwq->nr_active || !list_empty(&pwq->delayed_works))
				show_pwq(pwq);
			spin_unlock_irqrestore(&pwq->pool->lock, flags);
		}
	}

	for_each_pool(pool, pi) {
		struct worker *worker;
		bool first = true;

		spin_lock_irqsave(&pool->lock, flags);
		if (pool->nr_workers == pool->nr_idle)
			goto next_pool;

		pr_info("pool %d:", pool->id);
		pr_cont_pool_info(pool);
		pr_cont(" workers=%d", pool->nr_workers);
		if (pool->manager)
			pr_cont(" manager: %d",
				task_pid_nr(pool->manager->task));
		list_for_each_entry(worker, &pool->idle_list, entry) {
			pr_cont(" %s%d", first ? "idle: " : "",
				task_pid_nr(worker->task));
			first = false;
		}
		pr_cont("\n");
	next_pool:
		spin_unlock_irqrestore(&pool->lock, flags);
	}

	rcu_read_unlock();
}

/*
 * CPU hotplug.
 *
 * There are two challenges in supporting CPU hotplug.  Firstly, there
 * are a lot of assumptions on strong associations among work, pwq and
 * pool which make migrating pending and scheduled works very
 * difficult to implement without impacting hot paths.  Secondly,
 * worker pools serve mix of short, long and very long running works making
 * blocked draining impractical.
 *
 * This is solved by allowing the pools to be disassociated from the CPU
 * running as an unbound one and allowing it to be reattached later if the
 * cpu comes back online.
 */

static void wq_unbind_fn(struct work_struct *work)
{
	int cpu = smp_processor_id();
	struct worker_pool *pool;
	struct worker *worker;

	for_each_cpu_worker_pool(pool, cpu) {
		mutex_lock(&pool->attach_mutex);
		spin_lock_irq(&pool->lock);

		/*
		 * We've blocked all attach/detach operations. Make all workers
		 * unbound and set DISASSOCIATED.  Before this, all workers
		 * except for the ones which are still executing works from
		 * before the last CPU down must be on the cpu.  After
		 * this, they may become diasporas.
		 */
		for_each_pool_worker(worker, pool)
			worker->flags |= WORKER_UNBOUND;

		pool->flags |= POOL_DISASSOCIATED;

		spin_unlock_irq(&pool->lock);
		mutex_unlock(&pool->attach_mutex);

		/*
		 * Call schedule() so that we cross rq->lock and thus can
		 * guarantee sched callbacks see the %WORKER_UNBOUND flag.
		 * This is necessary as scheduler callbacks may be invoked
		 * from other cpus.
		 */
		schedule();

		/*
		 * Sched callbacks are disabled now.  Zap nr_running.
		 * After this, nr_running stays zero and need_more_worker()
		 * and keep_working() are always true as long as the
		 * worklist is not empty.  This pool now behaves as an
		 * unbound (in terms of concurrency management) pool which
		 * are served by workers tied to the pool.
		 */
		atomic_set(&pool->nr_running, 0);

		/*
		 * With concurrency management just turned off, a busy
		 * worker blocking could lead to lengthy stalls.  Kick off
		 * unbound chain execution of currently pending work items.
		 */
		spin_lock_irq(&pool->lock);
		wake_up_worker(pool);
		spin_unlock_irq(&pool->lock);
	}
}

/**
 * rebind_workers - rebind all workers of a pool to the associated CPU
 * @pool: pool of interest
 *
 * @pool->cpu is coming online.  Rebind all workers to the CPU.
 */
static void rebind_workers(struct worker_pool *pool)
{
	struct worker *worker;

	lockdep_assert_held(&pool->attach_mutex);

	/*
	 * Restore CPU affinity of all workers.  As all idle workers should
	 * be on the run-queue of the associated CPU before any local
	 * wake-ups for concurrency management happen, restore CPU affinity
	 * of all workers first and then clear UNBOUND.  As we're called
	 * from CPU_ONLINE, the following shouldn't fail.
	 */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task,
						  pool->attrs->cpumask) < 0);

	spin_lock_irq(&pool->lock);

	/*
	 * XXX: CPU hotplug notifiers are weird and can call DOWN_FAILED
	 * w/o preceding DOWN_PREPARE.  Work around it.  CPU hotplug is
	 * being reworked and this can go away in time.
	 */
	if (!(pool->flags & POOL_DISASSOCIATED)) {
		spin_unlock_irq(&pool->lock);
		return;
	}

	pool->flags &= ~POOL_DISASSOCIATED;

	for_each_pool_worker(worker, pool) {
		unsigned int worker_flags = worker->flags;

		/*
		 * A bound idle worker should actually be on the runqueue
		 * of the associated CPU for local wake-ups targeting it to
		 * work.  Kick all idle workers so that they migrate to the
		 * associated CPU.  Doing this in the same loop as
		 * replacing UNBOUND with REBOUND is safe as no worker will
		 * be bound before @pool->lock is released.
		 */
		if (worker_flags & WORKER_IDLE)
			wake_up_process(worker->task);

		/*
		 * We want to clear UNBOUND but can't directly call
		 * worker_clr_flags() or adjust nr_running.  Atomically
		 * replace UNBOUND with another NOT_RUNNING flag REBOUND.
		 * @worker will clear REBOUND using worker_clr_flags() when
		 * it initiates the next execution cycle thus restoring
		 * concurrency management.  Note that when or whether
		 * @worker clears REBOUND doesn't affect correctness.
		 *
		 * ACCESS_ONCE() is necessary because @worker->flags may be
		 * tested without holding any lock in
		 * wq_worker_waking_up().  Without it, NOT_RUNNING test may
		 * fail incorrectly leading to premature concurrency
		 * management operations.
		 */
		WARN_ON_ONCE(!(worker_flags & WORKER_UNBOUND));
		worker_flags |= WORKER_REBOUND;
		worker_flags &= ~WORKER_UNBOUND;
		ACCESS_ONCE(worker->flags) = worker_flags;
	}

	spin_unlock_irq(&pool->lock);
}

/**
 * restore_unbound_workers_cpumask - restore cpumask of unbound workers
 * @pool: unbound pool of interest
 * @cpu: the CPU which is coming up
 *
 * An unbound pool may end up with a cpumask which doesn't have any online
 * CPUs.  When a worker of such pool get scheduled, the scheduler resets
 * its cpus_allowed.  If @cpu is in @pool's cpumask which didn't have any
 * online CPU before, cpus_allowed of all its workers should be restored.
 */
static void restore_unbound_workers_cpumask(struct worker_pool *pool, int cpu)
{
	static cpumask_t cpumask;
	struct worker *worker;

	lockdep_assert_held(&pool->attach_mutex);

	/* is @cpu allowed for @pool? */
	if (!cpumask_test_cpu(cpu, pool->attrs->cpumask))
		return;

	/* is @cpu the only online CPU? */
	cpumask_and(&cpumask, pool->attrs->cpumask, cpu_online_mask);
	if (cpumask_weight(&cpumask) != 1)
		return;

	/* as we're called from CPU_ONLINE, the following shouldn't fail */
	for_each_pool_worker(worker, pool)
		WARN_ON_ONCE(set_cpus_allowed_ptr(worker->task,
						  pool->attrs->cpumask) < 0);
}

/*
 * Workqueues should be brought up before normal priority CPU notifiers.
 * This will be registered high priority CPU notifier.
 */
static int workqueue_cpu_up_callback(struct notifier_block *nfb,
					       unsigned long action,
					       void *hcpu)
{
	int cpu = (unsigned long)hcpu;
	struct worker_pool *pool;
	struct workqueue_struct *wq;
	int pi;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_UP_PREPARE:
		for_each_cpu_worker_pool(pool, cpu) {
			if (pool->nr_workers)
				continue;
			if (!create_worker(pool))
				return NOTIFY_BAD;
		}
		break;

	case CPU_DOWN_FAILED:
	case CPU_ONLINE:
		mutex_lock(&wq_pool_mutex);

		for_each_pool(pool, pi) {
			mutex_lock(&pool->attach_mutex);

			if (pool->cpu == cpu)
				rebind_workers(pool);
			else if (pool->cpu < 0)
				restore_unbound_workers_cpumask(pool, cpu);

			mutex_unlock(&pool->attach_mutex);
		}

		/* update NUMA affinity of unbound workqueues */
		list_for_each_entry(wq, &workqueues, list)
			wq_update_unbound_numa(wq, cpu, true);

		mutex_unlock(&wq_pool_mutex);
		break;
	}
	return NOTIFY_OK;
}

/*
 * Workqueues should be brought down after normal priority CPU notifiers.
 * This will be registered as low priority CPU notifier.
 */
static int workqueue_cpu_down_callback(struct notifier_block *nfb,
						 unsigned long action,
						 void *hcpu)
{
	int cpu = (unsigned long)hcpu;
	struct work_struct unbind_work;
	struct workqueue_struct *wq;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DOWN_PREPARE:
		/* unbinding per-cpu workers should happen on the local CPU */
		INIT_WORK_ONSTACK(&unbind_work, wq_unbind_fn);
		queue_work_on(cpu, system_highpri_wq, &unbind_work);

		/* update NUMA affinity of unbound workqueues */
		mutex_lock(&wq_pool_mutex);
		list_for_each_entry(wq, &workqueues, list)
			wq_update_unbound_numa(wq, cpu, false);
		mutex_unlock(&wq_pool_mutex);

		/* wait for per-cpu unbinding to finish */
		flush_work(&unbind_work);
		destroy_work_on_stack(&unbind_work);
		break;
	}
	return NOTIFY_OK;
}

#ifdef CONFIG_SMP

struct work_for_cpu {
	struct work_struct work;
	long (*fn)(void *);
	void *arg;
	long ret;
};

static void work_for_cpu_fn(struct work_struct *work)
{
	struct work_for_cpu *wfc = container_of(work, struct work_for_cpu, work);

	wfc->ret = wfc->fn(wfc->arg);
}

/**
 * work_on_cpu - run a function in user context on a particular cpu
 * @cpu: the cpu to run on
 * @fn: the function to run
 * @arg: the function arg
 *
 * It is up to the caller to ensure that the cpu doesn't go offline.
 * The caller must not hold any locks which would prevent @fn from completing.
 *
 * Return: The value @fn returns.
 */
long work_on_cpu(int cpu, long (*fn)(void *), void *arg)
{
	struct work_for_cpu wfc = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK(&wfc.work, work_for_cpu_fn);
	schedule_work_on(cpu, &wfc.work);
	flush_work(&wfc.work);
	destroy_work_on_stack(&wfc.work);
	return wfc.ret;
}
EXPORT_SYMBOL_GPL(work_on_cpu);
#endif /* CONFIG_SMP */

#ifdef CONFIG_FREEZER

/**
 * freeze_workqueues_begin - begin freezing workqueues
 *
 * Start freezing workqueues.  After this function returns, all freezable
 * workqueues will queue new works to their delayed_works list instead of
 * pool->worklist.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
void freeze_workqueues_begin(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(workqueue_freezing);
	workqueue_freezing = true;

	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

	mutex_unlock(&wq_pool_mutex);
}

/**
 * freeze_workqueues_busy - are freezable workqueues still busy?
 *
 * Check whether freezing is complete.  This function must be called
 * between freeze_workqueues_begin() and thaw_workqueues().
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex.
 *
 * Return:
 * %true if some freezable workqueues are still busy.  %false if freezing
 * is complete.
 */
/*
 * 判断一个冻结的workqueue是否仍有任务处于busy状态。
 */
bool freeze_workqueues_busy(void)
{
	bool busy = false;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	WARN_ON_ONCE(!workqueue_freezing);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_FREEZABLE))
			continue;
		/*
		 * nr_active is monotonically decreasing.  It's safe
		 * to peek without lock.
		 */
		rcu_read_lock();
		for_each_pwq(pwq, wq) {
			WARN_ON_ONCE(pwq->nr_active < 0);
			/*
			 * 被激活的work项数目大于0，则说明wq上仍有未处理的任务
			 * 置为busy状态。
			 */
			if (pwq->nr_active) {
				busy = true;
				rcu_read_unlock();
				goto out_unlock;
			}
		}
		rcu_read_unlock();
	}
out_unlock:
	mutex_unlock(&wq_pool_mutex);
	return busy;
}

/**
 * thaw_workqueues - thaw workqueues
 *
 * Thaw workqueues.  Normal queueing is restored and all collected
 * frozen works are transferred to their respective pool worklists.
 *
 * CONTEXT:
 * Grabs and releases wq_pool_mutex, wq->mutex and pool->lock's.
 */
void thaw_workqueues(void)
{
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	mutex_lock(&wq_pool_mutex);

	if (!workqueue_freezing)
		goto out_unlock;

	workqueue_freezing = false;

	/* restore max_active and repopulate worklist */
	list_for_each_entry(wq, &workqueues, list) {
		mutex_lock(&wq->mutex);
		for_each_pwq(pwq, wq)
			pwq_adjust_max_active(pwq);
		mutex_unlock(&wq->mutex);
	}

out_unlock:
	mutex_unlock(&wq_pool_mutex);
}
#endif /* CONFIG_FREEZER */

static int workqueue_apply_unbound_cpumask(void)
{
	LIST_HEAD(ctxs);
	int ret = 0;
	struct workqueue_struct *wq;
	struct apply_wqattrs_ctx *ctx, *n;

	lockdep_assert_held(&wq_pool_mutex);

	list_for_each_entry(wq, &workqueues, list) {
		if (!(wq->flags & WQ_UNBOUND))
			continue;
		/* creating multiple pwqs breaks ordering guarantee */
		if (wq->flags & __WQ_ORDERED)
			continue;

		ctx = apply_wqattrs_prepare(wq, wq->unbound_attrs);
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}

		list_add_tail(&ctx->list, &ctxs);
	}

	list_for_each_entry_safe(ctx, n, &ctxs, list) {
		if (!ret)
			apply_wqattrs_commit(ctx);
		apply_wqattrs_cleanup(ctx);
	}

	return ret;
}

/**
 *  workqueue_set_unbound_cpumask - Set the low-level unbound cpumask
 *  @cpumask: the cpumask to set
 *
 *  The low-level workqueues cpumask is a global cpumask that limits
 *  the affinity of all unbound workqueues.  This function check the @cpumask
 *  and apply it to all unbound workqueues and updates all pwqs of them.
 *
 *  Retun:	0	- Success
 *  		-EINVAL	- Invalid @cpumask
 *  		-ENOMEM	- Failed to allocate memory for attrs or pwqs.
 */
int workqueue_set_unbound_cpumask(cpumask_var_t cpumask)
{
	int ret = -EINVAL;
	cpumask_var_t saved_cpumask;

	if (!zalloc_cpumask_var(&saved_cpumask, GFP_KERNEL))
		return -ENOMEM;

	cpumask_and(cpumask, cpumask, cpu_possible_mask);
	if (!cpumask_empty(cpumask)) {
		apply_wqattrs_lock();

		/* save the old wq_unbound_cpumask. */
		cpumask_copy(saved_cpumask, wq_unbound_cpumask);

		/* update wq_unbound_cpumask at first and apply it to wqs. */
		cpumask_copy(wq_unbound_cpumask, cpumask);
		ret = workqueue_apply_unbound_cpumask();

		/* restore the wq_unbound_cpumask when failed. */
		if (ret < 0)
			cpumask_copy(wq_unbound_cpumask, saved_cpumask);

		apply_wqattrs_unlock();
	}

	free_cpumask_var(saved_cpumask);
	return ret;
}

#ifdef CONFIG_SYSFS
/*
 * Workqueues with WQ_SYSFS flag set is visible to userland via
 * /sys/bus/workqueue/devices/WQ_NAME.  All visible workqueues have the
 * following attributes.
 *
 *  per_cpu	RO bool	: whether the workqueue is per-cpu or unbound
 *  max_active	RW int	: maximum number of in-flight work items
 *
 * Unbound workqueues have the following extra attributes.
 *
 *  id		RO int	: the associated pool ID
 *  nice	RW int	: nice value of the workers
 *  cpumask	RW mask	: bitmask of allowed CPUs for the workers
 */
struct wq_device {
	struct workqueue_struct		*wq;
	struct device			dev;
};

static struct workqueue_struct *dev_to_wq(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	return wq_dev->wq;
}

static ssize_t per_cpu_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", (bool)!(wq->flags & WQ_UNBOUND));
}
static DEVICE_ATTR_RO(per_cpu);

static ssize_t max_active_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", wq->saved_max_active);
}

static ssize_t max_active_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int val;

	if (sscanf(buf, "%d", &val) != 1 || val <= 0)
		return -EINVAL;

	workqueue_set_max_active(wq, val);
	return count;
}
static DEVICE_ATTR_RW(max_active);

static struct attribute *wq_sysfs_attrs[] = {
	&dev_attr_per_cpu.attr,
	&dev_attr_max_active.attr,
	NULL,
};
ATTRIBUTE_GROUPS(wq_sysfs);

static ssize_t wq_pool_ids_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	const char *delim = "";
	int node, written = 0;

	get_online_cpus();
	rcu_read_lock();
	for_each_node(node) {
		written += scnprintf(buf + written, PAGE_SIZE - written,
				     "%s%d:%d", delim, node,
				     unbound_pwq_by_node(wq, node)->pool->id);
		delim = " ";
	}
	written += scnprintf(buf + written, PAGE_SIZE - written, "\n");
	rcu_read_unlock();
	put_online_cpus();

	return written;
}

static ssize_t wq_nice_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n", wq->unbound_attrs->nice);
	mutex_unlock(&wq->mutex);

	return written;
}

/* prepare workqueue_attrs for sysfs store operations */
static struct workqueue_attrs *wq_sysfs_prep_attrs(struct workqueue_struct *wq)
{
	struct workqueue_attrs *attrs;

	lockdep_assert_held(&wq_pool_mutex);

	attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if (!attrs)
		return NULL;

	copy_workqueue_attrs(attrs, wq->unbound_attrs);
	return attrs;
}

static ssize_t wq_nice_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	if (sscanf(buf, "%d", &attrs->nice) == 1 &&
	    attrs->nice >= MIN_NICE && attrs->nice <= MAX_NICE)
		ret = apply_workqueue_attrs_locked(wq, attrs);
	else
		ret = -EINVAL;

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_cpumask_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq->unbound_attrs->cpumask));
	mutex_unlock(&wq->mutex);
	return written;
}

static ssize_t wq_cpumask_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = cpumask_parse(buf, attrs->cpumask);
	if (!ret)
		ret = apply_workqueue_attrs_locked(wq, attrs);

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static ssize_t wq_numa_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	int written;

	mutex_lock(&wq->mutex);
	written = scnprintf(buf, PAGE_SIZE, "%d\n",
			    !wq->unbound_attrs->no_numa);
	mutex_unlock(&wq->mutex);

	return written;
}

static ssize_t wq_numa_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct workqueue_struct *wq = dev_to_wq(dev);
	struct workqueue_attrs *attrs;
	int v, ret = -ENOMEM;

	apply_wqattrs_lock();

	attrs = wq_sysfs_prep_attrs(wq);
	if (!attrs)
		goto out_unlock;

	ret = -EINVAL;
	if (sscanf(buf, "%d", &v) == 1) {
		attrs->no_numa = !v;
		ret = apply_workqueue_attrs_locked(wq, attrs);
	}

out_unlock:
	apply_wqattrs_unlock();
	free_workqueue_attrs(attrs);
	return ret ?: count;
}

static struct device_attribute wq_sysfs_unbound_attrs[] = {
	__ATTR(pool_ids, 0444, wq_pool_ids_show, NULL),
	__ATTR(nice, 0644, wq_nice_show, wq_nice_store),
	__ATTR(cpumask, 0644, wq_cpumask_show, wq_cpumask_store),
	__ATTR(numa, 0644, wq_numa_show, wq_numa_store),
	__ATTR_NULL,
};

static struct bus_type wq_subsys = {
	.name				= "workqueue",
	.dev_groups			= wq_sysfs_groups,
};

static ssize_t wq_unbound_cpumask_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int written;

	mutex_lock(&wq_pool_mutex);
	written = scnprintf(buf, PAGE_SIZE, "%*pb\n",
			    cpumask_pr_args(wq_unbound_cpumask));
	mutex_unlock(&wq_pool_mutex);

	return written;
}

static ssize_t wq_unbound_cpumask_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	cpumask_var_t cpumask;
	int ret;

	if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parse(buf, cpumask);
	if (!ret)
		ret = workqueue_set_unbound_cpumask(cpumask);

	free_cpumask_var(cpumask);
	return ret ? ret : count;
}

static struct device_attribute wq_sysfs_cpumask_attr =
	__ATTR(cpumask, 0644, wq_unbound_cpumask_show,
	       wq_unbound_cpumask_store);

static int __init wq_sysfs_init(void)
{
	int err;

	err = subsys_virtual_register(&wq_subsys, NULL);
	if (err)
		return err;

	return device_create_file(wq_subsys.dev_root, &wq_sysfs_cpumask_attr);
}
core_initcall(wq_sysfs_init);

static void wq_device_release(struct device *dev)
{
	struct wq_device *wq_dev = container_of(dev, struct wq_device, dev);

	kfree(wq_dev);
}

/**
 * workqueue_sysfs_register - make a workqueue visible in sysfs
 * @wq: the workqueue to register
 *
 * Expose @wq in sysfs under /sys/bus/workqueue/devices.
 * alloc_workqueue*() automatically calls this function if WQ_SYSFS is set
 * which is the preferred method.
 *
 * Workqueue user should use this function directly iff it wants to apply
 * workqueue_attrs before making the workqueue visible in sysfs; otherwise,
 * apply_workqueue_attrs() may race against userland updating the
 * attributes.
 *
 * Return: 0 on success, -errno on failure.
 */
int workqueue_sysfs_register(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev;
	int ret;

	/*
	 * Adjusting max_active or creating new pwqs by applying
	 * attributes breaks ordering guarantee.  Disallow exposing ordered
	 * workqueues.
	 */
	if (WARN_ON(wq->flags & __WQ_ORDERED_EXPLICIT))
		return -EINVAL;

	wq->wq_dev = wq_dev = kzalloc(sizeof(*wq_dev), GFP_KERNEL);
	if (!wq_dev)
		return -ENOMEM;

	wq_dev->wq = wq;
	wq_dev->dev.bus = &wq_subsys;
	wq_dev->dev.init_name = wq->name;
	wq_dev->dev.release = wq_device_release;

	/*
	 * unbound_attrs are created separately.  Suppress uevent until
	 * everything is ready.
	 */
	dev_set_uevent_suppress(&wq_dev->dev, true);

	ret = device_register(&wq_dev->dev);
	if (ret) {
		put_device(&wq_dev->dev);
		wq->wq_dev = NULL;
		return ret;
	}

	if (wq->flags & WQ_UNBOUND) {
		struct device_attribute *attr;

		for (attr = wq_sysfs_unbound_attrs; attr->attr.name; attr++) {
			ret = device_create_file(&wq_dev->dev, attr);
			if (ret) {
				device_unregister(&wq_dev->dev);
				wq->wq_dev = NULL;
				return ret;
			}
		}
	}

	dev_set_uevent_suppress(&wq_dev->dev, false);
	kobject_uevent(&wq_dev->dev.kobj, KOBJ_ADD);
	return 0;
}

/**
 * workqueue_sysfs_unregister - undo workqueue_sysfs_register()
 * @wq: the workqueue to unregister
 *
 * If @wq is registered to sysfs by workqueue_sysfs_register(), unregister.
 */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)
{
	struct wq_device *wq_dev = wq->wq_dev;

	if (!wq->wq_dev)
		return;

	wq->wq_dev = NULL;
	device_unregister(&wq_dev->dev);
}
#else	/* CONFIG_SYSFS */
static void workqueue_sysfs_unregister(struct workqueue_struct *wq)	{ }
#endif	/* CONFIG_SYSFS */

static void __init wq_numa_init(void)
{
	cpumask_var_t *tbl;
	int node, cpu;

	if (num_possible_nodes() <= 1)
		return;

	if (wq_disable_numa) {
		pr_info("workqueue: NUMA affinity support disabled\n");
		return;
	}

	wq_update_unbound_numa_attrs_buf = alloc_workqueue_attrs(GFP_KERNEL);
	BUG_ON(!wq_update_unbound_numa_attrs_buf);

	/*
	 * We want masks of possible CPUs of each node which isn't readily
	 * available.  Build one from cpu_to_node() which should have been
	 * fully initialized by now.
	 */
	tbl = kzalloc(nr_node_ids * sizeof(tbl[0]), GFP_KERNEL);
	BUG_ON(!tbl);

	for_each_node(node)
		BUG_ON(!zalloc_cpumask_var_node(&tbl[node], GFP_KERNEL,
				node_online(node) ? node : NUMA_NO_NODE));

	for_each_possible_cpu(cpu) {
		node = cpu_to_node(cpu);
		if (WARN_ON(node == NUMA_NO_NODE)) {
			pr_warn("workqueue: NUMA node mapping not available for cpu%d, disabling NUMA support\n", cpu);
			/* happens iff arch is bonkers, let's just proceed */
			return;
		}
		cpumask_set_cpu(cpu, tbl[node]);
	}

	wq_numa_possible_cpumask = tbl;
	wq_numa_enabled = true;
}

static int __init init_workqueues(void)
{
	int std_nice[NR_STD_WORKER_POOLS] = { 0, HIGHPRI_NICE_LEVEL };
	int i, cpu;

	WARN_ON(__alignof__(struct pool_workqueue) < __alignof__(long long));

	BUG_ON(!alloc_cpumask_var(&wq_unbound_cpumask, GFP_KERNEL));
	cpumask_copy(wq_unbound_cpumask, cpu_possible_mask);

	pwq_cache = KMEM_CACHE(pool_workqueue, SLAB_PANIC);

	cpu_notifier(workqueue_cpu_up_callback, CPU_PRI_WORKQUEUE_UP);
	hotcpu_notifier(workqueue_cpu_down_callback, CPU_PRI_WORKQUEUE_DOWN);

	wq_numa_init();

	/* initialize CPU pools */
	for_each_possible_cpu(cpu) {
		struct worker_pool *pool;

		i = 0;
		for_each_cpu_worker_pool(pool, cpu) {
			BUG_ON(init_worker_pool(pool));
			pool->cpu = cpu;
			cpumask_copy(pool->attrs->cpumask, cpumask_of(cpu));
			pool->attrs->nice = std_nice[i++];
			pool->node = cpu_to_node(cpu);

			/* alloc pool ID */
			mutex_lock(&wq_pool_mutex);
			BUG_ON(worker_pool_assign_id(pool));
			mutex_unlock(&wq_pool_mutex);
		}
	}

	/* create the initial worker */
	for_each_online_cpu(cpu) {
		struct worker_pool *pool;

		for_each_cpu_worker_pool(pool, cpu) {
			pool->flags &= ~POOL_DISASSOCIATED;
			BUG_ON(!create_worker(pool));
		}
	}

	/* create default unbound and ordered wq attrs */
	for (i = 0; i < NR_STD_WORKER_POOLS; i++) {
		struct workqueue_attrs *attrs;

		BUG_ON(!(attrs = alloc_workqueue_attrs(GFP_KERNEL)));
		attrs->nice = std_nice[i];
		unbound_std_wq_attrs[i] = attrs;

		/*
		 * An ordered wq should have only one pwq as ordering is
		 * guaranteed by max_active which is enforced by pwqs.
		 * Turn off NUMA so that dfl_pwq is used for all nodes.
		 */
		BUG_ON(!(attrs = alloc_workqueue_attrs(GFP_KERNEL)));
		attrs->nice = std_nice[i];
		attrs->no_numa = true;
		ordered_wq_attrs[i] = attrs;
	}

	system_wq = alloc_workqueue("events", 0, 0);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND,
					    WQ_UNBOUND_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE, 0);
	system_power_efficient_wq = alloc_workqueue("events_power_efficient",
					      WQ_POWER_EFFICIENT, 0);
	system_freezable_power_efficient_wq = alloc_workqueue("events_freezable_power_efficient",
					      WQ_FREEZABLE | WQ_POWER_EFFICIENT,
					      0);
	BUG_ON(!system_wq || !system_highpri_wq || !system_long_wq ||
	       !system_unbound_wq || !system_freezable_wq ||
	       !system_power_efficient_wq ||
	       !system_freezable_power_efficient_wq);
	return 0;
}
early_initcall(init_workqueues);
