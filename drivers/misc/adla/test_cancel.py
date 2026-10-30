#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute actual pending/finished/scheduled cancellation with controlled lists."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parent / 'drv/common/adlak_submit.c').read_text()
functions = source[source.index('static int adlak_invoke_del_from_nosch_list('):
                   source.index('int adlak_invoke_del_all(')]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define AML_LOG_DEBUG(...) ((void)0)
#define AML_LOG_INFO(...) ((void)0)
#define ADLAK_TASK_CANCELED 1
struct adlak_task;
struct list_head { struct list_head *next, *prev; struct adlak_task *owner; };
struct adlak_context { int net_id, invoke_cnt; };
struct adlak_task { struct list_head head; struct adlak_context *context; int invoke_idx, flag; };
struct adlak_workqueue { struct list_head pending_list, finished_list, scheduled_list; int pending_num, finished_num, wq_mutex; };
struct adlak_device { struct adlak_workqueue queue; };
static void init(struct list_head *h) { h->next=h->prev=h; h->owner=NULL; }
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_del(struct list_head *h) { h->prev->next=h->next; h->next->prev=h->prev; }
static void add(struct list_head *h,struct adlak_task *t,struct adlak_context *c,int id) {
 t->context=c; c->invoke_cnt++; t->invoke_idx=id; t->head.owner=t;
 t->head.prev=h->prev; t->head.next=h; h->prev->next=&t->head; h->prev=&t->head;
}
#define list_for_each_entry_safe(p,t,h,member) \
 for(p=(h)->next->owner,t=p ? p->head.next->owner:NULL; p; \
     p=t,t=p ? p->head.next->owner:NULL)
static void adlak_invoke_destroy(struct adlak_task *t) { t->context->invoke_cnt--; }
static void adlak_os_mutex_lock(int *p) { assert(!*p); *p=1; }
static void adlak_os_mutex_unlock(int *p) { assert(*p); *p=0; }
'''
test = r'''
int main(void) {
 struct adlak_device d={0}; struct adlak_workqueue *q=&d.queue;
 struct adlak_context a={.net_id=1},b={.net_id=2};
 struct adlak_task pending_a={0},pending_b={0},finished={0},scheduled={0};
 init(&q->pending_list); init(&q->finished_list); init(&q->scheduled_list);
 add(&q->pending_list,&pending_a,&a,1); add(&q->pending_list,&pending_b,&b,2);
 add(&q->finished_list,&finished,&a,3); add(&q->scheduled_list,&scheduled,&a,4);
 q->pending_num=2; q->finished_num=1;
 assert(adlak_invoke_del_with_invokeid(&d,1,-1)==-1);
 assert(q->pending_num==1 && q->finished_num==0);
 assert(a.invoke_cnt==1 && b.invoke_cnt==1);
 assert(scheduled.flag & ADLAK_TASK_CANCELED);
 assert(!list_empty(&q->scheduled_list));
 assert(!adlak_invoke_del_with_invokeid(&d,2,999));
 assert(q->pending_num==1 && b.invoke_cnt==1);
 assert(!adlak_invoke_del_with_invokeid(&d,2,2));
 assert(!q->pending_num && !b.invoke_cnt);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(stubs + functions + test)
    subprocess.run(['cc', '-std=gnu11', '-fsanitize=address,undefined',
                    str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)

# Exercise the actual synchronous cancellation path with controlled worker events.
begin = source.index('int adlak_uninvoke_request(')
end = source.index('static int adlak_get_status_locked(', begin)
cancel_function = source[begin:end]
cancel_stubs = r'''
#include <assert.h>
#include <errno.h>
#define AML_LOG_DEBUG(...) ((void)0)
#define READ_ONCE(x) (x)
typedef int wait_queue_head_t;
struct queue { int wq_mutex; struct { int sem_irq; } dev_inference; };
struct adlak_device { int dev_mutex, recovery_error; struct queue queue; };
struct model { int invoke_count; };
struct adlak_context { struct adlak_device *padlak; struct model *pmodel_attr;
 int net_id, invoke_cnt, wait, terminal_invoke, terminal_error; };
struct adlak_network_invoke_del_desc { int net_register_idx, invoke_register_idx; };
static struct adlak_context *current;
static int active, reset_error, dma, notifications;
static void adlak_os_mutex_lock(int *p) { (void)p; }
static void adlak_os_mutex_unlock(int *p) { (void)p; }
static int adlak_invoke_del_with_invokeid(struct adlak_device *d,int n,int i) {
 (void)d; (void)n; (void)i;
 if(active) return -1;
 current->invoke_cnt=0; return 0;
}
static void worker_finish(void) {
 if(reset_error) { current->padlak->recovery_error=reset_error; return; }
 dma=0; current->invoke_cnt=0;
}
static void adlak_to_umd_sinal_give(int q) {
 (void)q; assert(!dma); notifications++;
}
#define msecs_to_jiffies(ms) (ms)
#define wait_event_timeout(q, condition, ms) \
 ((void)(ms), worker_finish(), (condition) ? 1L : 0L)
'''
cancel_test = r'''
int main(void) {
 struct adlak_device device={0}; struct model model={.invoke_count=7};
 struct adlak_context ctx={.padlak=&device,.pmodel_attr=&model,.net_id=3,.invoke_cnt=1};
 struct adlak_network_invoke_del_desc id={3,7}; current=&ctx;
 assert(!adlak_uninvoke_request(&ctx,&id));
 assert(!ctx.invoke_cnt && ctx.terminal_error==-ECANCELED && ctx.terminal_invoke==7);
 active=1; ctx.invoke_cnt=1; dma=1; ctx.terminal_error=0;
 assert(!adlak_uninvoke_request(&ctx,&id));
 assert(!dma && !ctx.invoke_cnt && notifications==2);
 ctx.invoke_cnt=1; dma=1; ctx.terminal_error=0; reset_error=-EIO;
 assert(adlak_uninvoke_request(&ctx,&id)==-EIO);
 assert(dma && ctx.invoke_cnt==1 && !ctx.terminal_error && notifications==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'cancel.c').write_text(cancel_stubs + cancel_function + cancel_test)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(root / 'cancel.c'),
                    '-o', str(root / 'cancel')], check=True)
    subprocess.run([str(root / 'cancel')], check=True)
