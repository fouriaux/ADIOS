
/*
  adios_icee.c
  uses evpath for io in conjunction with read/read_icee.c
*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>
#include <signal.h>
//#include <mpi.h>

// xml parser
#include <mxml.h>

// add by Kimmy 10/15/2012
#include <sys/types.h>
#include <sys/stat.h>
// end of change

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "public/adios.h"
#include "public/adios_mpi.h"
#include "public/adios_error.h"
#include "core/adios_transport_hooks.h"
#include "core/adios_bp_v1.h"
#include "core/adios_internals.h"
#include "core/buffer.h"
#include "core/util.h"
#include "core/adios_logger.h"

#ifdef HAVE_ICEE

// // evpath libraries
#include <evpath.h>
#include <cod.h>
#include <sys/queue.h>

///////////////////////////
// Global Variables
///////////////////////////
#include "core/adios_icee.h"

#define DUMP(fmt, ...) fprintf(stderr, ">>> "fmt"\n", ## __VA_ARGS__); 

static int adios_icee_initialized = 0;
static int icee_num_parallel = 0;

CManager icee_write_cm;
EVsource icee_write_source;

int n_client = 0;
int max_client = 1;
icee_clientinfo_rec_t *client_info;

icee_fileinfo_rec_ptr_t fp = NULL;
int reverse_dim = 0;

int timestep = 0; // global timestep. Will be increased by 1 at each adios_open

/*
 * Thread pool implementation
 * Credit: Multithreaded Programming Guide by Oracle
 * http://docs.oracle.com/cd/E19253-01/816-5137/6mba5vqn3/index.html
 */

/*
 * FIFO queued job
 */
typedef struct job job_t;
struct job {
	job_t	*job_next;		/* linked list of jobs */
	void	*(*job_func)(void *);	/* function to call */
	void	*job_arg;		/* its argument */
};

/*
 * List of active worker threads, linked through their stacks.
 */
typedef struct active active_t;
struct active {
	active_t	*active_next;	/* linked list of threads */
	pthread_t	active_tid;	/* active thread id */
};

/*
 * The thread pool, opaque to the clients.
 */
struct thr_pool {
	thr_pool_t	*pool_forw;	/* circular linked list */
	thr_pool_t	*pool_back;	/* of all thread pools */
	pthread_mutex_t	pool_mutex;	/* protects the pool data */
	pthread_cond_t	pool_busycv;	/* synchronization in pool_queue */
	pthread_cond_t	pool_workcv;	/* synchronization with workers */
	pthread_cond_t	pool_waitcv;	/* synchronization in pool_wait() */
	active_t	*pool_active;	/* list of threads performing work */
	job_t		*pool_head;	/* head of FIFO job queue */
	job_t		*pool_tail;	/* tail of FIFO job queue */
	pthread_attr_t	pool_attr;	/* attributes of the workers */
	int		pool_flags;	/* see below */
	uint_t		pool_linger;	/* seconds before idle workers exit */
	int		pool_minimum;	/* minimum number of worker threads */
	int		pool_maximum;	/* maximum number of worker threads */
	int		pool_nthreads;	/* current number of worker threads */
	int		pool_idle;	/* number of idle workers */
};

/* pool_flags */
#define	POOL_WAIT	0x01		/* waiting in thr_pool_wait() */
#define	POOL_DESTROY	0x02		/* pool is being destroyed */

/* the list of all created and not yet destroyed thread pools */
static thr_pool_t *thr_pools = NULL;

/* protects thr_pools */
static pthread_mutex_t thr_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* set of all signals */
static sigset_t fillset;

static void *worker_thread(void *);

static int
create_worker(thr_pool_t *pool)
{
	sigset_t oset;
	int error;
    pthread_t thrid;

	(void) pthread_sigmask(SIG_SETMASK, &fillset, &oset);
	error = pthread_create(&thrid, &pool->pool_attr, worker_thread, pool);
	(void) pthread_sigmask(SIG_SETMASK, &oset, NULL);
	return (error);
}

/*
 * Worker thread is terminating.  Possible reasons:
 * - excess idle thread is terminating because there is no work.
 * - thread was cancelled (pool is being destroyed).
 * - the job function called pthread_exit().
 * In the last case, create another worker thread
 * if necessary to keep the pool populated.
 */
static void
worker_cleanup(thr_pool_t *pool)
{
	--pool->pool_nthreads;
	if (pool->pool_flags & POOL_DESTROY) {
		if (pool->pool_nthreads == 0)
			(void) pthread_cond_broadcast(&pool->pool_busycv);
	} else if (pool->pool_head != NULL &&
               pool->pool_nthreads < pool->pool_maximum &&
               create_worker(pool) == 0) {
		pool->pool_nthreads++;
	}
	(void) pthread_mutex_unlock(&pool->pool_mutex);
}

static void
notify_waiters(thr_pool_t *pool)
{
	if (pool->pool_head == NULL && pool->pool_active == NULL) {
		pool->pool_flags &= ~POOL_WAIT;
		(void) pthread_cond_broadcast(&pool->pool_waitcv);
	}
}

/*
 * Called by a worker thread on return from a job.
 */
static void
job_cleanup(thr_pool_t *pool)
{
	pthread_t my_tid = pthread_self();
	active_t *activep;
	active_t **activepp;

	(void) pthread_mutex_lock(&pool->pool_mutex);
	for (activepp = &pool->pool_active;
         (activep = *activepp) != NULL;
         activepp = &activep->active_next) {
		if (activep->active_tid == my_tid) {
			*activepp = activep->active_next;
			break;
		}
	}
	if (pool->pool_flags & POOL_WAIT)
		notify_waiters(pool);
}

static void *
worker_thread(void *arg)
{
	thr_pool_t *pool = (thr_pool_t *)arg;
	int timedout;
	job_t *job;
	void *(*func)(void *);
	active_t active;
	struct timespec ts;

	/*
	 * This is the worker's main loop.  It will only be left
	 * if a timeout occurs or if the pool is being destroyed.
	 */
	(void) pthread_mutex_lock(&pool->pool_mutex);
	pthread_cleanup_push((void *)worker_cleanup, pool);
	active.active_tid = pthread_self();
	for (;;) {
		/*
		 * We don't know what this thread was doing during
		 * its last job, so we reset its signal mask and
		 * cancellation state back to the initial values.
		 */
		(void) pthread_sigmask(SIG_SETMASK, &fillset, NULL);
		(void) pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
		(void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		timedout = 0;
		pool->pool_idle++;
		if (pool->pool_flags & POOL_WAIT)
			notify_waiters(pool);
		while (pool->pool_head == NULL &&
               !(pool->pool_flags & POOL_DESTROY)) {
			if (pool->pool_nthreads <= pool->pool_minimum) {
				(void) pthread_cond_wait(&pool->pool_workcv,
                                         &pool->pool_mutex);
			} else {
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
                clock_serv_t cclock;
                mach_timespec_t mts;
                host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
                clock_get_time(cclock, &mts);
                mach_port_deallocate(mach_task_self(), cclock);
                ts.tv_sec = mts.tv_sec;
                ts.tv_nsec = mts.tv_nsec;
#else
				(void) clock_gettime(CLOCK_REALTIME, &ts);
#endif
				ts.tv_sec += pool->pool_linger;
				if (pool->pool_linger == 0 ||
				    pthread_cond_timedwait(&pool->pool_workcv,
                                           &pool->pool_mutex, &ts) == ETIMEDOUT) {
					timedout = 1;
					break;
				}
			}
		}
		pool->pool_idle--;
		if (pool->pool_flags & POOL_DESTROY)
			break;
		if ((job = pool->pool_head) != NULL) {
			timedout = 0;
			func = job->job_func;
			arg = job->job_arg;
			pool->pool_head = job->job_next;
			if (job == pool->pool_tail)
				pool->pool_tail = NULL;
			active.active_next = pool->pool_active;
			pool->pool_active = &active;
			(void) pthread_mutex_unlock(&pool->pool_mutex);
			pthread_cleanup_push((void *)job_cleanup, pool);
			free(job);
			/*
			 * Call the specified job function.
			 */
			(void) func(arg);
			/*
			 * If the job function calls pthread_exit(), the thread
			 * calls job_cleanup(pool) and worker_cleanup(pool);
			 * the integrity of the pool is thereby maintained.
			 */
			pthread_cleanup_pop(1);	/* job_cleanup(pool) */
		}
		if (timedout && pool->pool_nthreads > pool->pool_minimum) {
			/*
			 * We timed out and there is no work to be done
			 * and the number of workers exceeds the minimum.
			 * Exit now to reduce the size of the pool.
			 */
			break;
		}
	}
	pthread_cleanup_pop(1);	/* worker_cleanup(pool) */
	return (NULL);
}

static void
clone_attributes(pthread_attr_t *new_attr, pthread_attr_t *old_attr)
{
	struct sched_param param;
	void *addr;
	size_t size;
	int value;

	(void) pthread_attr_init(new_attr);

	if (old_attr != NULL) {
		(void) pthread_attr_getstack(old_attr, &addr, &size);
		/* don't allow a non-NULL thread stack address */
		(void) pthread_attr_setstack(new_attr, NULL, size);

		(void) pthread_attr_getscope(old_attr, &value);
		(void) pthread_attr_setscope(new_attr, value);

		(void) pthread_attr_getinheritsched(old_attr, &value);
		(void) pthread_attr_setinheritsched(new_attr, value);

		(void) pthread_attr_getschedpolicy(old_attr, &value);
		(void) pthread_attr_setschedpolicy(new_attr, value);

		(void) pthread_attr_getschedparam(old_attr, &param);
		(void) pthread_attr_setschedparam(new_attr, &param);

		(void) pthread_attr_getguardsize(old_attr, &size);
		(void) pthread_attr_setguardsize(new_attr, size);
	}

	/* make all pool threads be detached threads */
	(void) pthread_attr_setdetachstate(new_attr, PTHREAD_CREATE_DETACHED);
}

thr_pool_t *
thr_pool_create(uint_t min_threads, uint_t max_threads, uint_t linger,
                pthread_attr_t *attr)
{
	thr_pool_t	*pool;

	(void) sigfillset(&fillset);

	if (min_threads > max_threads || max_threads < 1) {
		errno = EINVAL;
		return (NULL);
	}

	if ((pool = malloc(sizeof (*pool))) == NULL) {
		errno = ENOMEM;
		return (NULL);
	}
	(void) pthread_mutex_init(&pool->pool_mutex, NULL);
	(void) pthread_cond_init(&pool->pool_busycv, NULL);
	(void) pthread_cond_init(&pool->pool_workcv, NULL);
	(void) pthread_cond_init(&pool->pool_waitcv, NULL);
	pool->pool_active = NULL;
	pool->pool_head = NULL;
	pool->pool_tail = NULL;
	pool->pool_flags = 0;
	pool->pool_linger = linger;
	pool->pool_minimum = min_threads;
	pool->pool_maximum = max_threads;
	pool->pool_nthreads = 0;
	pool->pool_idle = 0;

	/*
	 * We cannot just copy the attribute pointer.
	 * We need to initialize a new pthread_attr_t structure using
	 * the values from the caller-supplied attribute structure.
	 * If the attribute pointer is NULL, we need to initialize
	 * the new pthread_attr_t structure with default values.
	 */
	clone_attributes(&pool->pool_attr, attr);

	/* insert into the global list of all thread pools */
	(void) pthread_mutex_lock(&thr_pool_lock);
	if (thr_pools == NULL) {
		pool->pool_forw = pool;
		pool->pool_back = pool;
		thr_pools = pool;
	} else {
		thr_pools->pool_back->pool_forw = pool;
		pool->pool_forw = thr_pools;
		pool->pool_back = thr_pools->pool_back;
		thr_pools->pool_back = pool;
	}
	(void) pthread_mutex_unlock(&thr_pool_lock);

	return (pool);
}

int
thr_pool_queue(thr_pool_t *pool, void *(*func)(void *), void *arg)
{
	job_t *job;

	if ((job = malloc(sizeof (*job))) == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	job->job_next = NULL;
	job->job_func = func;
	job->job_arg = arg;

	(void) pthread_mutex_lock(&pool->pool_mutex);

	if (pool->pool_head == NULL)
		pool->pool_head = job;
	else
		pool->pool_tail->job_next = job;
	pool->pool_tail = job;

	if (pool->pool_idle > 0)
		(void) pthread_cond_signal(&pool->pool_workcv);
	else if (pool->pool_nthreads < pool->pool_maximum &&
             create_worker(pool) == 0)
		pool->pool_nthreads++;

	(void) pthread_mutex_unlock(&pool->pool_mutex);
	return (0);
}

void
thr_pool_wait(thr_pool_t *pool)
{
	(void) pthread_mutex_lock(&pool->pool_mutex);
	pthread_cleanup_push((void *)pthread_mutex_unlock, &pool->pool_mutex);
	while (pool->pool_head != NULL || pool->pool_active != NULL) {
		pool->pool_flags |= POOL_WAIT;
		(void) pthread_cond_wait(&pool->pool_waitcv, &pool->pool_mutex);
	}
	pthread_cleanup_pop(1);	/* pthread_mutex_unlock(&pool->pool_mutex); */
}

void
thr_pool_destroy(thr_pool_t *pool)
{
	active_t *activep;
	job_t *job;

	(void) pthread_mutex_lock(&pool->pool_mutex);
	pthread_cleanup_push((void *)pthread_mutex_unlock, &pool->pool_mutex);

	/* mark the pool as being destroyed; wakeup idle workers */
	pool->pool_flags |= POOL_DESTROY;
	(void) pthread_cond_broadcast(&pool->pool_workcv);

	/* cancel all active workers */
	for (activep = pool->pool_active;
         activep != NULL;
         activep = activep->active_next)
		(void) pthread_cancel(activep->active_tid);

	/* wait for all active workers to finish */
	while (pool->pool_active != NULL) {
		pool->pool_flags |= POOL_WAIT;
		(void) pthread_cond_wait(&pool->pool_waitcv, &pool->pool_mutex);
	}

	/* the last worker to terminate will wake us up */
	while (pool->pool_nthreads != 0)
		(void) pthread_cond_wait(&pool->pool_busycv, &pool->pool_mutex);

	pthread_cleanup_pop(1);	/* pthread_mutex_unlock(&pool->pool_mutex); */

	/*
	 * Unlink the pool from the global list of all pools.
	 */
	(void) pthread_mutex_lock(&thr_pool_lock);
	if (thr_pools == pool)
		thr_pools = pool->pool_forw;
	if (thr_pools == pool)
		thr_pools = NULL;
	else {
		pool->pool_back->pool_forw = pool->pool_forw;
		pool->pool_forw->pool_back = pool->pool_back;
	}
	(void) pthread_mutex_unlock(&thr_pool_lock);

	/*
	 * There should be no pending jobs, but just in case...
	 */
	for (job = pool->pool_head; job != NULL; job = pool->pool_head) {
		pool->pool_head = job->job_next;
		free(job);
	}
	(void) pthread_attr_destroy(&pool->pool_attr);
	free(pool);
}


/*
 *  ICEE
 */

thr_pool_t *icee_pool;

static int
icee_clientinfo_handler(CManager cm, void *vevent, void *client_data, attr_list attrs)
{
    log_debug ("%s\n", __FUNCTION__);

    icee_clientinfo_rec_ptr_t event = vevent;
    log_debug ("%s (%s)\n", "client_host", event->client_host);
    log_debug ("%s (%d)\n", "client_port", event->client_port);
    log_debug ("%s (%d)\n", "stone_id", event->stone_id);

    client_info[n_client].client_host = strdup(event->client_host);
    client_info[n_client].client_port = event->client_port;
    client_info[n_client].stone_id = event->stone_id;
    n_client++;

    return 1;
}

void *dosubmit(icee_fileinfo_rec_t *fp)  
{
    if (adios_verbose_level > 3) 
        DUMP("threadid is %lu, submitting %d(%s)", 
             (unsigned long)pthread_self(), fp->varinfo->varid, fp->varinfo->varname);

    EVsubmit(icee_write_source, fp, NULL);
    
    icee_varinfo_rec_ptr_t vp = fp->varinfo;
    free(vp->varname);
    free(vp->gdims);
    free(vp->ldims);
    free(vp->offsets);
    free(fp);
    
    return NULL;  
}  

// Initializes icee write local data structures
extern void 
adios_icee_init(const PairStruct *params, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    int cm_port = 59999;
    char *cm_host = "localhost";
    char *cm_attr = NULL;

    int rank;
    MPI_Comm_rank(method->init_comm, &rank);
    log_debug ("rank : %d\n", rank);
    

    const PairStruct * p = params;

    while (p)
    {
        if (!strcasecmp (p->name, "cm_attr"))
        {
            cm_attr = p->value;
        }
        else if (!strcasecmp (p->name, "cm_host"))
        {
            cm_host = p->value;
        }
        else if (!strcasecmp (p->name, "cm_port"))
        {
            cm_port = atoi(p->value);
        }
        else if (!strcasecmp (p->name, "cm_list"))
        {
            char **plist;
            int plen = 8;

            plist = malloc(plen * sizeof(char *));

            char* token = strtok(p->value, ",");
            int len = 0;
            while (token) 
            {
                plist[len] = token;

                token = strtok(NULL, ",");
                len++;

                if (len > plen)
                {
                    plen = plen*2;
                    realloc (plist, plen * sizeof(char *));
                }
            }

            char *myparam = plist[rank % len];
            token = strtok(myparam, ":");

            if (myparam[0] == ':')
            {
                cm_port = atoi(token);
            }
            else
            {
                cm_host = token;
                token = strtok(NULL, ":");
                cm_port = atoi(token);
            }

            free(plist);
        }
        else if (!strcasecmp (p->name, "reverse_dim"))
        {
            reverse_dim = 1;
        }
        else if (!strcasecmp (p->name, "max_client"))
        {
            max_client = atoi(p->value);
        }
        else if (!strcasecmp (p->name, "num_parallel"))
        {
            icee_num_parallel = atoi(p->value);
        }

        p = p->next;
    }

    //log_info ("cm_attr : %s\n", cm_attr);
    //log_info ("cm_host : %s\n", cm_host);
    log_info ("cm_port : %d\n", cm_port);
    log_debug ("parallel writing : %d\n", icee_num_parallel);

    if (!adios_icee_initialized)
    {

        // Init parallel
        if (icee_num_parallel > 1)
        {
            pthread_attr_t attr;  
            pthread_attr_init(&attr);  
            pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);  
            icee_pool = thr_pool_create(icee_num_parallel,icee_num_parallel,10,NULL);  
        }

        EVstone stone, remote_stone;
        attr_list contact_list;

        icee_write_cm = CManager_create();
        CMlisten(icee_write_cm);

        contact_list = create_attr_list();
        add_int_attr(contact_list, attr_atom_from_string("IP_PORT"), cm_port);

        if (CMlisten_specific(icee_write_cm, contact_list) == 0) 
        {
            fprintf(stderr, "error: unable to initialize connection manager.\n");
            exit(-1);
        }

        log_debug("Contact list \"%s\"\n", attr_list_to_string(contact_list));

        stone = EValloc_stone(icee_write_cm);
        log_debug("Stone ID: %d\n", stone);
        EVassoc_terminal_action(icee_write_cm, stone, icee_clientinfo_format_list, icee_clientinfo_handler, NULL);

        client_info = calloc(max_client, sizeof(icee_clientinfo_rec_t));

        while (n_client < max_client) {
            /* do some work here */
            usleep(0.1*1E7);
            CMpoll_network(icee_write_cm);
            log_debug("Num. of client: %d\n", n_client);
        }

        EVstone split_stone;
        EVaction split_action;
        split_stone = EValloc_stone(icee_write_cm);
        split_action = EVassoc_split_action(icee_write_cm, split_stone, NULL);

        int i;
        for (i=0; i<max_client; i++)
        {
            remote_stone = client_info[i].stone_id;
            stone = EValloc_stone(icee_write_cm);
            contact_list = create_attr_list();
            add_int_attr(contact_list, attr_atom_from_string("IP_PORT"), client_info[i].client_port);
            add_string_attr(contact_list, attr_atom_from_string("IP_HOST"), client_info[i].client_host);

            EVaction evaction = EVassoc_bridge_action(icee_write_cm, stone, contact_list, remote_stone);
            if (evaction == -1)
            {
                fprintf(stderr, "No connection. Exit.\n");
                exit(1);
            }

            EVaction_add_split_target(icee_write_cm, split_stone, split_action, stone);

        }
        icee_write_source = EVcreate_submit_handle(icee_write_cm, split_stone, icee_fileinfo_format_list);

        adios_icee_initialized = 1;
    }
}

extern int 
adios_icee_open(struct adios_file_struct *fd, 
                struct adios_method_struct *method, 
                MPI_Comm comm) 
{    
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
        return -1;
    }

    if (fp == NULL) fp = calloc(1, sizeof(icee_fileinfo_rec_t));
    
    fp->fname = fd->name;
    MPI_Comm_size(comm, &(fp->comm_size));
    MPI_Comm_rank(comm, &(fp->comm_rank));
    fp->timestep = timestep++;

    return 0;	
}

//  writes data to multiqueue
extern void
adios_icee_write(
    struct adios_file_struct *fd, 
    struct adios_var_struct *f, 
    void *data, 
    struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
    }

    icee_varinfo_rec_ptr_t vp = fp->varinfo;
    icee_varinfo_rec_ptr_t prev = NULL;

    while (vp != NULL)
    {
        prev = vp;
        vp = vp->next;
    }

    vp = calloc(1, sizeof(icee_varinfo_rec_t));

    if (prev == NULL)
        fp->varinfo = vp;
    else
        prev->next = vp;

    if (f->path[0] == '\0')
        vp->varname = strdup(f->name);
    else
    {
        char buff[80];
        sprintf(buff, "%s/%s", f->path, f->name);
        vp->varname = strdup(buff);
    }

    vp->varid = f->id;
    if (adios_verbose_level > 3) DUMP("id,name = %d,%s", vp->varid, vp->varname);
    vp->type = f->type;
    vp->typesize = adios_get_type_size(f->type, ""); 

    vp->ndims = count_dimensions(f->dimensions);

    vp->varlen = vp->typesize;
    if (vp->ndims > 0)
    {
        vp->gdims = calloc(vp->ndims, sizeof(uint64_t));
        vp->ldims = calloc(vp->ndims, sizeof(uint64_t));
        vp->offsets = calloc(vp->ndims, sizeof(uint64_t));
        
        struct adios_dimension_struct *d = f->dimensions;
        // Default: Fortran. 
        if (reverse_dim)
        {
            int i;
            for (i = vp->ndims-1; i >= 0; --i)
            {
                vp->gdims[i] = adios_get_dim_value(&d->global_dimension);
                vp->ldims[i] = adios_get_dim_value(&d->dimension);
                vp->offsets[i] = adios_get_dim_value(&d->local_offset);
                
                vp->varlen *= vp->ldims[i];
                
                d = d->next;
            }
        }
        else
        {
            int i;
            for (i = 0; i < vp->ndims; ++i)
            {
                vp->gdims[i] = adios_get_dim_value(&d->global_dimension);
                vp->ldims[i] = adios_get_dim_value(&d->dimension);
                vp->offsets[i] = adios_get_dim_value(&d->local_offset);
                
                vp->varlen *= vp->ldims[i];
                
                d = d->next;
            }
        }
    }
    
    vp->data = f->data;

    fp->nvars++;
}

extern void 
adios_icee_close(struct adios_file_struct *fd, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if( fd == NULL || method == NULL) {
        perror("open: Bad input parameters\n");
    }

    // Write data to the network
    if (icee_num_parallel > 1)
    {
        icee_varinfo_rec_ptr_t vp = fp->varinfo;
        icee_varinfo_rec_ptr_t prev = NULL;

        int comm_nvars = 0;
        MPI_Allreduce(&fp->nvars, &comm_nvars, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        while (vp != NULL)
        {
            prev = vp;
            vp = vp->next;

            icee_fileinfo_rec_ptr_t p = malloc(sizeof(icee_fileinfo_rec_t));
            memcpy(p, fp, sizeof(icee_fileinfo_rec_t));

            p->nchunks = comm_nvars;
            p->varinfo = prev;
            prev->next = NULL;

            thr_pool_queue(icee_pool, (void*)dosubmit, (void*) p);
        }

        thr_pool_wait(icee_pool);
    }
    else
    {
        fp->nchunks = fp->comm_size;
        EVsubmit(icee_write_source, fp, NULL);

        // Free
        icee_varinfo_rec_ptr_t vp = fp->varinfo;
        while (vp != NULL)
        {
            free(vp->varname);
            free(vp->gdims);
            free(vp->ldims);
            free(vp->offsets);
            
            icee_varinfo_rec_ptr_t prev = vp;
            vp = vp->next;
            
            free(prev);
        }
    }

    free(fp);
    fp = NULL;
}

// wait until all open files have finished sending data to shutdown
extern void 
adios_icee_finalize(int mype, struct adios_method_struct *method) 
{
    log_debug ("%s\n", __FUNCTION__);

    if (adios_icee_initialized)
    {
        CManager_close(icee_write_cm);
        adios_icee_initialized = 0;
    }
}

// provides unknown functionality
extern enum ADIOS_FLAG 
adios_icee_should_buffer (struct adios_file_struct * fd,struct adios_method_struct * method) 
{
    return adios_flag_no;
}

// provides unknown functionality
extern void 
adios_icee_end_iteration(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_start_calculation(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_stop_calculation(struct adios_method_struct *method) 
{
}

// provides unknown functionality
extern void 
adios_icee_get_write_buffer(struct adios_file_struct *fd, 
                            struct adios_var_struct *v, 
                            uint64_t *size, 
                            void **buffer, 
                            struct adios_method_struct *method) 
{
}

// should not be called from write, reason for inclusion here unknown
void 
adios_icee_read(struct adios_file_struct *fd, 
                struct adios_var_struct *f, 
                void *buffer, 
                uint64_t buffer_size, 
                struct adios_method_struct *method) 
{
}

#else // print empty version of all functions (if HAVE_ICEE == 0)

void 
adios_icee_read(struct adios_file_struct *fd, 
                struct adios_var_struct *f, 
                void *buffer, 
                struct adios_method_struct *method) 
{
}

extern void 
adios_icee_get_write_buffer(struct adios_file_struct *fd, 
                            struct adios_var_struct *f, 
                            unsigned long long *size, 
                            void **buffer, 
                            struct adios_method_struct *method) 
{
}

extern void 
adios_icee_stop_calculation(struct adios_method_struct *method) 
{
}

extern void 
adios_icee_start_calculation(struct adios_method_struct *method) 
{
}

extern void 
adios_icee_end_iteration(struct adios_method_struct *method) 
{
}

#endif

