#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile actual backend functions with deterministic hardware-free Zephyr fakes.

Run directly; does not change run.sh, firmware, NVS or host build directories.
The fakes assert that no driver/NVS call runs while server_lock is held.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/papermono_content_server.c").read_text()


def function(name):
    import re
    match = re.search(r"^(?:static )?(?:enum \w+|void|int|bool) " + name + r"\([^;]*?\)\n\{", SOURCE, re.M)
    if not match:
        raise AssertionError(f"missing C function: {name}")
    start = match.start()
    depth = 0
    for index in range(SOURCE.index("{", match.start()), len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:index + 1] + "\n"
    raise AssertionError(f"unclosed C function: {name}")


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <papermono/content_server.h>
#define PAPERMONO_WIFI_SSID_MAX 32
#define PAPERMONO_WIFI_PSK_MAX 64
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_SECONDS(x) ((x)*1000)
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_FREQ_BAND_2_4_GHZ 0
#define WIFI_CHANNEL_ANY 0
#define WIFI_MFP_DISABLE 0
#define WIFI_MFP_OPTIONAL 1
#define NET_REQUEST_WIFI_SCAN 1
#define NET_REQUEST_WIFI_CONNECT 2
#define NET_REQUEST_WIFI_DISCONNECT 3
#define NET_REQUEST_WIFI_AP_DISABLE 4
#define LOG_WRN(...) ((void)0)
typedef int k_timeout_t;
struct k_work { int unused; };
struct k_work_delayable { int unused; };
struct k_work_q { int unused; };
struct net_if { int unused; };
enum wifi_security_type { WIFI_SECURITY_TYPE_NONE, WIFI_SECURITY_TYPE_PSK, WIFI_SECURITY_TYPE_SAE };
struct wifi_scan_result { uint8_t ssid[33]; uint8_t ssid_length, band, channel; enum wifi_security_type security; int8_t rssi; };
struct wifi_scan_params { int scan_type; };
struct wifi_connect_req_params { const uint8_t *ssid, *psk; uint8_t ssid_length, psk_length, channel, band; enum wifi_security_type security; int timeout; int mfp; };
struct papermono_preferences { char wifi_ssid[33], wifi_psk[65]; bool wifi_configured; };
static struct papermono_preferences preferences;
static struct papermono_wifi_state wifi_state;
static struct papermono_content_server_state server_state;
static struct papermono_wifi_network scan_networks[PAPERMONO_WIFI_NETWORK_MAX];
static uint8_t scan_count;
static bool scan_inflight, initialized = true, ap_enabling;
static struct k_work scan_work;
static struct k_work_q wifi_work_queue;
static struct k_work_delayable scan_timeout_work, wifi_command_work, station_retry_work, apply_wifi_work, disable_ap_work, enable_ap_work;
static int server_lock, lock_depth, refreshes, scheduled, retries, recovery, reset_calls;
static int wifi_paused;
static bool wifi_is_paused(void) { return wifi_paused != 0; }
static int driver_result, nvs_result, writes, disconnects, scan_mode;
static int64_t now;
static struct net_if station_iface, ap_iface;
enum wifi_command { WIFI_COMMAND_NONE, WIFI_COMMAND_CONNECT, WIFI_COMMAND_SAVED, WIFI_COMMAND_FORGET, WIFI_COMMAND_RESET };
static enum wifi_command pending_wifi_command;
static uint8_t wifi_command_phase;
static int64_t wifi_command_deadline;
static char pending_wifi_ssid[33], pending_wifi_psk[65];
static void k_mutex_lock(int *m, int t) { (void)m; (void)t; assert(lock_depth == 0); ++lock_depth; }
static void k_mutex_unlock(int *m) { (void)m; assert(lock_depth == 1); --lock_depth; }
static struct net_if *net_if_get_wifi_sta(void) { return &station_iface; }
static struct net_if *net_if_get_wifi_sap(void) { return &ap_iface; }
static const struct papermono_preferences *papermono_settings_get(void) { return &preferences; }
static int papermono_settings_set_wifi(const char *ssid, const char *psk) {
 assert(!lock_depth); ++writes; if(nvs_result) return nvs_result;
 snprintf(preferences.wifi_ssid, sizeof(preferences.wifi_ssid), "%s", ssid);
 snprintf(preferences.wifi_psk, sizeof(preferences.wifi_psk), "%s", psk);
 preferences.wifi_configured = true; return 0;
}
static int papermono_settings_clear_wifi(void) {
 assert(!lock_depth); ++writes; if(nvs_result) return nvs_result;
 memset(&preferences, 0, sizeof(preferences)); return 0;
}
static void papermono_ui_request_background_refresh(void) { assert(!lock_depth); ++refreshes; }
static int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w) { assert(q==&wifi_work_queue); (void)w; ++scheduled; return 1; }
static int k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *w, int d) { assert(q==&wifi_work_queue); (void)w; (void)d; ++scheduled; return 1; }
static int k_work_cancel_delayable(struct k_work_delayable *w) { (void)w; return 0; }
static int64_t k_uptime_get(void) { return now; }
static void schedule_station_retry(void) { assert(!lock_depth); ++retries; }
static void schedule_recovery_ap(void) { assert(!lock_depth); ++recovery; }
static int reset_wifi_now(void) { assert(!lock_depth); ++reset_calls; wifi_state.connection=PAPERMONO_WIFI_DISCONNECTED; return 0; }
static void finish_scan_locked(int error);
static int net_mgmt(int request, struct net_if *iface, void *data, size_t size) {
 assert(!lock_depth); (void)iface; (void)size;
 if(request == NET_REQUEST_WIFI_DISCONNECT) ++disconnects;
 if(request == NET_REQUEST_WIFI_CONNECT) {
   struct wifi_connect_req_params *c=data;
   assert(c->ssid_length > 0 && c->ssid_length <= 32);
   assert(c->psk_length <= 63);
   assert(c->security == (c->psk_length ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE));
 }
 /* Emulate DONE synchronously inside the scan request, before return. */
 if(request == NET_REQUEST_WIFI_SCAN && scan_mode) {
   k_mutex_lock(&server_lock,0); scan_inflight=false; finish_scan_locked(0); k_mutex_unlock(&server_lock);
 }
 return driver_result;
}
'''

MAIN = r'''
static struct papermono_wifi_network candidate(void) {
 struct papermono_wifi_network n={.ssid="test-network",.ssid_length=12,.security=PAPERMONO_WIFI_SECURITY_WPA2_PSK,.supported=true}; return n;
}
static void clean(void) {
 memset(&wifi_state,0,sizeof(wifi_state)); memset(&server_state,0,sizeof(server_state));
 memset(&preferences,0,sizeof(preferences)); memset(scan_networks,0,sizeof(scan_networks));
 pending_wifi_command=WIFI_COMMAND_NONE; wifi_command_phase=0;
 scan_count=0; scan_inflight=false; ap_enabling=false; nvs_result=driver_result=scan_mode=0; now=0;
 refreshes=writes=disconnects=scheduled=reset_calls=0;
}
int main(void) {
 clean();
 struct wifi_scan_result r={.ssid_length=32,.band=0,.channel=6,.security=WIFI_SECURITY_TYPE_PSK,.rssi=-70};
 memset(r.ssid,'x',sizeof(r.ssid)); scan_add_locked(&r);
 assert(scan_count==1 && scan_networks[0].ssid[32]==0 && scan_networks[0].ssid_length==32 && scan_networks[0].supported);
 r.rssi=-30; scan_add_locked(&r); assert(scan_count==1 && scan_networks[0].rssi==-30);
 r.rssi=-90; scan_add_locked(&r); assert(scan_networks[0].rssi==-30);
 r.ssid_length=0; scan_add_locked(&r); assert(scan_networks[1].hidden && !scan_networks[1].supported);
 r.ssid_length=3; memcpy(r.ssid,"a\0b",3); scan_add_locked(&r); assert(!scan_networks[2].supported);
 r.ssid_length=2; memcpy(r.ssid,"sa",2); r.security=WIFI_SECURITY_TYPE_SAE; scan_add_locked(&r);
 assert(scan_networks[3].security==PAPERMONO_WIFI_SECURITY_UNSUPPORTED && !scan_networks[3].supported);
 clean();
 for(int i=0;i<40;++i) { r.ssid_length=2; r.ssid[0]='A'+i; r.ssid[1]='x'; r.security=WIFI_SECURITY_TYPE_NONE; r.rssi=-100+i; scan_add_locked(&r); }
 assert(scan_count==16); for(int i=1;i<16;++i) assert(scan_networks[i-1].rssi>=scan_networks[i].rssi);
 assert(scan_networks[0].rssi==-61 && scan_networks[15].rssi==-76);
 finish_scan_locked(0); assert(wifi_state.count==16 && wifi_state.scan_generation==1);
 clean(); assert(papermono_content_server_scan_wifi()==0 && wifi_state.scan_busy);
 assert(papermono_content_server_scan_wifi()==-EBUSY);
 driver_result=7; scan_wifi_worker(NULL); assert(!wifi_state.scan_busy && !scan_inflight && wifi_state.scan_error==-EIO);
 clean(); papermono_content_server_scan_wifi(); scan_mode=1; scan_wifi_worker(NULL);
 assert(!wifi_state.scan_busy && !scan_inflight && wifi_state.scan_generation==1);
 clean(); papermono_content_server_scan_wifi(); scan_timeout_worker(NULL);
 assert(wifi_state.scan_error==-ETIMEDOUT && !wifi_state.scan_busy && scan_inflight);
 assert(papermono_content_server_scan_wifi()==-EBUSY); /* quarantine late scan events */
 clean(); struct papermono_wifi_network n=candidate(); char password[]="dynamic-psk";
 assert(papermono_content_server_connect_wifi(&n,password)==0);
 assert(writes==0 && wifi_state.request_busy); memset(password,0,sizeof(password));
 assert(papermono_content_server_connect_wifi(&n,"another-psk")==-EBUSY);
 wifi_command_worker(NULL); assert(writes==1 && wifi_state.request_busy && server_state.station_connecting);
 assert(!strcmp(preferences.wifi_psk,"dynamic-psk") && pending_wifi_psk[0]==0);
 server_state.station_connecting=false; server_state.station_connected=true;
 wifi_state.connection=PAPERMONO_WIFI_CONNECTED; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.connection==PAPERMONO_WIFI_CONNECTED);
 clean(); n.security=PAPERMONO_WIFI_SECURITY_OPEN;
 assert(papermono_content_server_connect_wifi(&n,"")==0); wifi_command_worker(NULL);
 assert(preferences.wifi_psk[0]==0 && server_state.station_connecting);
 clean(); n=candidate(); n.hidden=true; assert(papermono_content_server_connect_wifi(&n,"valid-psk")==-ENOTSUP);
 n.hidden=false; assert(papermono_content_server_connect_wifi(&n,"short")==-EINVAL);
 assert(papermono_content_server_connect_wifi(&n,"1234567\n")==-EINVAL);
 char longkey[65]; memset(longkey,'a',64); longkey[64]=0; assert(papermono_content_server_connect_wifi(&n,longkey)==-EINVAL);
 clean(); server_state.station_connected=true; nvs_result=-EIO;
 assert(papermono_content_server_connect_wifi(&n,"valid-psk")==0); wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.last_connection_error==-EIO && !disconnects && server_state.station_connected);
 clean(); preferences.wifi_configured=true; strcpy(preferences.wifi_ssid,"saved"); strcpy(preferences.wifi_psk,"saved-pass");
 wifi_state.configured=true; assert(papermono_content_server_connect_saved()==0); wifi_command_worker(NULL);
 assert(writes==0 && server_state.station_connecting); now=40000; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.last_connection_error==-ETIMEDOUT);
 clean(); server_state.station_connected=true; server_state.ap_enabled=true;
 assert(papermono_content_server_forget_wifi()==0); wifi_command_worker(NULL);
 assert(wifi_state.request_busy && disconnects==1 && !reset_calls);
 server_state.station_connected=false; wifi_command_worker(NULL); assert(wifi_command_phase==3 && !reset_calls);
 server_state.ap_enabled=false; wifi_command_worker(NULL); assert(reset_calls==1 && !wifi_state.request_busy);
 clean(); ap_enabling=true;
 assert(papermono_content_server_forget_wifi()==0); wifi_command_worker(NULL);
 assert(wifi_state.request_busy && wifi_command_phase==1 && reset_calls==0);
 ap_enabling=false; server_state.ap_enabled=true; wifi_command_worker(NULL);
 assert(wifi_state.request_busy && wifi_command_phase==3 && reset_calls==0);
 server_state.ap_enabled=false; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && reset_calls==1);
 clean(); ap_enabling=true; papermono_content_server_forget_wifi(); wifi_command_worker(NULL);
 now=6000; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && reset_calls==0 && wifi_state.last_connection_error==-ETIMEDOUT);
 puts("wifi backend: scan, copy, async, lock, NVS, reconnect, forget + AP-start race PASS");
 return 0;
}
'''


LIFECYCLE_PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <papermono/content_server.h>
#define K_FOREVER 0
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define WIFI_SECURITY_TYPE_PSK 1
#define WIFI_FREQ_BAND_2_4_GHZ 0
#define NET_REQUEST_WIFI_AP_ENABLE 1
#define NET_REQUEST_WIFI_AP_DISABLE 2
#define NET_REQUEST_WIFI_IFACE_STATUS 3
#define WIFI_MODE_AP 1
#define NET_EVENT_WIFI_SCAN_RESULT 10
#define NET_EVENT_WIFI_SCAN_DONE 11
#define NET_EVENT_WIFI_AP_ENABLE_RESULT 12
#define NET_EVENT_WIFI_AP_DISABLE_RESULT 13
#define NET_EVENT_WIFI_CONNECT_RESULT 14
#define NET_EVENT_WIFI_DISCONNECT_RESULT 15
typedef int k_timeout_t;
struct k_work { int unused; };
struct k_work_delayable { bool pending; int64_t due; };
struct k_work_q { int unused; };
struct net_if { int unused; };
struct wifi_status { int status; };
struct wifi_scan_result { int unused; };
struct wifi_iface_status { int iface_mode; uint8_t channel; size_t ssid_len; char ssid[33]; };
struct net_mgmt_event_callback { void *info; size_t info_length; };
struct wifi_connect_req_params { const uint8_t *ssid, *psk; size_t ssid_length, psk_length; int security, channel, band; };
struct papermono_preferences { bool wifi_configured; };
static struct papermono_preferences preferences;
static struct papermono_wifi_state wifi_state;
static struct papermono_content_server_state server_state;
static struct net_if sta, ap;
static struct k_work_q wifi_work_queue;
static struct k_work_delayable enable_ap_work, disable_ap_work, ap_health_work,
 station_retry_work, scan_timeout_work;
static int server_lock, lock_depth, ap_starts, ap_stops, refreshes, normalized, status_queries;
static bool paused, ap_enabling, scan_inflight, connect_during_status, associate_during_status;
static struct wifi_iface_status reported_ap;
static uint8_t ap_retry_shift, station_retry_shift, ap_health_failures, wifi_command_phase;
static int64_t now;
static void k_mutex_lock(int *mutex, int timeout) {
 (void)mutex; (void)timeout; assert(!lock_depth); ++lock_depth;
}
static void k_mutex_unlock(int *mutex) { (void)mutex; assert(lock_depth==1); --lock_depth; }
static bool wifi_is_paused(void) { return paused; }
static struct net_if *net_if_get_wifi_sta(void) { return &sta; }
static struct net_if *net_if_get_wifi_sap(void) { return &ap; }
static const struct papermono_preferences *papermono_settings_get(void) { return &preferences; }
static int k_work_reschedule_for_queue(struct k_work_q *queue,
 struct k_work_delayable *work, int delay) {
 assert(queue==&wifi_work_queue); work->pending=true; work->due=now+delay; return 0;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) { work->pending=false; return 0; }
static int configure_ap_address(struct net_if *iface) { assert(iface==&ap && !lock_depth); return 0; }
static void normalize_provisioning_radio(bool ap_only) { (void)ap_only; assert(!lock_depth); ++normalized; }
static void wifi_event(struct net_mgmt_event_callback *callback, uint64_t event, struct net_if *iface);
static int net_mgmt(int request, struct net_if *iface, void *data, size_t size) {
 assert(!lock_depth && iface==&ap); (void)size;
 if(request==NET_REQUEST_WIFI_AP_ENABLE) {
  const struct wifi_connect_req_params *config=data;
  assert(config->channel==AP_CHANNEL); ++ap_starts;
 } else if(request==NET_REQUEST_WIFI_IFACE_STATUS) {
  *(struct wifi_iface_status *)data=reported_ap; ++status_queries;
  if(connect_during_status) {
   connect_during_status=false;
   struct wifi_status result={0}; struct net_mgmt_event_callback callback={&result,sizeof(result)};
   wifi_event(&callback,NET_EVENT_WIFI_CONNECT_RESULT,&sta);
  }
  if(associate_during_status) { associate_during_status=false; server_state.station_connecting=true; }
 } else { assert(request==NET_REQUEST_WIFI_AP_DISABLE); ++ap_stops; }
 return 0;
}
static int start_ap_dhcp(struct net_if *iface) { assert(iface==&ap && !lock_depth); return 0; }
static int net_dhcpv4_server_stop(struct net_if *iface) { assert(iface==&ap); return 0; }
static void scan_add_locked(const void *result) { (void)result; assert(false); }
static void finish_scan_locked(int error) { (void)error; assert(false); }
static void papermono_ui_request_background_refresh(void) { assert(!lock_depth); ++refreshes; }
'''

LIFECYCLE_MAIN = r'''
static void clean(void) {
 memset(&wifi_state,0,sizeof(wifi_state)); memset(&server_state,0,sizeof(server_state));
 memset(&enable_ap_work,0,sizeof(enable_ap_work)); memset(&disable_ap_work,0,sizeof(disable_ap_work));
 memset(&ap_health_work,0,sizeof(ap_health_work)); memset(&station_retry_work,0,sizeof(station_retry_work));
 memset(&scan_timeout_work,0,sizeof(scan_timeout_work));
 preferences.wifi_configured=true; paused=ap_enabling=scan_inflight=false;
 connect_during_status=associate_during_status=false;
 ap_retry_shift=station_retry_shift=ap_health_failures=wifi_command_phase=0;
 ap_starts=ap_stops=refreshes=normalized=status_queries=0; now=0; assert(!lock_depth);
 snprintf(server_state.ap_ssid,sizeof(server_state.ap_ssid),"fixture-ap");
 reported_ap=(struct wifi_iface_status){.iface_mode=WIFI_MODE_AP,.channel=AP_CHANNEL};
 snprintf(reported_ap.ssid,sizeof(reported_ap.ssid),"fixture-ap"); reported_ap.ssid_len=strlen(reported_ap.ssid);
}
static void event(uint64_t kind, int result) {
 struct wifi_status status={result};
 struct net_mgmt_event_callback callback={&status,sizeof(status)};
 const bool is_ap=kind==NET_EVENT_WIFI_AP_ENABLE_RESULT || kind==NET_EVENT_WIFI_AP_DISABLE_RESULT;
 wifi_event(&callback,kind,is_ap?&ap:&sta);
}
static void run(struct k_work_delayable *work) {
 assert(work->pending); now=work->due; work->pending=false;
 if(work==&enable_ap_work) enable_ap_worker(NULL);
 else if(work==&ap_health_work) verify_ap_worker(NULL);
 else { assert(work==&disable_ap_work); disable_ap(NULL); }
}
int main(void) {
 /* Router unreachable: the real failed-connect event still queues recovery
  * on the private Wi-Fi queue and the normal worker starts the setup AP. */
 clean(); event(NET_EVENT_WIFI_CONNECT_RESULT,-EIO);
 assert(enable_ap_work.pending && station_retry_work.pending);
 assert(enable_ap_work.due==AP_RETRY_INITIAL_MS);
 run(&enable_ap_work); assert(ap_starts==1 && ap_enabling && normalized==1);
 event(NET_EVENT_WIFI_AP_ENABLE_RESULT,0);
 assert(server_state.ap_enabled && !ap_enabling && ap_health_work.pending);

 /* STA recovers before the AP job runs: cancel the queued job, and also
  * reject a callback already dequeued when cancellation raced it. */
 clean(); schedule_recovery_ap(); assert(enable_ap_work.pending);
 event(NET_EVENT_WIFI_CONNECT_RESULT,0);
 assert(server_state.station_connected && !enable_ap_work.pending);
 enable_ap_worker(NULL); assert(!ap_starts && !ap_enabling);

 /* AP startup can already be in flight when STA finishes. The later AP
  * success event must supply the handoff the STA event could not schedule. */
 clean(); enable_ap_worker(NULL); assert(ap_enabling && !server_state.ap_enabled);
 event(NET_EVENT_WIFI_CONNECT_RESULT,0);
 assert(server_state.station_connected && !disable_ap_work.pending);
 event(NET_EVENT_WIFI_AP_ENABLE_RESULT,0);
 assert(server_state.ap_enabled && !ap_enabling && disable_ap_work.pending);
 assert(disable_ap_work.due==AP_HANDOFF_DELAY_MS && !ap_health_work.pending);
 run(&disable_ap_work); assert(ap_stops==1);
 event(NET_EVENT_WIFI_AP_DISABLE_RESULT,0);
 assert(!server_state.ap_enabled && !enable_ap_work.pending);

 /* The same late AP success after a failed STA attempt keeps provisioning
  * and verifies its radio instead of scheduling a handoff. */
 clean(); enable_ap_worker(NULL); assert(ap_enabling);
 event(NET_EVENT_WIFI_CONNECT_RESULT,-EIO);
 event(NET_EVENT_WIFI_AP_ENABLE_RESULT,0);
 assert(server_state.ap_enabled && ap_health_work.pending && !disable_ap_work.pending);
 run(&ap_health_work);
 assert(server_state.ap_verified && !ap_stops && !disable_ap_work.pending);

 /* Recovery must also wait for an association attempt to settle, then a
  * failed-connect event restores the initial setup path normally. */
 clean(); schedule_recovery_ap(); server_state.station_connecting=true;
 run(&enable_ap_work); assert(!ap_starts && !ap_enabling);
 schedule_recovery_ap(); assert(!enable_ap_work.pending);
 event(NET_EVENT_WIFI_CONNECT_RESULT,-EIO);
 assert(enable_ap_work.pending); run(&enable_ap_work); assert(ap_starts==1);

 /* The reservation guard also sees a Wi-Fi command started after queuing.
  * Explicit reset/startup calls keep their separate command semantics. */
 clean(); schedule_recovery_ap(); wifi_state.request_busy=true;
 run(&enable_ap_work); assert(!ap_starts && !ap_enabling);
 assert(enable_ap(false)==0 && ap_starts==1 && ap_enabling);

 /* A queued AP handoff must not tear down the only live network once STA
  * disconnects, including a callback cancellation could not recall. */
 clean(); server_state.ap_enabled=true;
 event(NET_EVENT_WIFI_CONNECT_RESULT,0);
 assert(disable_ap_work.pending && disable_ap_work.due==AP_HANDOFF_DELAY_MS);
 event(NET_EVENT_WIFI_DISCONNECT_RESULT,-ECONNRESET);
 assert(!disable_ap_work.pending && !server_state.station_connected);
 assert(server_state.ap_enabled && station_retry_work.pending && !enable_ap_work.pending);
 disable_ap(NULL); assert(!ap_stops && server_state.ap_enabled);

 /* A healthy STA still completes normal handoff, without scheduling an AP
  * restart on the subsequent real AP_STOP notification. */
 clean(); server_state.ap_enabled=true;
 event(NET_EVENT_WIFI_CONNECT_RESULT,0); run(&disable_ap_work);
 assert(ap_stops==1); event(NET_EVENT_WIFI_AP_DISABLE_RESULT,0);
 assert(!server_state.ap_enabled && !enable_ap_work.pending);

 clean(); server_state.ap_enabled=true; event(NET_EVENT_WIFI_CONNECT_RESULT,0);
 wifi_state.request_busy=true; run(&disable_ap_work);
 assert(!ap_stops && server_state.ap_enabled);

 /* APSTA may use the router's channel until the planned handoff. A good
  * STA cancels health work and even a dequeued stale check cannot restart AP. */
 clean(); server_state.ap_enabled=true; reported_ap.channel=11;
 wifi_reschedule(&ap_health_work,K_MSEC(AP_HEALTH_DELAY_MS));
 event(NET_EVENT_WIFI_CONNECT_RESULT,0); assert(!ap_health_work.pending);
 for(unsigned i=0U;i<AP_HEALTH_MAX_FAILURES;++i) verify_ap_worker(NULL);
 assert(!status_queries && !ap_stops && !ap_health_failures && !ap_health_work.pending);
 assert(disable_ap_work.pending);

 /* While association is in progress, defer the channel check and its
  * failure counter. Once association fails, verification/recovery resumes. */
 clean(); server_state.ap_enabled=true; server_state.station_connecting=true;
 reported_ap.channel=11; ap_health_failures=AP_HEALTH_MAX_FAILURES-1U;
 verify_ap_worker(NULL);
 assert(ap_health_work.pending && ap_health_work.due==AP_HEALTH_DELAY_MS);
 assert(!status_queries && !ap_stops && ap_health_failures==AP_HEALTH_MAX_FAILURES-1U);
 event(NET_EVENT_WIFI_CONNECT_RESULT,-EIO); run(&ap_health_work);
 assert(status_queries==1 && ap_stops==1);
 event(NET_EVENT_WIFI_AP_DISABLE_RESULT,0);
 assert(enable_ap_work.pending); run(&enable_ap_work); assert(ap_starts==1);

 /* An association/result can arrive during the driver status query. The
  * second state check must discard that stale unhealthy status as well. */
 clean(); server_state.ap_enabled=true; reported_ap.channel=11;
 ap_health_failures=AP_HEALTH_MAX_FAILURES-1U; connect_during_status=true;
 verify_ap_worker(NULL);
 assert(status_queries==1 && !ap_stops && !ap_health_work.pending);
 assert(ap_health_failures==AP_HEALTH_MAX_FAILURES-1U && disable_ap_work.pending);
 clean(); server_state.ap_enabled=true; reported_ap.channel=11;
 ap_health_failures=AP_HEALTH_MAX_FAILURES-1U; associate_during_status=true;
 verify_ap_worker(NULL);
 assert(status_queries==1 && !ap_stops && ap_health_work.pending);
 assert(ap_health_failures==AP_HEALTH_MAX_FAILURES-1U);

 clean(); server_state.ap_enabled=true; verify_ap_worker(NULL);
 assert(server_state.ap_verified && !ap_stops && !ap_health_work.pending);

 /* Paused radio work remains inert in both directions. */
 clean(); schedule_recovery_ap(); paused=true; run(&enable_ap_work);
 assert(!ap_starts && !ap_enabling);
 clean(); server_state.ap_enabled=true; event(NET_EVENT_WIFI_CONNECT_RESULT,0);
 paused=true; run(&disable_ap_work); assert(!ap_stops);
 puts("wifi lifecycle: queued recovery/handoff cancellation, execution guards, failed-router recovery and pause: PASS");
 return 0;
}
'''


class WifiBackendTests(unittest.TestCase):
    def test_actual_ap_lifecycle(self):
        constants = ("AP_CHANNEL", "AP_RETRY_INITIAL_MS", "AP_RETRY_MAX_MS", "AP_RETRY_MAX_SHIFT",
                     "STA_RETRY_INITIAL_MS", "STA_RETRY_MAX_MS", "STA_RETRY_MAX_SHIFT",
                     "AP_HANDOFF_DELAY_MS", "AP_HEALTH_DELAY_MS", "AP_HEALTH_MAX_FAILURES")
        defines = "\n".join(re.search(r"^#define " + name + r" .+$", SOURCE, re.M).group()
                            for name in constants) + "\n"
        names = ("wifi_reschedule", "schedule_station_retry", "enable_ap", "schedule_recovery_ap",
                 "enable_ap_worker", "verify_ap_worker", "disable_ap", "wifi_event")
        code = defines + LIFECYCLE_PREFIX + "\n".join(function(name) for name in names) + LIFECYCLE_MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-wifi-lifecycle-") as directory:
            source = Path(directory) / "lifecycle.c"
            executable = Path(directory) / "lifecycle-test"
            source.write_text(code)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "include"), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_actual_c_backend(self):
        names = ["wifi_reschedule", "wipe_wifi_secret", "connect_station", "scan_security",
                 "scan_add_locked", "finish_scan_locked", "scan_wifi_worker", "scan_timeout_worker",
                 "wifi_command_complete", "wifi_command_worker", "papermono_content_server_get_wifi_state",
                 "papermono_content_server_scan_wifi", "submit_wifi_command_locked",
                 "papermono_content_server_connect_wifi", "papermono_content_server_connect_saved",
                 "papermono_content_server_forget_wifi"]
        code = PREFIX + "\n".join(function(name) for name in names) + MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-wifi-") as directory:
            source = Path(directory) / "wifi.c"
            executable = Path(directory) / "wifi-test"
            source.write_text(code)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-variable", "-I", str(ROOT / "include"), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_no_system_queue_or_direct_http_nvs(self):
        # Wi-Fi transitions and NVS must use their dedicated queue. The RSS
        # deadline is the only system-queue exception: its callback only checks
        # short state and shuts down a socket, leaving cleanup to HTTP ABORT.
        system_calls = re.finditer(
            r"\bk_work_(?:reschedule|schedule|submit)\s*\(\s*([^,)]+)", SOURCE)
        for call in system_calls:
            with self.subTest(system_work=call.group(1).strip()):
                self.assertEqual(call.group(1).strip(), "&rss_request_timeout",
                                 "Wi-Fi work must stay on wifi_work_queue")
        self.assertIn("k_work_reschedule_for_queue(&wifi_work_queue", function("wifi_reschedule"))
        deadline = function("rss_request_timeout_worker")
        self.assertIn("K_NO_WAIT", deadline)
        self.assertNotIn("K_FOREVER", deadline)
        for forbidden in ("papermono_settings_", "net_mgmt(", "papermono_content_store_",
                          "papermono_ui_", "papermono_rss_sources_replace", "k_msleep("):
            self.assertNotIn(forbidden, deadline)
        self.assertNotIn("papermono_settings_set_wifi", function("wifi_handler"))
        self.assertNotIn("net_mgmt(", function("papermono_content_server_connect_wifi"))
        self.assertNotIn("net_mgmt(", function("papermono_content_server_scan_wifi"))
        self.assertNotIn("psk", (ROOT / "include/papermono/content_server.h").read_text().split("struct papermono_wifi_state {")[1].split("};")[0])

    def test_event_integrity_and_boot(self):
        events = function("wifi_event")
        self.assertIn("callback->info_length >= sizeof(struct wifi_scan_result)", events)
        self.assertIn("iface != net_if_get_wifi_sta()", events)
        self.assertIn("wifi_state.scan_busy", events)
        self.assertIn("const bool intentional", events)
        boot = function("papermono_content_server_init")
        self.assertNotIn("ret = connect_station()", boot)
        self.assertIn("wifi_reschedule(&apply_wifi_work", boot)


if __name__ == "__main__":
    unittest.main()
