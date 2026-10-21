#!/usr/bin/env python3
"""Run the production HID callbacks against a one-slot connection pool."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

STUB = r"""
#ifndef BLE_TEST_H
#define BLE_TEST_H
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define __packed __attribute__((packed))
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define ARG_UNUSED(x) (void)(x)
#define LOG_MODULE_REGISTER(...)
static inline void test_log(const char *format, ...) {(void)format;}
#define LOG_INF(...) test_log(__VA_ARGS__)
#define LOG_WRN(...) test_log(__VA_ARGS__)
#define CONFIG_BT_DEVICE_NAME "test"
#define K_FOREVER 0
#define K_NO_WAIT 0
struct device;
struct k_mutex { int unused; };
#define K_MUTEX_DEFINE(n) struct k_mutex n
static inline void k_mutex_lock(struct k_mutex *m, int t) {(void)m;(void)t;}
static inline void k_mutex_unlock(struct k_mutex *m) {(void)m;}
static inline void k_msleep(int t) {(void)t;}
struct k_work { void (*handler)(struct k_work *); };
static struct k_work *queued;
static uint32_t queued_due;
struct k_work_delayable {struct k_work work;};
#define K_WORK_DELAYABLE_DEFINE(n,f) struct k_work_delayable n = {.work={.handler=f}}
#define K_MSEC(v) (v)
static uint32_t mock_uptime=100;
static inline uint32_t k_uptime_get_32(void) {return mock_uptime;}
static inline int k_work_schedule(struct k_work_delayable *w,int t) {
 if(queued==&w->work)return 0;
 assert(queued==NULL);queued=&w->work;queued_due=mock_uptime+t;return 1;
}
static inline int k_work_reschedule(struct k_work_delayable *w,int t) {
 assert(queued==NULL || queued==&w->work);
 queued=&w->work;queued_due=mock_uptime+t;return 1;
}
#define K_WORK_DEFINE(n, f) struct k_work n = {.handler=f}
static inline int k_work_submit(struct k_work *w) {queued=w;queued_due=mock_uptime;return 1;}
static void drain_work(void) {
 if(!queued || (int32_t)(mock_uptime-queued_due)<0)return;
 struct k_work *w=queued;queued=NULL;if(w)w->handler(w);
}
static void advance_work(void) {assert(queued);mock_uptime=queued_due;drain_work();}
struct bt_conn { int refs; };
static struct bt_conn slot;
typedef uint8_t bt_security_t;
typedef int bt_addr_le_t;
enum bt_security_err { BT_SECURITY_ERR_SUCCESS };
#define BT_SECURITY_L2 2
#define BT_SECURITY_L3 3
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_ID_DEFAULT 0
#define BT_ADDR_LE_ANY NULL
struct bt_conn_cb {
 void (*connected)(struct bt_conn *, uint8_t);
 void (*disconnected)(struct bt_conn *, uint8_t);
 void (*recycled)(void);
 void (*security_changed)(struct bt_conn *, bt_security_t, enum bt_security_err);
 void (*le_param_updated)(struct bt_conn *, uint16_t, uint16_t, uint16_t);
};
#define BT_CONN_CB_DEFINE(n) struct bt_conn_cb n
struct bt_conn_auth_cb {
 void (*passkey_display)(struct bt_conn *, unsigned int);
 void (*cancel)(struct bt_conn *);
};
struct bt_conn_auth_info_cb {
 void (*pairing_complete)(struct bt_conn *, bool);
 void (*pairing_failed)(struct bt_conn *, enum bt_security_err);
 void (*bond_deleted)(uint8_t, const bt_addr_le_t *);
};
struct bt_bond_info {int unused;};
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) {c->refs++;return c;}
static inline void bt_conn_unref(struct bt_conn *c) {assert(c->refs>0);c->refs--;}
static inline int bt_conn_set_security(struct bt_conn *c, int l) {(void)c;(void)l;return 0;}
static inline int bt_conn_disconnect(struct bt_conn *c, int r) {(void)c;(void)r;return 0;}
static inline int bt_conn_auth_cb_register(void *p) {(void)p;return 0;}
static inline int bt_conn_auth_info_cb_register(void *p) {(void)p;return 0;}
static inline int bt_enable(void *p) {(void)p;return 0;}
static inline int settings_load_subtree(const char *p) {(void)p;return 0;}
static inline int bt_unpair(int i, const void *p) {(void)i;(void)p;return 0;}
static inline void bt_foreach_bond(int i, void (*cb)(const struct bt_bond_info *,void *),void *p) {(void)i;(void)cb;(void)p;}
struct bt_gatt_attr {void *user_data;};
#define BT_GATT_SERVICE_DEFINE(n, ...) struct {struct bt_gatt_attr attrs[20];} n
#define BT_GATT_ERR(e) (-(e))
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 1
#define BT_ATT_ERR_VALUE_NOT_ALLOWED 2
#define BT_GATT_CCC_NOTIFY 1
static inline ssize_t bt_gatt_attr_read(struct bt_conn *c,const struct bt_gatt_attr *a,void *b,uint16_t l,uint16_t o,const void *v,uint16_t s) {(void)c;(void)a;(void)b;(void)l;(void)o;(void)v;return s;}
static inline bool bt_gatt_is_subscribed(struct bt_conn *c,const struct bt_gatt_attr *a,int v) {(void)c;(void)a;(void)v;return true;}
static int notify_result, notify_calls;
static uint8_t notified[8];
static uint32_t notify_times[64];
static void (*notify_hook)(void);
static inline int bt_gatt_notify(struct bt_conn *c,const struct bt_gatt_attr *a,const void *p,size_t l) {
 (void)c;(void)a;assert(notify_calls<64);notify_times[notify_calls++]=mock_uptime;
 assert(l<=sizeof(notified));__builtin_memcpy(notified,p,l);
 if(notify_hook){void (*hook)(void)=notify_hook;notify_hook=NULL;hook();}
 return notify_result;
}
static inline int bt_bas_set_battery_level(uint8_t p) {(void)p;return 0;}
#define sys_cpu_to_le16(v) (v)
struct bt_data {int unused;};
#define BT_DATA_BYTES(...) {0}
#define BT_DATA(...) {0}
#define BT_LE_ADV_CONN_FAST_1 NULL
static int adv_calls;
static inline int bt_le_adv_start(const void *p,const struct bt_data *a,size_t n,const struct bt_data *s,size_t m) {
 (void)p;(void)a;(void)n;(void)s;(void)m;
 adv_calls++;return slot.refs ? -ENOMEM : 0;
}
#endif
"""

TEST = r"""
#include "papermono_hid.c"
void papermono_ui_request_background_refresh(void) {}
static void reset(void) {
 memset(&hid_state,0,sizeof(hid_state));hid_state.initialized=true;
 active_connection=NULL;slot.refs=0;queued=NULL;adv_calls=0;
 mouse_enabled=false;clear_mouse_locked();notify_calls=0;notify_result=0;
 mouse_clock_valid=false;mock_uptime=100;queued_due=0;notify_hook=NULL;
}
static void connect_slot(void) {
 slot.refs=1;connection_callbacks.connected(&slot,0);
 assert(hid_state.connected && slot.refs==2);
}
static void preempt_with_sample(void) {papermono_hid_mouse_move(3,0,0);}
int main(void) {
 reset();connect_slot();
 connection_callbacks.disconnected(&slot,0x13);
 assert(slot.refs==1 && !hid_state.connected && !hid_state.advertising);
 assert(adv_calls==0 && queued==NULL); /* stack still owns the slot */
 bt_conn_unref(&slot);connection_callbacks.recycled();
 assert(adv_calls==0); /* HCI command must run from queued work */
 drain_work();assert(adv_calls==1 && hid_state.advertising);
 connection_callbacks.recycled();drain_work();assert(adv_calls==1);

 reset();slot.refs=1;connection_callbacks.connected(&slot,0x3e);
 assert(adv_calls==0);bt_conn_unref(&slot);
 connection_callbacks.recycled();drain_work();assert(hid_state.advertising);
 assert(hid_state.last_error==0);

 reset();connect_slot();assert(papermono_hid_clear_bonds()==0);
 drain_work();assert(adv_calls==0); /* disconnect is asynchronous */
 connection_callbacks.disconnected(&slot,0x13);bt_conn_unref(&slot);
 connection_callbacks.recycled();drain_work();assert(hid_state.advertising);

 reset();connection_callbacks.recycled();connect_slot();
 drain_work();assert(adv_calls==0); /* stale queued restart after new link */

 reset();hid_state.initialized=false;
 connection_callbacks.recycled();drain_work();assert(adv_calls==0);

 reset();slot.refs=1;connection_callbacks.recycled();drain_work();
 assert(adv_calls==1 && hid_state.last_error==-ENOMEM && !hid_state.advertising);
 bt_conn_unref(&slot);connection_callbacks.recycled();drain_work();
 assert(adv_calls==2 && hid_state.last_error==0 && hid_state.advertising);

 reset();connect_slot();hid_state.secured=true;papermono_hid_mouse_enable(true);
 papermono_hid_mouse_move(12,5,0);papermono_hid_mouse_move(7,-2,0);
 assert(notify_calls==0 && queued_due==mock_uptime);drain_work();
 assert(notify_calls==1 && (int8_t)notified[1]==19 && (int8_t)notified[2]==3);
 mock_uptime=108;papermono_hid_mouse_move(400,-300,0);drain_work();
 assert(notify_calls==1 && queued_due==115);advance_work();
 assert((int8_t)notified[1]==127 && (int8_t)notified[2]==-127);
 advance_work();assert((int8_t)notified[1]==127);
 papermono_hid_mouse_enable(false);int calls=notify_calls;
 advance_work();assert(notify_calls==calls); /* no backlog after page exit */

 papermono_hid_mouse_enable(true);notify_result=-ENOMEM;
 papermono_hid_mouse_move(20,4,0);drain_work();assert(mouse_x==20);
 notify_result=0;advance_work();assert((int8_t)notified[1]==20 && mouse_x==0);
 papermono_hid_mouse_move(30,0,0);calls=notify_calls;mock_uptime+=101;
 drain_work();assert(notify_calls==calls && mouse_x==0); /* discard stale motion */
 papermono_hid_mouse_move(10,0,0);
 connection_callbacks.disconnected(&slot,0x13);bt_conn_unref(&slot);
 connect_slot();hid_state.secured=true;calls=notify_calls;
 drain_work();assert(notify_calls==calls); /* no move crosses a reconnect */

 reset();connect_slot();hid_state.secured=true;papermono_hid_mouse_enable(true);
 papermono_hid_mouse_move(1,0,0);drain_work();assert(notify_times[0]==100);
 mock_uptime=108;papermono_hid_mouse_move(1,0,0);assert(queued_due==115);
 mock_uptime=114;papermono_hid_mouse_move(2,0,0);assert(queued_due==115);
 drain_work();assert(notify_calls==1);advance_work();
 assert(notify_calls==2 && notified[1]==3 && notify_times[1]-notify_times[0]==15);
 mock_uptime=200;papermono_hid_mouse_move(5,0,0);
 assert(queued_due==200);drain_work();assert(notify_times[2]==200);

 reset();connect_slot();hid_state.secured=true;papermono_hid_mouse_enable(true);
 notify_hook=preempt_with_sample;papermono_hid_mouse_move(5,0,0);drain_work();
 assert(notify_calls==1 && queued_due==115); /* slot reserved before notify */
 queued_due=mock_uptime;drain_work(); /* an already queued early wakeup */
 assert(notify_calls==1 && queued_due==115);advance_work();
 assert(notify_calls==2 && notified[1]==3 && notify_times[1]-notify_times[0]==15);

 reset();connect_slot();hid_state.secured=true;papermono_hid_mouse_enable(true);
 mock_uptime=UINT32_MAX-4U;papermono_hid_mouse_move(5,0,0);drain_work();
 mock_uptime=3;papermono_hid_mouse_move(5,0,0);assert(queued_due==10);
 drain_work();assert(notify_calls==1);advance_work();
 assert(notify_calls==2 && (uint32_t)(notify_times[1]-notify_times[0])==15U);

 reset();connect_slot();hid_state.secured=true;papermono_hid_mouse_enable(true);
 papermono_hid_mouse_move(5,0,0);drain_work();
 mock_uptime=108;papermono_hid_mouse_move(4,0,0);assert(queued_due==115);
 papermono_hid_mouse_enable(false);papermono_hid_mouse_enable(true);
 mock_uptime=109;papermono_hid_mouse_move(2,0,0);assert(queued_due==109);
 drain_work();assert(notify_calls==2 && notified[1]==2);
 return 0;
}
"""


class HidLifecycleTest(unittest.TestCase):
    def test_connection_pool_recovery(self):
        with tempfile.TemporaryDirectory(prefix="papermono-hid-") as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ble_test.h").write_text(STUB)
            headers = ["kernel.h", "device.h", "bluetooth/bluetooth.h",
                       "bluetooth/conn.h", "bluetooth/gatt.h", "bluetooth/hci.h",
                       "bluetooth/services/bas.h", "bluetooth/uuid.h",
                       "logging/log.h", "settings/settings.h", "sys/byteorder.h"]
            for name in headers:
                path = tmp / "zephyr" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "ble_test.h"\n')
            source = tmp / "test.c"
            source.write_text(TEST)
            subprocess.run(["cc", "-std=gnu17", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-Wno-unused-const-variable",
                            f"-I{tmp}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}",
                            str(source), str(ROOT / "src/papermono_hid_reports.c"),
                            "-o", str(tmp / "test")], check=True)
            subprocess.run([str(tmp / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
