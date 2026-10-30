#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute real error-completion and close paths with controlled DMA/reset events."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
context = (root / 'drv/common/adlak_context.c').read_text()
close = context[context.index('int adlak_destroy_context('):
                context.index('struct context_buf *find_buffer_by_desc(')]
source = (root / 'drv/common/adlak_inference.c').read_text()
report = source[source.index('case ADLAK_INFERENCE_STATE_REPORT_TASK_CUR:'):
                source.index('            default:',
                             source.index('case ADLAK_INFERENCE_STATE_REPORT_TASK_CUR:'))]
submit = (root / 'drv/common/adlak_submit.c').read_text()
wait = submit[submit.index('static bool adlak_status_ready('):
              submit.index('int adlak_submit_patch_and_exec(')]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define ERR(x) (-(x))
#define NONE 0
#define ASSERT(x) assert(x)
#define AML_LOG_DEBUG(...) ((void)0)
#define AML_LOG_INFO(...) ((void)0)
#define AML_LOG_WARN(...) ((void)0)
#define AML_LOG_ERR(...) ((void)0)
#define trace_adla_phase(...) ((void)0)
#define clk_get_rate(...) 800000000
static unsigned long long ktime_get_ns(void) { return 0; }
#define ADLAK_INFERENCE_STATE_REPORT_TASK_CUR 1
#define ADLAK_INFERENCE_STATE_DEV_RESET 2
#define CONTEXT_STATE_CLOSED 1
struct adlak_task;
struct work_struct { int unused; };
struct adlak_workqueue { int wq_mutex, sched_num, finished_num, finished_list;
 struct adlak_task *ptask_sch_cur; struct { void *sem_irq; } dev_inference; };
struct adlak_device { int recovery_error, dev_mutex, net_id_bitmap;
 void *release_wq;
 struct adlak_workqueue queue; };
struct adlak_context {
 struct adlak_device *padlak;
 int net_id, context_mutex, state, head, terminal_error, invoke_cnt, close_deferred;
 void *ctx_idle, *sem_irq, *invoke_state, *wait;
 struct work_struct close_work;
};
struct adlak_task { struct adlak_context *context; int flag, head; };
static int dma_active, reset_error, busy, freed, notified, fail_during_wait;
#define THIS_MODULE 0
static int module_pins;
static void __module_get(int m) { (void)m; module_pins++; }
static void module_put(int m) { (void)m; assert(module_pins>0); module_pins--; }
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static struct work_struct *queued;
static void queue_work(void *queue, struct work_struct *w) {
 (void)queue; assert(!dma_active && module_pins); queued=w;
}
static struct adlak_device *current;
static int adlak_hal_reset_and_start(struct adlak_device *d) {
 (void)d; if (!reset_error) dma_active=0; return reset_error;
}
static void adlak_recovery_failed(struct adlak_device *d,int r) { d->recovery_error=r; }
static void adlak_os_mutex_lock(int *m) { (void)m; }
static void adlak_os_mutex_unlock(int *m) { (void)m; }
static void adlak_os_mutex_destroy(int *m) { (void)m; }
static int adlak_queue_update_task_state(struct adlak_device *d,struct adlak_task *t) {
 (void)d; (void)t; assert(!dma_active); notified++; return 1;
}
static void adlak_os_sema_give(void *s) { (void)s; }
static void adlak_to_umd_sinal_give(void *s) {
 (void)s; assert(!dma_active && !current->queue.ptask_sch_cur);
}
static int adlak_invoke_del_all(struct adlak_device *d,int id) { (void)d; (void)id; return busy; }
static int adlak_os_sema_take_timeout(void *s,int ms) {
 if(fail_during_wait==2) return 1;
 (void)s; (void)ms; if(fail_during_wait) current->recovery_error=-EIO;
 else dma_active=0; return 0;
}
static void adlak_mem_free_all_context(struct adlak_context *c) { (void)c; assert(!dma_active); freed++; }
static void adlak_net_dettach_by_id(struct adlak_context *c,int id) { (void)c; (void)id; }
static void adlak_to_umd_sinal_deinit(void **p) { (void)p; }
static void adlak_os_sema_destroy(void **p) { (void)p; }
static void list_del(int *p) { (void)p; }
static void list_move_tail(int *a,int *b) { (void)a; (void)b; }
static void adlak_invoke_destroy(struct adlak_task *t) { assert(!dma_active); t->context->invoke_cnt--; }
#define ADLAK_TASK_CANCELED 1
#define mb() ((void)0)
static void adlak_os_free(void *p) { (void)p; }
static void adlak_simple_bitmap_free(int *p,int id) { (void)p; (void)id; }
#define ERESTARTSYS 512
typedef int wait_queue_head_t;
struct adlak_get_stat_desc { int timeout_ms, net_register_idx, ret_state, invoke_register_idx; };
struct adlak_network_invoke_del_desc { int net_register_idx, invoke_register_idx; };
static int canceled;
static int adlak_uninvoke_request(struct adlak_context *c,
                                  struct adlak_network_invoke_del_desc *d) {
 (void)c; (void)d; canceled++; dma_active=0; return 0;
}
static int wait_mode, completed;
static int adlak_get_status_request(struct adlak_context *c,struct adlak_get_stat_desc *s) {
 (void)c; s->ret_state=completed ? (wait_mode==2 ? -1:0):1;
 return completed && wait_mode==2 ? -ETIMEDOUT:0;
}
#define msecs_to_jiffies(x) (x)
#define wait_event_interruptible_timeout(q, condition, ticks) \
 ({ long r=0; (void)(ticks); if(condition) r=1; \
    else if(wait_mode==3) r=-ERESTARTSYS; \
    else if(wait_mode) { completed=1; if(condition) r=1; } r; })
'''
deferred = context[context.index('static void adlak_release_deferred('):
                   context.index('int adlak_create_context(')]
begin = submit.index('int adlak_queue_update_task_state(')
begin = submit.index('        mb();', begin)
end = submit.index('    return ret;', begin)
publication = submit[begin:end]
publication = publication[:publication.rfind('    }')]
publish = '''
static int publish(struct adlak_device *padlak, struct adlak_task *ptask) {
 struct adlak_workqueue *pwq=&padlak->queue;
 struct adlak_context *context;
 int ret=0;
''' + publication + '''
 return ret;
}
'''
wrapper = '''
static void complete(struct adlak_device *padlak, struct adlak_context *ctx) {
 struct adlak_task task={.context=ctx}, *ptask_sch_cur=&task;
 struct adlak_workqueue queue={0}, *pwq=&queue;
 int ret, inference_state=1;
 uint64_t phase_start;
 while (1) {
  if(padlak->recovery_error) return;
  switch(inference_state) {
''' + report + '''
  default: return;
  }
 }
}
int main(void) {
 struct adlak_device device={0}; struct adlak_context ctx={0}; current=&device;
 ctx.padlak=&device;
 dma_active=1; complete(&device,&ctx); assert(notified==1 && !dma_active);
 busy=1; dma_active=1; assert(!adlak_destroy_context(&device,&ctx)); assert(freed==1);
 device.recovery_error=0; notified=0; reset_error=-EIO; dma_active=1;
 complete(&device,&ctx); assert(device.recovery_error==-EIO && !notified && dma_active);
 assert(adlak_destroy_context(&device,&ctx)==-EIO && freed==1);
 struct adlak_get_stat_desc status={.timeout_ms=100, .net_register_idx=0};
 for(wait_mode=0;wait_mode<4;wait_mode++) {
  completed=0; int r=adlak_wait_until_finished(&ctx,&status);
  assert(r==(wait_mode==0 || wait_mode==2 ? -ETIMEDOUT :
             wait_mode==3 ? -ERESTARTSYS:0));
 }
 assert(canceled==1);
 status.timeout_ms=-1;
 assert(adlak_wait_until_finished(&ctx,&status)==-EINVAL);
 device.recovery_error=0; fail_during_wait=1;
 assert(adlak_destroy_context(&device,&ctx)==-EIO && freed==1);
 device.recovery_error=0; reset_error=0; fail_during_wait=2; busy=1;
 dma_active=1; ctx.invoke_cnt=1;
 assert(adlak_destroy_context(&device,&ctx)==-ETIMEDOUT);
 assert(!device.recovery_error && ctx.close_deferred && module_pins==1 && freed==1);
 struct adlak_task late={.context=&ctx,.flag=ADLAK_TASK_CANCELED};
 device.queue.ptask_sch_cur=&late; device.queue.sched_num=1; dma_active=0;
 assert(!publish(&device,&late));
 assert(!device.queue.ptask_sch_cur && !ctx.invoke_cnt && queued==&ctx.close_work);
 assert(freed==1 && module_pins==1);
 busy=0; fail_during_wait=0; adlak_release_deferred(queued);
 assert(freed==2 && module_pins==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    (directory / 'test.c').write_text(stubs + close + deferred + publish + wait + wrapper)
    subprocess.run(['cc', '-std=gnu11', '-fsanitize=address,undefined',
                    str(directory / 'test.c'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)

poll = source[source.index('int adlak_submit_wait('):
              source.index('static void adlak_debug_print_device_state(')]
poll_stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define READ_ONCE(x) (x)
#define ADLAK_TASK_CANCELED 1
#define min_t(t,a,b) ((t)(a)<(t)(b)?(a):(b))
struct adlak_device { int spinlock; };
struct model { unsigned int hw_timeout_ms; };
struct context { struct adlak_device *padlak; struct model *pmodel_attr; };
struct adlak_task { int flag; struct context *context;
 struct { struct { int timeout; } irq_status; } hw_stat; };
struct adlak_dev_inference { int sem_irq; };
static struct adlak_task *current_task;
static int mode, calls, elapsed;
static int adlak_os_sema_take_timeout(int sem,unsigned int ms) {
 (void)sem; assert(ms<=10); calls++; elapsed+=ms;
 if(mode==1) return 0;
 if(mode==2) current_task->flag=ADLAK_TASK_CANCELED;
 return -ETIMEDOUT;
}
static void adlak_os_spinlock_lock(int *s) { (void)s; }
static void adlak_os_spinlock_unlock(int *s) { (void)s; }
static void adlak_irq_proc(struct adlak_device *d) { (void)d; }
'''
poll_test = r'''
int main(void) {
 struct adlak_device device={0}; struct model model={.hw_timeout_ms=17};
 struct context context={&device,&model};
 struct adlak_task task={.context=&context}; struct adlak_dev_inference irq={0};
 current_task=&task;
 assert(adlak_submit_wait(&irq,&task)==-ETIMEDOUT && elapsed==34);
 mode=1; calls=0; assert(!adlak_submit_wait(&irq,&task) && calls==1);
 mode=2; calls=0; elapsed=0;
 assert(adlak_submit_wait(&irq,&task)==-ECANCELED && elapsed==10);
 model.hw_timeout_ms=0; task.flag=0; elapsed=0;
 assert(adlak_submit_wait(&irq,&task)==-ECANCELED && elapsed==10);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    (directory / 'poll.c').write_text(poll_stubs + poll + poll_test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(directory / 'poll.c'),
                    '-o', str(directory / 'poll')], check=True)
    subprocess.run([str(directory / 'poll')], check=True)

# A completion must wake both WAIT (interruptible) and CANCEL (uninterruptible).
os_source = (root / 'drv/port/os/linux/adlak_os.c').read_text()
give = os_source[os_source.index('int adlak_to_umd_sinal_give('):
                 os_source.index('adlak_os_ktime_t adlak_os_ktime_get(')]
wake_test = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#define PRINT_FUNC_NAME ((void)0)
#define ERR(x) (-(x))
#define NONE 0
typedef struct { unsigned int sleeping; } wait_queue_head_t;
enum { INTERRUPTIBLE=1, UNINTERRUPTIBLE=2 };
#define wake_up_interruptible(q) ((q)->sleeping &= ~INTERRUPTIBLE)
#define wake_up_all(q) ((q)->sleeping = 0)
''' + give + r'''
int main(void) {
 wait_queue_head_t queue={INTERRUPTIBLE|UNINTERRUPTIBLE};
 assert(!adlak_to_umd_sinal_give((uintptr_t)&queue));
 assert(!queue.sleeping);
 assert(adlak_to_umd_sinal_give(0)==-EINVAL);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    (directory / 'wake.c').write_text(wake_test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(directory / 'wake.c'),
                    '-o', str(directory / 'wake')], check=True)
    subprocess.run([str(directory / 'wake')], check=True)
